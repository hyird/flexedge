#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace flexedge::route_policy {

[[nodiscard]] inline constexpr char lower_ascii(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
}

[[nodiscard]] inline constexpr bool ascii_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (lower_ascii(static_cast<unsigned char>(left[i])) !=
            lower_ascii(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline constexpr bool ascii_starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && ascii_equal(value.substr(0, prefix.size()), prefix);
}

[[nodiscard]] inline constexpr std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline constexpr bool is_http_token_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

[[nodiscard]] inline constexpr bool http_token(std::string_view value, std::size_t maximum = 256) noexcept {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    for (const char c : value) {
        if (!is_http_token_char(c)) {
            return false;
        }
    }
    return true;
}

inline bool clean_text(std::string_view value, std::size_t maximum) {
    return value.size() <= maximum &&
           std::ranges::none_of(value, [](unsigned char c) { return std::iscntrl(c) != 0; });
}

inline std::string hostname(std::string_view value) {
    // Website route domains are DNS names, not IPv6 literals or wildcard patterns.
    value = value.substr(0, value.find(':'));
    if (value.ends_with('.')) {
        value.remove_suffix(1);
    }
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

inline bool exact_hostname(std::string_view value) {
    if (value.empty() || value.size() > 253) {
        return false;
    }
    if (value.ends_with('.')) {
        value.remove_suffix(1);
    }
    while (!value.empty()) {
        const auto dot = value.find('.');
        const auto label = value.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            std::ranges::any_of(label, [](unsigned char c) {
                return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-');
            })) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        value.remove_prefix(dot + 1);
        if (value.empty()) {
            return false;
        }
    }
    return false;
}

inline bool rewrite_mode(std::string_view mode) {
    return mode.empty() || mode == "none" || mode == "replace_path" ||
           mode == "strip_prefix" || mode == "replace_prefix";
}

inline bool query_mode(std::string_view mode) {
    return mode.empty() || mode == "preserve" || mode == "drop" || mode == "replace";
}

inline bool query_string(std::string_view value) {
    return value.size() <= 2048 && !value.starts_with('?') &&
           std::ranges::none_of(value, [](unsigned char c) {
               return c <= 32 || c == 127 || c == '#';
           });
}

inline bool path_only(std::string_view value) {
    return !value.empty() && value.front() == '/' && value.size() <= 2048 &&
           value.find_first_of("?#") == std::string_view::npos &&
           std::ranges::none_of(value, [](unsigned char c) { return c <= 32 || c == 127; });
}

}  // namespace flexedge::route_policy
