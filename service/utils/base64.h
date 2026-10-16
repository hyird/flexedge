#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace service::utils {

namespace base64_detail {

[[nodiscard]] inline std::string encode(std::string_view value, std::string_view alphabet, bool padding) {
    std::string result;
    result.reserve((value.size() + 2) / 3 * 4);
    std::size_t index = 0;
    for (; index + 2 < value.size(); index += 3) {
        const auto chunk = (static_cast<unsigned>(static_cast<unsigned char>(value[index])) << 16) |
                           (static_cast<unsigned>(static_cast<unsigned char>(value[index + 1])) << 8) |
                           static_cast<unsigned>(static_cast<unsigned char>(value[index + 2]));
        result.push_back(alphabet[(chunk >> 18) & 0x3F]);
        result.push_back(alphabet[(chunk >> 12) & 0x3F]);
        result.push_back(alphabet[(chunk >> 6) & 0x3F]);
        result.push_back(alphabet[chunk & 0x3F]);
    }
    const auto rest = value.size() - index;
    if (rest > 0) {
        auto chunk = static_cast<unsigned>(static_cast<unsigned char>(value[index])) << 16;
        if (rest == 2) {
            chunk |= static_cast<unsigned>(static_cast<unsigned char>(value[index + 1])) << 8;
        }
        result.push_back(alphabet[(chunk >> 18) & 0x3F]);
        result.push_back(alphabet[(chunk >> 12) & 0x3F]);
        if (rest == 2) {
            result.push_back(alphabet[(chunk >> 6) & 0x3F]);
        }
        if (padding) {
            result.append(rest == 1 ? "==" : "=");
        }
    }
    return result;
}

}  // namespace base64_detail

// Standard alphabet, padded.
[[nodiscard]] inline std::string base64_encode(std::string_view value) {
    return base64_detail::encode(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

// URL-safe alphabet, unpadded.
[[nodiscard]] inline std::string base64_url_encode(std::string_view value) {
    return base64_detail::encode(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

[[nodiscard]] inline bool is_base64_url_char(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
           value == '-' || value == '_';
}

// Accepts both alphabets; decoding stops at the first '='. nullopt on any
// other character.
[[nodiscard]] inline std::optional<std::string> base64_url_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size() * 3 / 4);
    unsigned buffer = 0;
    int bits = 0;
    for (const auto ch : value) {
        int digit = -1;
        if (ch >= 'A' && ch <= 'Z') {
            digit = ch - 'A';
        } else if (ch >= 'a' && ch <= 'z') {
            digit = ch - 'a' + 26;
        } else if (ch >= '0' && ch <= '9') {
            digit = ch - '0' + 52;
        } else if (ch == '-' || ch == '+') {
            digit = 62;
        } else if (ch == '_' || ch == '/') {
            digit = 63;
        } else if (ch == '=') {
            break;
        } else {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return result;
}

}  // namespace service::utils
