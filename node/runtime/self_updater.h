#pragma once

#include <chrono>
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

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/runtime/binary_digest.h"
#include "node/runtime/release_response.h"
#include "node/runtime/upgrade_record.h"

namespace flexedge::node {

struct SelfUpdaterConfig final {
    std::filesystem::path binaryPath;
    std::string currentVersion;
    std::filesystem::path upgradeRecordPath;
    std::function<bool(std::string_view, std::string_view, std::string_view)> nodeLog;
    std::function<void()> requestRestart;
};

// The control channel owns this object. Release bytes never leave the authenticated
// WebSocket protocol: ControlChannel asks for one bounded chunk, writes it, then asks for
// the next offset. A disconnected transfer is discarded and starts from offset zero after
// the next authenticated control session.
class SelfUpdater final {
  public:
    explicit SelfUpdater(SelfUpdaterConfig config)
        : config_(std::move(config)), currentDigest_(binarySha256(config_.binaryPath)) {
        validateConfig();
    }

    SelfUpdater(const SelfUpdater&) = delete;
    SelfUpdater& operator=(const SelfUpdater&) = delete;

    ~SelfUpdater() { abort(); }

    [[nodiscard]] bool updateAvailable(std::string_view digest) const noexcept {
        return validNodeReleaseDigest(digest) && digest != currentDigest_;
    }

    void begin(std::string_view version, std::string_view digest, std::uint64_t totalBytes) {
        if (transfer_) {
            throw std::logic_error("node release transfer is already in progress");
        }
        if (!updateAvailable(digest) || !validNodeReleaseVersion(version) || totalBytes == 0 ||
            totalBytes > kMaximumNodeReleaseBytes) {
            throw std::runtime_error("control plane sent invalid node release metadata");
        }

        Transfer value{
            .version = std::string(version),
            .digest = std::string(digest),
            .totalBytes = totalBytes,
            .candidate = candidatePath(),
        };
        value.output.open(value.candidate, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!value.output) {
            throw std::runtime_error("could not create node upgrade candidate");
        }
        transfer_.emplace(std::move(value));
    }

    void append(std::uint64_t offset, std::string_view bytes) {
        if (!transfer_) {
            throw std::logic_error("node release transfer is not in progress");
        }
        auto& value = *transfer_;
        if (offset != value.writtenBytes || bytes.empty() ||
            bytes.size() > kNodeReleaseChunkBytes ||
            static_cast<std::uint64_t>(bytes.size()) > value.totalBytes - value.writtenBytes) {
            throw std::runtime_error("control plane sent an invalid node release chunk");
        }
        value.output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!value.output) {
            throw std::runtime_error("could not write node upgrade candidate");
        }
        value.writtenBytes += static_cast<std::uint64_t>(bytes.size());
    }

    [[nodiscard]] bool complete() const noexcept {
        return transfer_ && transfer_->writtenBytes == transfer_->totalBytes;
    }

    [[nodiscard]] std::uint64_t writtenBytes() const {
        if (!transfer_) {
            throw std::logic_error("node release transfer is not in progress");
        }
        return transfer_->writtenBytes;
    }

    void commit() {
        if (!complete()) {
            throw std::runtime_error("node release transfer is incomplete");
        }
        auto value = std::move(*transfer_);
        transfer_.reset();
        value.output.flush();
        value.output.close();
        if (!value.output) {
            removeCandidate(value.candidate);
            throw std::runtime_error("could not finalize node upgrade candidate");
        }

        try {
            if (binarySha256(value.candidate) != value.digest) {
                throw std::runtime_error("control plane node release digest mismatch");
            }
            const UpgradeRecord upgradeRecord{
                .previousVersion = config_.currentVersion,
                .currentVersion = value.version,
                .targetSha256 = value.digest,
            };
            try {
                writePendingUpgradeRecord(config_.upgradeRecordPath, upgradeRecord);
            } catch (const std::exception& error) {
                std::cerr << "flexedge node could not persist upgrade record: " << error.what()
                          << '\n';
            }
            installCandidate(value.candidate);
            std::cerr << "flexedge node " << upgradeRecord.message() << "; restarting service\n";
            (void)log("info", upgradeRecord.message());
            if (config_.requestRestart) {
                config_.requestRestart();
            }
        } catch (...) {
            removeCandidate(value.candidate);
            throw;
        }
    }

    void abort() noexcept {
        if (!transfer_) {
            return;
        }
        transfer_->output.close();
        removeCandidate(transfer_->candidate);
        transfer_.reset();
    }

  private:
    struct Transfer final {
        std::string version;
        std::string digest;
        std::uint64_t totalBytes{};
        std::uint64_t writtenBytes{};
        std::filesystem::path candidate;
        std::ofstream output;
    };

    bool log(std::string_view level, std::string_view message) const noexcept {
        try {
            return config_.nodeLog && config_.nodeLog(level, "upgrade", message);
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::filesystem::path candidatePath() const {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        auto candidate = config_.binaryPath.parent_path();
        candidate /= "." + config_.binaryPath.filename().string() + ".upgrade." +
#ifdef _WIN32
                     "windows"
#else
                     std::to_string(::getpid())
#endif
                     + "." + std::to_string(now);
        return candidate;
    }

    static void removeCandidate(const std::filesystem::path& path) noexcept {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    void installCandidate(const std::filesystem::path& candidate) const {
#ifdef _WIN32
        (void)candidate;
        throw std::runtime_error("node self-upgrade is not supported on Windows");
#else
        if (::chmod(candidate.c_str(),
                    S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
            throw std::runtime_error("failed to set node binary permissions");
        }
        std::error_code error;
        std::filesystem::rename(candidate, config_.binaryPath, error);
        if (error) {
            throw std::runtime_error("failed to replace node binary");
        }
#endif
    }

    void validateConfig() const {
        if (config_.binaryPath.empty()) {
            throw std::runtime_error("node binary path is required");
        }
        if (config_.upgradeRecordPath.empty()) {
            throw std::runtime_error("node upgrade record path is required");
        }
        if (!validNodeReleaseVersion(config_.currentVersion)) {
            throw std::runtime_error("current node version is invalid");
        }
    }

    SelfUpdaterConfig config_;
    const std::string currentDigest_;
    std::optional<Transfer> transfer_;
};

} // namespace flexedge::node
