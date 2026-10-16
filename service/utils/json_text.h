#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace service::utils {

// Appends `value` as a quoted JSON string.
inline void append_json_string(std::string& output, std::string_view value) {
    constexpr std::string_view digits = "0123456789abcdef";
    output.push_back('"');
    for (const auto raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"':
                output.append("\\\"");
                break;
            case '\\':
                output.append("\\\\");
                break;
            case '\n':
                output.append("\\n");
                break;
            case '\t':
                output.append("\\t");
                break;
            case '\r':
                output.append("\\r");
                break;
            default:
                if (ch < 0x20) {
                    output.append("\\u00");
                    output.push_back(digits[ch >> 4]);
                    output.push_back(digits[ch & 0x0F]);
                } else {
                    output.push_back(raw);
                }
        }
    }
    output.push_back('"');
}

// Upper bound of the bytes `value` takes as a JSON string, quotes included.
[[nodiscard]] inline std::size_t json_string_size(std::string_view value) noexcept {
    std::size_t size = 2;
    for (const auto raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (raw == '"' || raw == '\\' || raw == '\n' || raw == '\t' || raw == '\r') {
            size += 2;
        } else if (ch < 0x20) {
            size += 6;
        } else {
            ++size;
        }
    }
    return size;
}

}  // namespace service::utils
