#pragma once

#include <optional>
#include <string>
#include <string_view>
#include "node/proto/release_metadata.h"

namespace service::node_release {

struct Manifest final {
    std::string version;
    std::string digest;
};

inline std::optional<Manifest> parseManifest(std::string_view value) {
    constexpr std::string_view versionPrefix{"version="};
    constexpr std::string_view digestPrefix{"sha256="};
    const auto takeLine = [&value]() -> std::optional<std::string_view> {
        const auto lineEnd = value.find('\n');
        if (lineEnd == std::string_view::npos) {
            return std::nullopt;
        }
        auto line = value.substr(0, lineEnd);
        value.remove_prefix(lineEnd + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        return line;
    };

    const auto headerLine = takeLine();
    const auto versionLine = takeLine();
    const auto digestLine = takeLine();
    if (!headerLine || *headerLine != "flexedge-node-release-v1" || !versionLine || !digestLine ||
        !value.empty()) {
        return std::nullopt;
    }
    if (!versionLine->starts_with(versionPrefix)) {
        return std::nullopt;
    }
    if (!digestLine->starts_with(digestPrefix)) {
        return std::nullopt;
    }

    Manifest result{.version = std::string(versionLine->substr(versionPrefix.size())),
                    .digest = std::string(digestLine->substr(digestPrefix.size()))};
    if (!flexedge::node::validNodeReleaseVersion(result.version) ||
        !flexedge::crypto::isSha256Digest(result.digest)) {
        return std::nullopt;
    }
    return result;
}

} // namespace service::node_release
