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
#include <unordered_map>
#include <utility>
#include <vector>

#include "node/proto/edge_control.pb.h"
#include "common/route_policy.h"
#include "common/route_match.h"

namespace flexedge::node {

using RoutePatterns = std::unordered_map<const v2::RouteRule*, std::shared_ptr<const RE2>>;

inline std::shared_ptr<const RE2> routePattern(const v2::RouteRule& rule, const RoutePatterns* patterns) {
    if (rule.match_type() != "regex") return {};
    if (patterns) {
        const auto found = patterns->find(&rule);
        return found == patterns->end() ? nullptr : found->second;
    }
    // Direct policy-unit callers may omit the compiled configuration.
    return flexedge::route_policy::compilePattern(rule.path());
}

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
        const auto character = static_cast<unsigned char>(ch);
        if (!std::isalnum(character) && character != '!' && character != '#' &&
            character != '$' && character != '%' && character != '&' && character != '\'' &&
            character != '*' && character != '+' && character != '-' && character != '.' &&
            character != '^' && character != '_' && character != '`' && character != '|' &&
            character != '~') {
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

inline void validateRouteRules(const v2::Website& website, RoutePatterns* patterns = nullptr) {
    if (website.route_rules_size() > 100) {
        throw std::runtime_error("too many website route rules");
    }
    std::unordered_set<std::string_view> ids;
    for (const auto& rule : website.route_rules()) {
        namespace policy = flexedge::route_policy;
        std::shared_ptr<const RE2> pattern;
        if (rule.match_type() == "regex") {
            pattern = policy::compilePattern(rule.path());
            if (!pattern || !policy::captureTemplate(rule.rewrite_path(), pattern->NumberOfCapturingGroups()) ||
                !policy::captureTemplate(rule.redirect_url(), pattern->NumberOfCapturingGroups()))
                throw std::runtime_error("invalid route regex or capture template");
            if (patterns) patterns->emplace(&rule, pattern);
        }
        if (rule.conditions_size() > 20) throw std::runtime_error("too many route conditions");
        for (const auto& condition : rule.conditions()) {
            if (!policy::conditionOptions(condition.source(), condition.name(), condition.op(), condition.value()))
                throw std::runtime_error("invalid route condition");
        }
        const auto& mode = rule.rewrite_mode();
        if (!policy::cleanText(rule.name(), 100) || rule.description().size() > 1000 ||
            rule.hostnames_size() > 100 || !policy::rewriteMode(mode) ||
            !policy::queryMode(rule.query_mode()) || !policy::queryString(rule.query_string()) ||
            (rule.query_mode() != "replace" && !rule.query_string().empty()) ||
            ((mode == "none" || mode == "strip_prefix") && !rule.rewrite_path().empty()) ||
            ((mode == "replace_path" || mode == "replace_prefix") && rule.rewrite_path().empty()) ||
            ((mode == "strip_prefix" || mode == "replace_prefix") &&
             (rule.match_type() != "prefix" || !policy::pathOnly(rule.path()) ||
              (mode == "replace_prefix" && !policy::pathOnly(rule.rewrite_path())))) ||
            (rule.action() == "redirect" && !mode.empty() && mode != "none")) {
            throw std::runtime_error("invalid route policy options");
        }
        std::unordered_set<std::string> hostnames;
        for (const auto& host : rule.hostnames()) {
            if (!policy::exactHostname(host) || !hostnames.emplace(policy::hostname(host)).second) {
                throw std::runtime_error("invalid route hostname filter");
            }
        }
        if (rule.id().empty() || !ids.emplace(rule.id()).second ||
            policy::matchPriority(rule.match_type()) == 0 ||
            (rule.match_type() == "suffix"
                ? (rule.path().empty() || !policy::cleanText(rule.path(), 2048) || rule.path().find_first_of(" ?#") != std::string::npos)
                : (rule.match_type() != "regex" && !routePath(rule.path()))) || !routeMethods(rule.methods()) ||
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
                   !policy::redirectStatus(rule.redirect_status())) {
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
                                                           std::string_view target,
                                                           std::string_view host = {},
                                                           std::span<const std::pair<std::string_view, std::string_view>> headers = {},
                                                           const RoutePatterns* patterns = nullptr) {
    namespace policy = flexedge::route_policy;
    const auto hostname = flexedge::route_policy::hostname(host);
    const auto pathEnd = target.find('?');
    const auto path = target.substr(0, pathEnd);
    const v2::RouteRule* matched = nullptr;
    std::optional<policy::QueryValues> query;
    bool queryParsed = false;
    for (const auto& rule : website.route_rules()) {
        if (!rule.enabled() || !routeMethodMatches(rule, method)) {
            continue;
        }
        if (!rule.hostnames().empty() && std::ranges::none_of(rule.hostnames(), [&](const auto& candidate) {
                return flexedge::route_policy::hostname(candidate) == hostname;
            })) continue;
        bool conditionsMatch = true;
        for (const auto& condition : rule.conditions()) {
            if (condition.source() == "query" && !queryParsed) {
                query = policy::queryValues(target); queryParsed = true;
            }
            if (!policy::conditionMatches(condition.source(), condition.name(), condition.op(), condition.value(), headers, query)) {
                conditionsMatch = false; break;
            }
        }
        if (!conditionsMatch) continue;
        bool pathMatches = false;
        if (rule.match_type() == "regex") {
            const auto pattern = routePattern(rule, patterns);
            pathMatches = pattern && RE2::PartialMatch(absl::string_view(path.data(), path.size()), *pattern);
        } else if (rule.match_type() == "suffix") pathMatches = path.ends_with(rule.path());
        else if (rule.match_type() == "exact") pathMatches = path == rule.path();
        else pathMatches = path.starts_with(rule.path()) &&
            (rule.path() == "/" || rule.path().ends_with('/') || path.size() == rule.path().size() || path[rule.path().size()] == '/');
        if (!pathMatches) {
            continue;
        }
        if (matched == nullptr ||
            policy::matchPriority(rule.match_type()) > policy::matchPriority(matched->match_type()) ||
            (rule.match_type() == matched->match_type() && rule.match_type() != "regex" &&
             rule.path().size() > matched->path().size())) {
            matched = &rule;
        }
    }
    return matched;
}

[[nodiscard]] inline std::string routeTarget(std::string_view target, const v2::RouteRule* rule,
                                            const RoutePatterns* patterns = nullptr) {
    if (rule == nullptr) {
        return std::string(target);
    }
    const auto path = target.substr(0, target.find('?'));
    auto result = flexedge::route_policy::rewritePath(path, rule->path(), rule->rewrite_path(), rule->rewrite_mode());
    if (rule->match_type() == "regex" && (rule->rewrite_mode() == "replace_path" ||
                                        (rule->rewrite_mode().empty() && !rule->rewrite_path().empty()))) {
        const auto pattern = routePattern(*rule, patterns);
        if (!pattern) return {};
        auto expanded = flexedge::route_policy::expandCaptures(result, path, *pattern);
        if (!expanded) return {};
        result = std::move(*expanded);
    }
    return flexedge::route_policy::applyQuery(result, target, rule->query_mode(), rule->query_string());
}

[[nodiscard]] inline std::string routeRedirectLocation(std::string_view target,
                                                       const v2::RouteRule& rule,
                                                       const RoutePatterns* patterns = nullptr) {
    auto destination = rule.redirect_url();
    if (rule.match_type() == "regex") {
        const auto pattern = routePattern(rule, patterns);
        if (!pattern) return {};
        auto expanded = flexedge::route_policy::expandCaptures(destination, target.substr(0, target.find('?')), *pattern);
        if (!expanded) return {};
        destination = std::move(*expanded);
    }
    return flexedge::route_policy::applyQuery(destination, target, rule.query_mode(), rule.query_string());
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
