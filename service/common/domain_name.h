#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace service::common {

inline std::string normalize_domain_name(std::string_view value) {
    std::string result(value);
    while (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    std::ranges::transform(result, result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

[[nodiscard]] inline constexpr bool is_valid_dns_label(std::string_view label,
    bool letters_only = false) noexcept {
    if (label.empty() || label.size() > 63) {
        return false;
    }
    const auto is_alpha = [](char ch) noexcept {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    };
    const auto is_alnum = [&](char ch) noexcept {
        return is_alpha(ch) || (ch >= '0' && ch <= '9');
    };
    if (letters_only) {
        if (label.size() < 2) {
            return false;
        }
        for (const char ch : label) {
            if (!is_alpha(ch)) {
                return false;
            }
        }
        return true;
    }
    if (!is_alnum(label.front()) || !is_alnum(label.back())) {
        return false;
    }
    for (const char ch : label) {
        if (!is_alnum(ch) && ch != '-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline constexpr bool is_hostname(std::string_view value,
    bool allow_wildcard = true) noexcept {
    if (value.empty() || value.size() > 253) {
        return false;
    }
    if (value.starts_with("*.")) {
        if (!allow_wildcard) {
            return false;
        }
        value.remove_prefix(2);
    }
    std::size_t label_count = 0;
    while (!value.empty()) {
        const auto dot = value.find('.');
        const bool is_last = (dot == std::string_view::npos);
        const auto label = value.substr(0, dot);
        if (!is_valid_dns_label(label, is_last)) {
            return false;
        }
        ++label_count;
        if (is_last) {
            return label_count >= 2;
        }
        value.remove_prefix(dot + 1);
        if (value.empty()) {
            return false;
        }
    }
    return false;
}

inline bool certificate_covers_hostname(std::string_view certificate_domain,
    std::string_view hostname) {
    const auto certificate = normalize_domain_name(certificate_domain);
    const auto host = normalize_domain_name(hostname);
    if (certificate.empty() || host.empty()) {
        return false;
    }
    if (certificate == host) {
        return true;
    }
    // A wildcard website hostname is only covered by the identical wildcard name.
    if (!certificate.starts_with("*.") || host.starts_with("*.")) {
        return false;
    }

    const auto base = certificate.substr(2);
    const auto suffix = "." + base;
    if (host.size() <= suffix.size() || !host.ends_with(suffix)) {
        return false;
    }
    return host.substr(0, host.size() - suffix.size()).find('.') == std::string::npos;
}

inline bool domain_belongs_to_zone(std::string_view hostname, std::string_view zone_domain) {
    auto host = normalize_domain_name(hostname);
    const auto zone = normalize_domain_name(zone_domain);
    if (host.starts_with("*.")) {
        host.erase(0, 2);
    }
    return !host.empty() && !zone.empty() &&
           (host == zone || (host.size() > zone.size() && host.ends_with("." + zone)));
}

}  // namespace service::common
