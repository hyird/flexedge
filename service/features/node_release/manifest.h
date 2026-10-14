#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "node/proto/release_metadata.h"

namespace service::node_release {

struct manifest final {
    std::string version_;
    std::string digest_;
};

inline std::optional<manifest> parse_manifest(std::string_view value) {
    constexpr std::string_view version_prefix{"version="};
    constexpr std::string_view digest_prefix{"sha256="};
    const auto take_line = [&value]() -> std::optional<std::string_view> {
        const auto line_end = value.find('\n');
        if (line_end == std::string_view::npos) {
            return std::nullopt;
        }
        auto line = value.substr(0, line_end);
        value.remove_prefix(line_end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        return line;
    };

    const auto header_line = take_line();
    const auto version_line = take_line();
    const auto digest_line = take_line();
    if (!header_line || *header_line != "flexedge-node-release-v1" || !version_line || !digest_line ||
        !value.empty()) {
        return std::nullopt;
    }
    if (!version_line->starts_with(version_prefix)) {
        return std::nullopt;
    }
    if (!digest_line->starts_with(digest_prefix)) {
        return std::nullopt;
    }

    manifest result{.version_ = std::string(version_line->substr(version_prefix.size())),
        .digest_ = std::string(digest_line->substr(digest_prefix.size()))};
    if (!flexedge::node::valid_node_release_version(result.version_) ||
        !flexedge::crypto::is_sha256_digest(result.digest_)) {
        return std::nullopt;
    }
    return result;
}

}  // namespace service::node_release
