#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "node/proto/release_metadata.h"
#include "node/runtime/secure_file.h"

namespace flexedge::node {

struct UpgradeRecord final {
    std::string previousVersion;
    std::string currentVersion;
    std::string targetSha256;

    [[nodiscard]] std::string message() const {
        return "upgraded from " + previousVersion + " to " + currentVersion + " (" + targetSha256 +
               ")";
    }
};

inline bool validUpgradeRecord(const UpgradeRecord& record) {
    return validNodeReleaseVersion(record.previousVersion) &&
           validNodeReleaseVersion(record.currentVersion) &&
           flexedge::crypto::isSha256Digest(record.targetSha256);
}

inline void writePendingUpgradeRecord(const std::filesystem::path& path,
                                      const UpgradeRecord& record) {
    if (!validUpgradeRecord(record)) {
        throw std::invalid_argument("upgrade record is invalid");
    }
    writeSecureFileAtomic(path, record.previousVersion + "\n" + record.currentVersion + "\n" +
                                    record.targetSha256 + "\n");
}

inline std::optional<UpgradeRecord> parseUpgradeRecord(std::string_view bytes) {
    const auto firstSeparator = bytes.find('\n');
    const auto secondSeparator = firstSeparator == std::string::npos
                                     ? std::string::npos
                                     : bytes.find('\n', firstSeparator + 1);
    const auto thirdSeparator = secondSeparator == std::string::npos
                                    ? std::string::npos
                                    : bytes.find('\n', secondSeparator + 1);
    if (firstSeparator == std::string::npos || secondSeparator == std::string::npos ||
        thirdSeparator == std::string::npos || thirdSeparator + 1 != bytes.size()) {
        return std::nullopt;
    }

    UpgradeRecord record{
        .previousVersion = std::string(bytes.substr(0, firstSeparator)),
        .currentVersion = std::string(bytes.substr(firstSeparator + 1, secondSeparator - firstSeparator - 1)),
        .targetSha256 = std::string(bytes.substr(secondSeparator + 1, thirdSeparator - secondSeparator - 1)),
    };
    if (!validUpgradeRecord(record)) {
        return std::nullopt;
    }
    return record;
}

inline std::optional<UpgradeRecord> takePendingUpgradeRecord(const std::filesystem::path& path,
                                                             std::string_view currentDigest) {
    std::optional<std::string> bytes;
    try {
        bytes = readSecureFile(path);
    } catch (...) {
        return std::nullopt;
    }
    if (!bytes) {
        return std::nullopt;
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    auto record = parseUpgradeRecord(*bytes);
    if (record && record->targetSha256 != currentDigest) return std::nullopt;
    return record;
}

} // namespace flexedge::node
