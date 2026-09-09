#pragma once

#include <span>
#include <string>

namespace flexedge::crypto {

inline std::string hexEncode(std::span<const unsigned char> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

} // namespace flexedge::crypto
