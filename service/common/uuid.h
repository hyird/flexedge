#pragma once
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace service::common {
inline std::optional<std::string> parseUuid(std::optional<std::string_view> input) {
    if (!input || input->size() != 36) {
        return std::nullopt;
    }

    std::string value;
    value.reserve(input->size());
    for (std::size_t index = 0; index < input->size(); ++index) {
        const char ch = (*input)[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-') {
                return std::nullopt;
            }
            value.push_back(ch);
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
        value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return value;
}

} // namespace service::common
