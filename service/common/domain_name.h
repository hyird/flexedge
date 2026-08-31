#pragma once

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <string_view>

namespace service::common {

inline std::string normalizeDomainName(std::string_view value) {
    std::string result(value);
    while (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    std::ranges::transform(result, result.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

inline bool isHostname(std::string_view value, bool allowWildcard = true) {
    if (value.empty() || value.size() > 253 || (!allowWildcard && value.starts_with("*."))) {
        return false;
    }
    static const std::regex pattern(
        R"(^(?:\*\.)?([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$)");
    return std::regex_match(value.begin(), value.end(), pattern);
}

inline bool certificateCoversHostname(std::string_view certificateDomain,
                                      std::string_view hostname) {
    const auto certificate = normalizeDomainName(certificateDomain);
    const auto host = normalizeDomainName(hostname);
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
    if (host == base) {
        return true;
    }
    const auto suffix = "." + base;
    if (host.size() <= suffix.size() || !host.ends_with(suffix)) {
        return false;
    }
    return host.substr(0, host.size() - suffix.size()).find('.') == std::string::npos;
}

inline bool domainBelongsToZone(std::string_view hostname, std::string_view zoneDomain) {
    auto host = normalizeDomainName(hostname);
    const auto zone = normalizeDomainName(zoneDomain);
    if (host.starts_with("*.")) {
        host.erase(0, 2);
    }
    return !host.empty() && !zone.empty() &&
           (host == zone || (host.size() > zone.size() && host.ends_with("." + zone)));
}

} // namespace service::common
