#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

inline bool routeHeaderEquals(std::string_view value, std::string_view expected) noexcept {
    if (value.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(expected[index]))) {
            return false;
        }
    }
    return true;
}

inline bool routeHeaderName(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const auto ch : value) {
        const auto value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && value != '!' && value != '#' && value != '$' && value != '%' &&
            value != '&' && value != '\'' && value != '*' && value != '+' && value != '-' &&
            value != '.' && value != '^' && value != '_' && value != '`' && value != '|' &&
            value != '~') {
            return false;
        }
    }
    return true;
}

inline bool routeHeaderValue(std::string_view value) noexcept {
    return value.size() <= 4096 && value.find('\r') == std::string_view::npos &&
           value.find('\n') == std::string_view::npos;
}

inline bool routeMutationHeader(std::string_view value) noexcept {
    if (!routeHeaderName(value)) {
        return false;
    }
    return !routeHeaderEquals(value, "Connection") && !routeHeaderEquals(value, "Content-Length") &&
           !routeHeaderEquals(value, "Host") && !routeHeaderEquals(value, "Keep-Alive") &&
           !routeHeaderEquals(value, "Proxy-Connection") && !routeHeaderEquals(value, "TE") &&
           !routeHeaderEquals(value, "Strict-Transport-Security") &&
           !routeHeaderEquals(value, "Trailer") && !routeHeaderEquals(value, "Transfer-Encoding") &&
           !routeHeaderEquals(value, "Upgrade");
}

inline bool routePath(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 2048 && value.front() == '/' &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

inline bool routeRedirectUrl(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 2048 &&
           (value.front() == '/' || value.starts_with("http://") ||
            value.starts_with("https://")) &&
           std::ranges::none_of(value, [](unsigned char ch) { return std::isspace(ch) != 0; }) &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

inline bool routeMethods(const google::protobuf::RepeatedPtrField<std::string>& methods) {
    if (methods.size() > 8) {
        return false;
    }
    std::unordered_set<std::string_view> seen;
    for (const auto& method : methods) {
        if ((method != "GET" && method != "HEAD" && method != "POST" && method != "PUT" &&
             method != "PATCH" && method != "DELETE" && method != "OPTIONS") ||
            !seen.emplace(method).second) {
            return false;
        }
    }
    return true;
}

inline bool routeHeaders(const google::protobuf::RepeatedPtrField<v2::HeaderMutation>& headers) {
    if (headers.size() > 20) {
        return false;
    }
    std::unordered_set<std::string> names;
    for (const auto& header : headers) {
        if (!routeMutationHeader(header.name()) || !routeHeaderValue(header.value())) {
            return false;
        }
        std::string normalized(header.name());
        std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!names.emplace(std::move(normalized)).second) {
            return false;
        }
    }
    return true;
}

inline bool routeOriginIds(const google::protobuf::RepeatedPtrField<std::string>& ids,
                           const v2::Website& website) {
    if (ids.size() > 20) {
        return false;
    }
    std::unordered_set<std::string_view> selected;
    std::unordered_set<std::string_view> enabled;
    for (const auto& origin : website.origins()) {
        if (origin.enabled()) {
            enabled.emplace(origin.id());
        }
    }
    for (const auto& id : ids) {
        if (id.empty() || !selected.emplace(id).second || !enabled.contains(id)) {
            return false;
        }
    }
    return true;
}

inline bool routeOriginGroup(const v2::RouteRule& rule, const v2::Website& website) {
    if (!rule.origin_ids().empty()) {
        return routeOriginIds(rule.origin_ids(), website);
    }
    if (rule.origin_group().empty()) {
        return false;
    }
    for (const auto& origin : website.origins()) {
        const auto group =
            origin.group().empty() ? std::string_view{"default"} : std::string_view(origin.group());
        if (group == rule.origin_group() && origin.enabled() && origin.role() == "primary") {
            return true;
        }
    }
    return false;
}

inline void validateRouteRules(const v2::Website& website) {
    if (website.route_rules_size() > 100) {
        throw std::runtime_error("too many website route rules");
    }
    std::unordered_set<std::string_view> ids;
    for (const auto& rule : website.route_rules()) {
        if (rule.id().empty() || !ids.emplace(rule.id()).second ||
            (rule.match_type() != "exact" && rule.match_type() != "prefix") ||
            !routePath(rule.path()) || !routeMethods(rule.methods()) ||
            (rule.action() != "proxy" && rule.action() != "redirect") ||
            !routeHeaders(rule.request_headers()) || !routeHeaders(rule.response_headers())) {
            throw std::runtime_error("invalid website route rule");
        }
        if (rule.action() == "proxy") {
            if (!routeOriginGroup(rule, website) ||
                (!rule.rewrite_path().empty() && !routePath(rule.rewrite_path())) ||
                !rule.redirect_url().empty() || rule.redirect_status() != 0) {
                throw std::runtime_error("invalid proxy route rule");
            }
        } else if (!rule.origin_ids().empty() || !rule.origin_group().empty() ||
                   rule.rewrite_path().empty() == false || !routeRedirectUrl(rule.redirect_url()) ||
                   (rule.redirect_status() != 301 && rule.redirect_status() != 302)) {
            throw std::runtime_error("invalid redirect route rule");
        }
    }
}

[[nodiscard]] inline bool routeMethodMatches(const v2::RouteRule& rule,
                                             std::string_view method) noexcept {
    return rule.methods().empty() ||
           std::ranges::any_of(rule.methods(),
                               [&](const auto& candidate) { return candidate == method; });
}

[[nodiscard]] inline const v2::RouteRule* matchedRouteRule(const v2::Website& website,
                                                           std::string_view method,
                                                           std::string_view target) noexcept {
    const auto pathEnd = target.find('?');
    const auto path = target.substr(0, pathEnd);
    const v2::RouteRule* matched = nullptr;
    for (const auto& rule : website.route_rules()) {
        if (!rule.enabled() || !routeMethodMatches(rule, method)) {
            continue;
        }
        const bool pathMatches =
            rule.match_type() == "exact"
                ? path == rule.path()
                : path.starts_with(rule.path()) &&
                      (rule.path() == "/" || rule.path().ends_with('/') ||
                       path.size() == rule.path().size() || path[rule.path().size()] == '/');
        if (!pathMatches) {
            continue;
        }
        if (matched == nullptr ||
            (rule.match_type() == "exact" && matched->match_type() != "exact") ||
            (rule.match_type() == matched->match_type() &&
             rule.path().size() > matched->path().size())) {
            matched = &rule;
        }
    }
    return matched;
}

[[nodiscard]] inline std::string routeTarget(std::string_view target, const v2::RouteRule* rule) {
    if (rule == nullptr || rule->rewrite_path().empty()) {
        return std::string(target);
    }
    std::string result(rule->rewrite_path());
    if (const auto query = target.find('?'); query != std::string_view::npos) {
        result.append(target.substr(query));
    }
    return result;
}

[[nodiscard]] inline std::string routeRedirectLocation(std::string_view target,
                                                       const v2::RouteRule& rule) {
    std::string result(rule.redirect_url());
    if (!result.contains('?')) {
        if (const auto query = target.find('?'); query != std::string_view::npos) {
            result.append(target.substr(query));
        }
    }
    return result;
}

template <typename HeaderRange>
inline void
applyRouteHeaders(HeaderRange& headers,
                  const google::protobuf::RepeatedPtrField<v2::HeaderMutation>& mutations) {
    if (mutations.empty()) {
        return;
    }
    headers.erase(
        std::remove_if(headers.begin(), headers.end(),
                       [&](const auto& header) {
                           return std::ranges::any_of(mutations, [&](const auto& mutation) {
                               return routeHeaderEquals(header.first, mutation.name());
                           });
                       }),
        headers.end());
    for (const auto& mutation : mutations) {
        headers.emplace_back(mutation.name(), mutation.value());
    }
}

} // namespace flexedge::node
