#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "node/proto/release_metadata.h"
#include "node/runtime/secure_file.h"

namespace flexedge::node {

struct upgrade_record final {
    std::string previous_version_;
    std::string current_version_;
    std::string target_sha256_;

    [[nodiscard]] std::string message() const {
        return "upgraded from " + previous_version_ + " to " + current_version_ + " (" + target_sha256_ +
               ")";
    }
};

inline bool valid_upgrade_record(const upgrade_record& record) {
    return valid_node_release_version(record.previous_version_) &&
           valid_node_release_version(record.current_version_) &&
           flexedge::crypto::is_sha256_digest(record.target_sha256_);
}

inline void write_pending_upgrade_record(const std::filesystem::path& path,
    const upgrade_record& record) {
    if (!valid_upgrade_record(record)) {
        throw std::invalid_argument("upgrade record is invalid");
    }
    write_secure_file_atomic(path, record.previous_version_ + "\n" + record.current_version_ + "\n" +
                                       record.target_sha256_ + "\n");
}

inline std::optional<upgrade_record> parse_upgrade_record(std::string_view bytes) {
    const auto first_separator = bytes.find('\n');
    const auto second_separator = first_separator == std::string::npos
                                      ? std::string::npos
                                      : bytes.find('\n', first_separator + 1);
    const auto third_separator = second_separator == std::string::npos
                                     ? std::string::npos
                                     : bytes.find('\n', second_separator + 1);
    if (first_separator == std::string::npos || second_separator == std::string::npos ||
        third_separator == std::string::npos || third_separator + 1 != bytes.size()) {
        return std::nullopt;
    }

    upgrade_record record{
        .previous_version_ = std::string(bytes.substr(0, first_separator)),
        .current_version_ = std::string(bytes.substr(first_separator + 1, second_separator - first_separator - 1)),
        .target_sha256_ = std::string(bytes.substr(second_separator + 1, third_separator - second_separator - 1)),
    };
    if (!valid_upgrade_record(record)) {
        return std::nullopt;
    }
    return record;
}

inline std::optional<upgrade_record> take_pending_upgrade_record(const std::filesystem::path& path,
    std::string_view current_digest) {
    std::optional<std::string> bytes;
    try {
        bytes = read_secure_file(path);
    } catch (...) {
        return std::nullopt;
    }
    if (!bytes) {
        return std::nullopt;
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    auto record = parse_upgrade_record(*bytes);
    if (record && record->target_sha256_ != current_digest) {
        return std::nullopt;
    }
    return record;
}

}  // namespace flexedge::node
