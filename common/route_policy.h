#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace flexedge::route_policy {

inline bool cleanText(std::string_view value, std::size_t maximum) {
    return value.size() <= maximum &&
           std::ranges::none_of(value, [](unsigned char c) { return std::iscntrl(c) != 0; });
}

inline std::string hostname(std::string_view value) {
    // Website route domains are DNS names, not IPv6 literals or wildcard patterns.
    value = value.substr(0, value.find(':'));
    if (value.ends_with('.')) value.remove_suffix(1);
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

inline bool exactHostname(std::string_view value) {
    if (value.empty() || value.size() > 253) return false;
    if (value.ends_with('.')) value.remove_suffix(1);
    while (!value.empty()) {
        const auto dot = value.find('.');
        const auto label = value.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            std::ranges::any_of(label, [](unsigned char c) {
                return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-');
            })) return false;
        if (dot == std::string_view::npos) return true;
        value.remove_prefix(dot + 1);
        if (value.empty()) return false;
    }
    return false;
}

inline bool rewriteMode(std::string_view mode) {
    return mode.empty() || mode == "none" || mode == "replace_path" ||
           mode == "strip_prefix" || mode == "replace_prefix";
}

inline bool queryMode(std::string_view mode) {
    return mode.empty() || mode == "preserve" || mode == "drop" || mode == "replace";
}

inline bool queryString(std::string_view value) {
    return value.size() <= 2048 && !value.starts_with('?') &&
           std::ranges::none_of(value, [](unsigned char c) {
               return c <= 32 || c == 127 || c == '#';
           });
}

inline bool pathOnly(std::string_view value) {
    return !value.empty() && value.front() == '/' && value.size() <= 2048 &&
           value.find_first_of("?#") == std::string_view::npos &&
           std::ranges::none_of(value, [](unsigned char c) { return c <= 32 || c == 127; });
}

inline std::string rewritePath(std::string_view path, std::string_view prefix,
                               std::string_view replacement, std::string_view mode) {
    if (mode.empty()) mode = replacement.empty() ? "none" : "replace_path";
    if (mode == "none") return std::string(path);
    if (mode == "replace_path") return std::string(replacement);
    if (!path.starts_with(prefix)) return std::string(path);
    auto suffix = path.substr(prefix.size());
    std::string result(mode == "strip_prefix" ? "/" : replacement);
    if (suffix.empty()) return result.empty() ? "/" : result;
    if (result.ends_with('/') && suffix.starts_with('/')) suffix.remove_prefix(1);
    else if (!result.ends_with('/') && !suffix.starts_with('/')) result.push_back('/');
    result.append(suffix);
    return result;
}

inline std::string applyQuery(std::string_view destination, std::string_view incoming,
                              std::string_view mode, std::string_view replacement) {
    const auto fragmentAt = destination.find('#');
    const auto fragment = fragmentAt == std::string_view::npos ? std::string_view{} : destination.substr(fragmentAt);
    auto base = destination.substr(0, fragmentAt);
    std::string result;
    if (mode == "drop" || mode == "replace") {
        result = base.substr(0, base.find('?'));
        if (mode == "replace" && !replacement.empty()) {
            result.push_back('?');
            result.append(replacement);
        }
    } else {
        // Preserve existing redirect query strings; otherwise forward the request query.
        result = base;
        const auto queryAt = incoming.find('?');
        if (base.find('?') == std::string_view::npos && queryAt != std::string_view::npos) {
            result.append(incoming.substr(queryAt));
        }
    }
    result.append(fragment);
    return result;
}

} // namespace flexedge::route_policy
