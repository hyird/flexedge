#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace service::utils {

struct percent_encode_options final {
    bool lower_hex_{};
    // application/x-www-form-urlencoded: a space becomes '+'.
    bool form_{};
};

// RFC 3986 percent-encoding: unreserved characters stay, every other byte
// becomes %XX (uppercase hex digits unless `lower_hex_`).
[[nodiscard]] inline std::string percent_encode(std::string_view input, percent_encode_options options = {}) {
    constexpr std::string_view upper_digits = "0123456789ABCDEF";
    constexpr std::string_view lower_digits = "0123456789abcdef";
    const auto digits = options.lower_hex_ ? lower_digits : upper_digits;
    std::string result;
    result.reserve(input.size());
    for (const unsigned char ch : input) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '.' || ch == '_' || ch == '~') {
            result.push_back(static_cast<char>(ch));
        } else if (options.form_ && ch == ' ') {
            result.push_back('+');
        } else {
            result.push_back('%');
            result.push_back(digits[ch >> 4]);
            result.push_back(digits[ch & 0x0F]);
        }
    }
    return result;
}

// Decodes %XX (and '+' as space); malformed escapes are kept literally.
[[nodiscard]] inline std::string percent_decode(std::string_view input) {
    const auto hex_value = [](char value) noexcept {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    std::string result;
    result.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '%' && index + 2 < input.size()) {
            const auto high = hex_value(input[index + 1]);
            const auto low = hex_value(input[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>(high * 16 + low));
                index += 2;
                continue;
            }
        }
        result.push_back(input[index] == '+' ? ' ' : input[index]);
    }
    return result;
}

}  // namespace service::utils
