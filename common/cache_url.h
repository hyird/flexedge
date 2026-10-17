#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "common/route_policy.h"
namespace flexedge::cache {
struct url final {
    std::string scheme_{};
    std::string authority_{};
    std::string hostname_{};
    std::string target_{};
    std::string value_{};
};
[[nodiscard]] inline std::optional<url> parse_url(std::string_view value) {
    if (value.size() > 4096 || value.find('#') != std::string_view::npos || std::ranges::any_of(value, [](unsigned char c) { return c <= 32 || c == 127; })) {
        return {};
    }
    const auto separator = value.find("://");
    if (separator == std::string_view::npos) {
        return {};
    }
    const auto scheme = value.substr(0, separator);
    if (scheme != "http" && scheme != "https") {
        return {};
    }
    const auto rest = value.substr(separator + 3);
    const auto end = rest.find_first_of("/?");
    const auto authority = rest.substr(0, end);
    if (authority.empty() || authority.find_first_of("@\\") != std::string_view::npos) {
        return {};
    }
    const auto colon = authority.find(':');
    const auto hostname = authority.substr(0, colon);
    if (!route_policy::exact_hostname(hostname)) {
        return {};
    }
    if (colon != std::string_view::npos) {
        const auto port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::ranges::all_of(port, [](char c) { return c >= '0' && c <= '9'; }) ||
            std::stoi(std::string(port)) < 1 || std::stoi(std::string(port)) > 65535) {
            return {};
        }
    }
    auto target = end == std::string_view::npos ? std::string("/") : std::string(rest.substr(end));
    if (target.starts_with('?')) {
        target.insert(target.begin(), '/');
    }
    url result{.scheme_ = std::string(scheme), .authority_ = std::string(authority), .hostname_ = route_policy::hostname(hostname), .target_ = std::move(target)};
    result.authority_ = result.hostname_;
    if (colon != std::string_view::npos) {
        const auto port = std::stoi(std::string(authority.substr(colon + 1)));
        if (!((scheme == "http" && port == 80) || (scheme == "https" && port == 443))) {
            result.authority_ += ":" + std::to_string(port);
        }
    }
    result.value_ = result.scheme_ + "://" + result.authority_ + result.target_;
    return result;
}
}  // namespace flexedge::cache
