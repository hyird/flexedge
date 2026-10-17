#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
namespace service::utils {

[[nodiscard]] inline char lower_ascii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] inline std::string to_lower_ascii(std::string_view value) {
    std::string result(value);
    for (auto& ch : result) {
        ch = lower_ascii(ch);
    }
    return result;
}

[[nodiscard]] inline constexpr bool is_whitespace_ascii(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\f' || value == '\v';
}

[[nodiscard]] inline constexpr std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && is_whitespace_ascii(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_whitespace_ascii(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline constexpr bool has_non_whitespace_ascii(std::string_view value) noexcept {
    return !trim_ascii(value).empty();
}

inline void append_length_prefixed(std::string& output, std::string_view field) {
    output += std::to_string(field.size());
    output.push_back(':');
    output.append(field);
}

[[nodiscard]] inline std::optional<std::string_view> consume_length_prefixed(std::string_view& payload) noexcept {
    const auto colon = payload.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t length{};
    const auto [end, error] = std::from_chars(payload.data(), payload.data() + colon, length);
    if (error != std::errc{} || end != payload.data() + colon || length > payload.size() - colon - 1) {
        return std::nullopt;
    }
    const auto field = payload.substr(colon + 1, length);
    payload.remove_prefix(colon + 1 + length);
    return field;
}

[[nodiscard]] inline bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (lower_ascii(left[index]) != lower_ascii(right[index])) {
            return false;
        }
    }
    return true;
}

// Whether `needle` occurs at `position` of `value`, ASCII case ignored.
[[nodiscard]] inline bool matches_ignore_case_at(std::string_view value, std::size_t position,
    std::string_view needle) noexcept {
    return position + needle.size() <= value.size() && equals_ignore_case(value.substr(position, needle.size()), needle);
}

[[nodiscard]] inline bool contains(std::span<const std::string_view> values, std::string_view value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

[[nodiscard]] inline bool is_ascii_alnum(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
}

// Lower-case media type of a Content-Type value, without parameters.
[[nodiscard]] inline std::string media_type(std::string_view content_type) {
    auto type = content_type.substr(0, content_type.find(';'));
    while (!type.empty() && (type.front() == ' ' || type.front() == '\t')) {
        type.remove_prefix(1);
    }
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.remove_suffix(1);
    }
    return to_lower_ascii(type);
}

// `type` as returned by media_type().
[[nodiscard]] inline bool json_media_type(std::string_view type) noexcept {
    return type == "application/json" || type.ends_with("+json") || type == "application/dns-json" ||
           type == "text/json";
}

}  // namespace service::utils
