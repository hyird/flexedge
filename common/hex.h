#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flexedge::crypto {

inline std::string hex_encode(std::span<const unsigned char> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

inline std::optional<std::vector<unsigned char>> hex_decode(std::string_view value) {
    if (value.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };
    std::vector<unsigned char> result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const int high = nibble(value[index]);
        const int low = nibble(value[index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return result;
}

}  // namespace flexedge::crypto
