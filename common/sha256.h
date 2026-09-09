#pragma once

#include <string_view>

namespace flexedge::crypto {

inline constexpr bool isSha256Digest(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace flexedge::crypto
