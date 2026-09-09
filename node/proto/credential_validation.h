#pragma once

#include <ranges>
#include <string_view>

namespace flexedge::node {

[[nodiscard]] inline constexpr bool validCredentialNodeId(std::string_view value) noexcept {
    return value.size() == 32 && std::ranges::all_of(value, [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

[[nodiscard]] inline constexpr bool validCredentialSecret(std::string_view value) noexcept {
    return value.size() >= 32 && value.size() <= 128 &&
           std::ranges::all_of(value, [](unsigned char ch) { return ch >= 0x21 && ch <= 0x7e; });
}

} // namespace flexedge::node
