#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace service::utils {

// Length of the valid UTF-8 sequence starting at `index`, or 0 when it is invalid.
[[nodiscard]] inline std::size_t utf8_sequence_length(std::string_view value, std::size_t index) noexcept {
    const auto lead = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    unsigned char lower = 0x80;
    unsigned char upper = 0xBF;
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        lower = lead == 0xE0 ? 0xA0 : 0x80;
        upper = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        lower = lead == 0xF0 ? 0x90 : 0x80;
        upper = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0;
    }
    if (index + length > value.size()) {
        return 0;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto next = static_cast<unsigned char>(value[index + offset]);
        if (next < (offset == 1 ? lower : 0x80) || next > (offset == 1 ? upper : 0xBF)) {
            return 0;
        }
    }
    return length;
}

// Shortens valid UTF-8 text to at most `limit` bytes without splitting a
// character. Reports whether anything was cut.
inline bool truncate_utf8(std::string& value, std::size_t limit) noexcept {
    if (value.size() <= limit) {
        return false;
    }
    auto size = limit;
    while (size > 0 && (static_cast<unsigned char>(value[size]) & 0xC0) == 0x80) {
        --size;
    }
    value.resize(size);
    return true;
}

// Valid UTF-8 without control characters other than line feeds, carriage
// returns and tabs.
[[nodiscard]] inline bool valid_utf8_text(std::string_view value) noexcept {
    for (std::size_t index = 0; index < value.size();) {
        const auto length = utf8_sequence_length(value, index);
        if (length == 0) {
            return false;
        }
        const auto lead = static_cast<unsigned char>(value[index]);
        if (length == 1 && lead < 0x20 && lead != '\n' && lead != '\r' && lead != '\t') {
            return false;
        }
        index += length;
    }
    return true;
}

// Valid UTF-8 only; NUL and control characters become spaces except line
// feeds and tabs in multi-line text (PostgreSQL rejects NUL in jsonb).
[[nodiscard]] inline std::string sanitized_utf8(std::string_view value, bool multiline) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto length = utf8_sequence_length(value, index);
        if (length == 0) {
            ++index;
            continue;
        }
        const auto lead = static_cast<unsigned char>(value[index]);
        if (length == 1 && (lead < 0x20 || lead == 0x7F)) {
            if (lead == '\r') {
                if (!multiline) {
                    result.push_back(' ');
                }
            } else if (multiline && (lead == '\n' || lead == '\t')) {
                result.push_back(static_cast<char>(lead));
            } else {
                result.push_back(' ');
            }
        } else {
            result.append(value.substr(index, length));
        }
        index += length;
    }
    return result;
}

}  // namespace service::utils
