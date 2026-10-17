#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "common/file_digest.h"
#include "common/sha256.h"
#include "node/proto/control_protocol.h"
#include "node/proto/release_metadata.h"
#include "node/runtime/binary_install.h"
#include "node/runtime/upgrade_candidate.h"
#include "node/runtime/upgrade_record.h"

namespace flexedge::node {

struct self_updater_config final {
    std::filesystem::path binary_path_;
    std::string current_version_;
    std::string current_sha256_{};
    std::filesystem::path upgrade_record_path_;
    std::function<bool(std::string_view, std::string_view, std::string_view)> node_log_;
    std::function<void()> request_restart_;
};

// The control channel owns this object. Release bytes never leave the authenticated
// WebSocket protocol: control_channel asks for one bounded chunk, writes it, then asks for
// the next offset. A disconnected transfer is discarded and starts from offset zero after
// the next authenticated control session.
class self_updater final {
public:
    explicit self_updater(self_updater_config config)
        : config_(std::move(config)),
          current_digest_(config_.current_sha256_.empty()
                              ? flexedge::crypto::file_sha256(config_.binary_path_)
                              : std::move(config_.current_sha256_)) {
        validate_config();
        startup_upgrade_ = read_pending_upgrade_record(config_.upgrade_record_path_, current_digest_);
    }

    self_updater(const self_updater&) = delete;
    self_updater& operator=(const self_updater&) = delete;

    ~self_updater() {
        abort();
    }

    [[nodiscard]] bool startup_pending() const noexcept {
        return startup_upgrade_.has_value() && !startup_confirmed_;
    }

    void confirm_startup() {
        if (startup_upgrade_ && !startup_confirmed_) {
            std::cerr << "flexedge node " << startup_upgrade_->message() << '\n';
            (void)log("info", startup_upgrade_->message());
        }
        startup_confirmed_ = true;
        std::error_code error;
        if (startup_upgrade_) {
            std::filesystem::remove(config_.upgrade_record_path_, error);
            if (error) {
                (void)log("warning", "could not confirm node upgrade on disk; previous binary retained");
                return;
            }
        }
        std::filesystem::remove(previous_node_binary_path(config_.binary_path_), error);
        if (error) {
            (void)log("warning", "could not retire previous node binary");
            return;
        }
        startup_upgrade_.reset();
    }

    [[nodiscard]] bool update_available(std::string_view digest) const noexcept {
        return flexedge::crypto::is_sha256_digest(digest) && digest != current_digest_;
    }

    void begin(std::string_view version, std::string_view digest, std::uint64_t total_bytes) {
        if (transfer_) {
            throw std::logic_error("node release transfer is already in progress");
        }
        if (!update_available(digest) || !valid_node_release_version(version) || total_bytes == 0 ||
            total_bytes > maximum_node_release_bytes) {
            throw std::runtime_error("control plane sent invalid node release metadata");
        }

        transfer value{
            .version_ = std::string(version),
            .digest_ = std::string(digest),
            .total_bytes_ = total_bytes,
            .candidate_ = node_upgrade_candidate_path(config_.binary_path_),
        };
        value.output_.open(value.candidate_, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!value.output_) {
            throw std::runtime_error("could not create node upgrade candidate");
        }
        transfer_.emplace(std::move(value));
    }

    void append(std::uint64_t offset, std::string_view bytes) {
        if (!transfer_) {
            throw std::logic_error("node release transfer is not in progress");
        }
        auto& value = *transfer_;
        if (offset != value.written_bytes_ || bytes.empty() ||
            bytes.size() > node_release_chunk_bytes ||
            static_cast<std::uint64_t>(bytes.size()) > value.total_bytes_ - value.written_bytes_) {
            throw std::runtime_error("control plane sent an invalid node release chunk");
        }
        value.output_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!value.output_) {
            throw std::runtime_error("could not write node upgrade candidate");
        }
        value.written_bytes_ += static_cast<std::uint64_t>(bytes.size());
    }

    [[nodiscard]] bool accept_chunk(std::string_view expected_digest,
        const v2::NodeReleaseChunk& chunk) {
        const auto expected_offset = transfer_ ? transfer_->written_bytes_ : std::uint64_t{0};
        if (!valid_node_release_version(chunk.version()) ||
            chunk.digest_sha256() != expected_digest || chunk.total_bytes() == 0 ||
            chunk.total_bytes() > maximum_node_release_bytes ||
            chunk.offset() != expected_offset || chunk.offset() >= chunk.total_bytes() ||
            chunk.data().empty() || chunk.data().size() > node_release_chunk_bytes ||
            static_cast<std::uint64_t>(chunk.data().size()) >
                chunk.total_bytes() - chunk.offset()) {
            throw std::runtime_error("control plane sent an invalid node release chunk");
        }
        if (!transfer_) {
            begin(chunk.version(), expected_digest, chunk.total_bytes());
        } else if (chunk.version() != transfer_->version_ ||
                   chunk.digest_sha256() != transfer_->digest_ ||
                   chunk.total_bytes() != transfer_->total_bytes_) {
            throw std::runtime_error("control plane changed the node release mid-transfer");
        }
        append(chunk.offset(), chunk.data());
        return complete();
    }

    [[nodiscard]] bool complete() const noexcept {
        return transfer_ && transfer_->written_bytes_ == transfer_->total_bytes_;
    }

    [[nodiscard]] std::uint64_t written_bytes() const {
        if (!transfer_) {
            throw std::logic_error("node release transfer is not in progress");
        }
        return transfer_->written_bytes_;
    }

    ruvia::task<void> commit(const ruvia::worker_handle& worker, ruvia::stop_token stop = {}) {
        if (!complete()) {
            throw std::runtime_error("node release transfer is incomplete");
        }
        auto value = std::move(*transfer_);
        transfer_.reset();
        value.output_.flush();
        value.output_.close();
        if (!value.output_) {
            remove_node_upgrade_candidate(value.candidate_);
            throw std::runtime_error("could not finalize node upgrade candidate");
        }

        bool previous_saved{};
        bool record_written{};
        bool installed{};
        try {
            if (flexedge::crypto::file_sha256(value.candidate_) != value.digest_) {
                throw std::runtime_error("control plane node release digest mismatch");
            }
            co_await check_node_upgrade_candidate(worker, value.candidate_, value.version_, stop);
            if (stop.stop_requested()) {
                throw std::runtime_error("node upgrade cancelled before installation");
            }
            const upgrade_record upgrade_record{
                .previous_version_ = config_.current_version_,
                .current_version_ = value.version_,
                .target_sha256_ = value.digest_,
            };
            preserve_node_binary(config_.binary_path_);
            previous_saved = true;
            write_pending_upgrade_record(config_.upgrade_record_path_, upgrade_record);
            record_written = true;
            install_node_upgrade_candidate(value.candidate_, config_.binary_path_);
            installed = true;
            const auto message = "validated node " + value.version_ + " candidate; restarting service";
            std::cerr << "flexedge node " << message << '\n';
            (void)log("info", message);
            if (config_.request_restart_) {
                config_.request_restart_();
            }
        } catch (...) {
            if (installed) {
                restore_node_binary(config_.binary_path_);
            } else if (previous_saved) {
                std::filesystem::remove(previous_node_binary_path(config_.binary_path_));
            }
            if (record_written) {
                std::filesystem::remove(config_.upgrade_record_path_);
            }
            remove_node_upgrade_candidate(value.candidate_);
            throw;
        }
    }

    void abort() noexcept {
        if (!transfer_) {
            return;
        }
        transfer_->output_.close();
        remove_node_upgrade_candidate(transfer_->candidate_);
        transfer_.reset();
    }

private:
    struct transfer final {
        std::string version_;
        std::string digest_;
        std::uint64_t total_bytes_{};
        std::uint64_t written_bytes_{};
        std::filesystem::path candidate_;
        std::ofstream output_{};
    };

    bool log(std::string_view level, std::string_view message) const noexcept {
        try {
            return config_.node_log_ && config_.node_log_(level, "upgrade", message);
        } catch (...) {
            return false;
        }
    }

    void validate_config() const {
        if (config_.binary_path_.empty()) {
            throw std::runtime_error("node binary path is required");
        }
        if (config_.upgrade_record_path_.empty()) {
            throw std::runtime_error("node upgrade record path is required");
        }
        if (!valid_node_release_version(config_.current_version_)) {
            throw std::runtime_error("current node version is invalid");
        }
    }

    self_updater_config config_;
    const std::string current_digest_;
    std::optional<transfer> transfer_;
    std::optional<upgrade_record> startup_upgrade_{};
    bool startup_confirmed_{};
};

}  // namespace flexedge::node
