#pragma once

#include <ranges>
#include <string>
#include <string_view>

#include "common/sha256.h"

namespace flexedge::node {

inline constexpr bool valid_node_release_version(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' || ch == '+';
    });
}

inline std::string node_release_entity_tag(std::string_view digest) {
    return "\"" + std::string(digest) + "\"";
}

}  // namespace flexedge::node
