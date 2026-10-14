#pragma once

#include <algorithm>
#include <cctype>
#include <regex>
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

inline bool is_hostname(std::string_view value, bool allow_wildcard = true) {
    if (value.empty() || value.size() > 253 || (!allow_wildcard && value.starts_with("*."))) {
        return false;
    }
    static const std::regex pattern(
        R"(^(?:\*\.)?([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$)");
    return std::regex_match(value.begin(), value.end(), pattern);
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
    if (!certificate.starts_with("*.")) {
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
