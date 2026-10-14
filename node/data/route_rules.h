#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/route_match.h"
#include "common/route_policy.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

using route_patterns = std::unordered_map<const v2::RouteRule*, std::shared_ptr<const RE2>>;

inline std::shared_ptr<const RE2> route_pattern(const v2::RouteRule& rule, const route_patterns* patterns) {
    if (rule.match_type() != "regex") {
        return {};
    }
    if (patterns) {
        const auto found = patterns->find(&rule);
        return found == patterns->end() ? nullptr : found->second;
    }
    // Direct policy-unit callers may omit the compiled configuration.
    return flexedge::route_policy::compile_pattern(rule.path());
}

inline bool route_header_equals(std::string_view value, std::string_view expected) noexcept {
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

inline bool route_header_name(std::string_view value) noexcept {
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

inline bool route_header_value(std::string_view value) noexcept {
    return value.size() <= 4096 && value.find('\r') == std::string_view::npos &&
           value.find('\n') == std::string_view::npos;
}

inline bool route_mutation_header(std::string_view value) noexcept {
    if (!route_header_name(value)) {
        return false;
    }
    return !route_header_equals(value, "Connection") && !route_header_equals(value, "Content-Length") &&
           !route_header_equals(value, "Host") && !route_header_equals(value, "Keep-Alive") &&
           !route_header_equals(value, "Proxy-Connection") && !route_header_equals(value, "TE") &&
           !route_header_equals(value, "Strict-Transport-Security") &&
           !route_header_equals(value, "Trailer") && !route_header_equals(value, "Transfer-Encoding") &&
           !route_header_equals(value, "Upgrade");
}

inline bool route_path(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 2048 && value.front() == '/' &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

inline bool route_redirect_url(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 2048 &&
           (value.front() == '/' || value.starts_with("http://") ||
               value.starts_with("https://")) &&
           std::ranges::none_of(value, [](unsigned char ch) { return std::isspace(ch) != 0; }) &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

inline bool route_methods(const google::protobuf::RepeatedPtrField<std::string>& methods) {
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

inline bool route_headers(const google::protobuf::RepeatedPtrField<v2::HeaderMutation>& headers) {
    if (headers.size() > 20) {
        return false;
    }
    std::unordered_set<std::string> names;
    for (const auto& header : headers) {
        if (!route_mutation_header(header.name()) || !route_header_value(header.value())) {
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

inline bool route_origin_ids(const google::protobuf::RepeatedPtrField<std::string>& ids,
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

inline bool route_origin_group(const v2::RouteRule& rule, const v2::Website& website) {
    if (!rule.origin_ids().empty()) {
        return route_origin_ids(rule.origin_ids(), website);
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

inline void validate_route_rules(const v2::Website& website, route_patterns* patterns = nullptr) {
    if (website.route_rules_size() > 100) {
        throw std::runtime_error("too many website route rules");
    }
    std::unordered_set<std::string_view> ids;
    for (const auto& rule : website.route_rules()) {
        namespace policy = flexedge::route_policy;
        std::shared_ptr<const RE2> pattern;
        if (rule.match_type() == "regex") {
            pattern = policy::compile_pattern(rule.path());
            if (!pattern || !policy::capture_template(rule.rewrite_path(), pattern->NumberOfCapturingGroups()) ||
                !policy::capture_template(rule.redirect_url(), pattern->NumberOfCapturingGroups())) {
                throw std::runtime_error("invalid route regex or capture template");
            }
            if (patterns) {
                patterns->emplace(&rule, pattern);
            }
        }
        if (rule.conditions_size() > 20) {
            throw std::runtime_error("too many route conditions");
        }
        for (const auto& condition : rule.conditions()) {
            if (!policy::condition_options(condition.source(), condition.name(), condition.op(), condition.value())) {
                throw std::runtime_error("invalid route condition");
            }
        }
        const auto& mode = rule.rewrite_mode();
        if (!policy::clean_text(rule.name(), 100) || rule.description().size() > 1000 ||
            rule.hostnames_size() > 100 || !policy::rewrite_mode(mode) ||
            !policy::query_mode(rule.query_mode()) || !policy::query_string(rule.query_string()) ||
            (rule.query_mode() != "replace" && !rule.query_string().empty()) ||
            ((mode == "none" || mode == "strip_prefix") && !rule.rewrite_path().empty()) ||
            ((mode == "replace_path" || mode == "replace_prefix") && rule.rewrite_path().empty()) ||
            ((mode == "strip_prefix" || mode == "replace_prefix") &&
                (rule.match_type() != "prefix" || !policy::path_only(rule.path()) ||
                    (mode == "replace_prefix" && !policy::path_only(rule.rewrite_path())))) ||
            (rule.action() == "redirect" && !mode.empty() && mode != "none")) {
            throw std::runtime_error("invalid route policy options");
        }
        std::unordered_set<std::string> hostnames;
        for (const auto& host : rule.hostnames()) {
            if (!policy::exact_hostname(host) || !hostnames.emplace(policy::hostname(host)).second) {
                throw std::runtime_error("invalid route hostname filter");
            }
        }
        if (rule.id().empty() || !ids.emplace(rule.id()).second ||
            !policy::valid_match_type(rule.match_type()) ||
            (rule.match_type() == "suffix"
                    ? (rule.path().empty() || !policy::clean_text(rule.path(), 2048) || rule.path().find_first_of(" ?#") != std::string::npos)
                    : (rule.match_type() != "regex" && !route_path(rule.path()))) ||
            !route_methods(rule.methods()) ||
            (rule.action() != "proxy" && rule.action() != "redirect" && rule.action() != "rewrite") ||
            !route_headers(rule.request_headers()) || !route_headers(rule.response_headers())) {
            throw std::runtime_error("invalid website route rule");
        }
        if (rule.action() == "rewrite") {
            if (!rule.origin_ids().empty() || !rule.origin_group().empty() ||
                !rule.redirect_url().empty() || rule.redirect_status() != 0 ||
                !rule.request_headers().empty() || !rule.response_headers().empty() ||
                (!rule.rewrite_path().empty() && !policy::path_only(rule.rewrite_path()))) {
                throw std::runtime_error("invalid rewrite route rule");
            }
        } else if (rule.action() == "proxy") {
            if (!route_origin_group(rule, website) ||
                (!rule.rewrite_path().empty() && !policy::path_only(rule.rewrite_path())) ||
                !rule.redirect_url().empty() || rule.redirect_status() != 0) {
                throw std::runtime_error("invalid proxy route rule");
            }
        } else if (!rule.origin_ids().empty() || !rule.origin_group().empty() ||
                   rule.rewrite_path().empty() == false || !route_redirect_url(rule.redirect_url()) ||
                   !policy::redirect_status(rule.redirect_status())) {
            throw std::runtime_error("invalid redirect route rule");
        }
    }
}

[[nodiscard]] inline bool route_method_matches(const v2::RouteRule& rule,
    std::string_view method) noexcept {
    return rule.methods().empty() ||
           std::ranges::any_of(rule.methods(),
               [&](const auto& candidate) { return candidate == method; });
}

[[nodiscard]] inline bool route_matches(const v2::RouteRule& rule, std::string_view method,
    std::string_view target, std::string_view host = {},
    std::span<const std::pair<std::string_view, std::string_view>> headers = {},
    const route_patterns* patterns = nullptr) {
    namespace policy = flexedge::route_policy;
    const auto hostname = flexedge::route_policy::hostname(host);
    const auto path_end = target.find('?');
    const auto path = target.substr(0, path_end);
    std::optional<policy::query_values> query;
    bool query_parsed = false;
    if (!rule.enabled() || !route_method_matches(rule, method)) {
        return false;
    }
    if (!rule.hostnames().empty() && std::ranges::none_of(rule.hostnames(), [&](const auto& candidate) {
            return flexedge::route_policy::hostname(candidate) == hostname;
        })) {
        return false;
    }
    bool conditions_match = true;
    for (const auto& condition : rule.conditions()) {
        if (condition.source() == "query" && !query_parsed) {
            query = policy::get_query_values(target);
            query_parsed = true;
        }
        if (!policy::condition_matches(condition.source(), condition.name(), condition.op(), condition.value(), headers, query)) {
            conditions_match = false;
            break;
        }
    }
    if (!conditions_match) {
        return false;
    }
    bool path_matches = false;
    if (rule.match_type() == "regex") {
        const auto pattern = route_pattern(rule, patterns);
        path_matches = pattern && RE2::PartialMatch(absl::string_view(path.data(), path.size()), *pattern);
    } else if (rule.match_type() == "suffix") {
        path_matches = path.ends_with(rule.path());
    } else if (rule.match_type() == "exact") {
        path_matches = path == rule.path();
    } else {
        path_matches = path.starts_with(rule.path()) &&
                       (rule.path() == "/" || rule.path().ends_with('/') || path.size() == rule.path().size() || path[rule.path().size()] == '/');
    }
    if (!path_matches) {
        return false;
    }
    return true;
}

[[nodiscard]] inline const v2::RouteRule* matched_route_rule(const v2::Website& website,
    std::string_view method, std::string_view target, std::string_view host = {},
    std::span<const std::pair<std::string_view, std::string_view>> headers = {},
    const route_patterns* patterns = nullptr) {
    for (const auto& rule : website.route_rules()) {
        if (route_matches(rule, method, target, host, headers, patterns)) {
            return &rule;
        }
    }
    return nullptr;
}

[[nodiscard]] inline std::string route_target(std::string_view target, const v2::RouteRule* rule,
    const route_patterns* patterns = nullptr) {
    if (rule == nullptr) {
        return std::string(target);
    }
    const auto path = target.substr(0, target.find('?'));
    auto result = flexedge::route_policy::rewrite_path(path, rule->path(), rule->rewrite_path(), rule->rewrite_mode());
    if (rule->match_type() == "regex" && (rule->rewrite_mode() == "replace_path" ||
                                             (rule->rewrite_mode().empty() && !rule->rewrite_path().empty()))) {
        const auto pattern = route_pattern(*rule, patterns);
        if (!pattern) {
            return {};
        }
        auto expanded = flexedge::route_policy::expand_captures(result, path, *pattern);
        if (!expanded) {
            return {};
        }
        result = std::move(*expanded);
    }
    return flexedge::route_policy::apply_query(result, target, rule->query_mode(), rule->query_string());
}

[[nodiscard]] inline std::string route_redirect_location(std::string_view target,
    const v2::RouteRule& rule,
    const route_patterns* patterns = nullptr) {
    auto destination = rule.redirect_url();
    if (rule.match_type() == "regex") {
        const auto pattern = route_pattern(rule, patterns);
        if (!pattern) {
            return {};
        }
        auto expanded = flexedge::route_policy::expand_captures(destination, target.substr(0, target.find('?')), *pattern);
        if (!expanded) {
            return {};
        }
        destination = std::move(*expanded);
    }
    return flexedge::route_policy::apply_query(destination, target, rule.query_mode(), rule.query_string());
}

// Each phase matches immutable input. Own the resolved policy through origin retries.
[[nodiscard]] inline std::optional<v2::RouteRule> evaluate_route_rules(
    const v2::Website& website, std::string_view method, std::string_view target,
    std::string_view host = {},
    std::span<const std::pair<std::string_view, std::string_view>> headers = {},
    const route_patterns* patterns = nullptr) {
    for (const auto& rule : website.route_rules()) {
        if (rule.action() != "redirect" || !route_matches(rule, method, target, host, headers, patterns)) {
            continue;
        }
        const auto location = route_redirect_location(target, rule, patterns);
        if (location.empty()) {
            return std::nullopt;
        }
        auto result = rule;
        result.set_match_type("exact");
        result.set_redirect_url(location);
        // Location is final; callers must not apply the incoming query again.
        result.set_query_mode("preserve");
        result.clear_query_string();
        return result;
    }
    std::string rewritten(target);
    for (const auto& rule : website.route_rules()) {
        if (rule.action() == "redirect" || !route_matches(rule, method, target, host, headers, patterns)) {
            continue;
        }
        const auto mode = rule.rewrite_mode().empty() ? (rule.rewrite_path().empty() ? "none" : "replace_path") : rule.rewrite_mode();
        if (mode != "none") {
            const auto candidate = route_target(target, &rule, patterns);
            if (candidate.empty()) {
                return std::nullopt;
            }
            // Path-only changes retain the query selected by an earlier rule.
            rewritten = candidate.substr(0, candidate.find('?')) +
                        (rewritten.find('?') == std::string::npos ? "" : rewritten.substr(rewritten.find('?')));
        }
        if (rule.query_mode() == "drop" || rule.query_mode() == "replace") {
            rewritten = flexedge::route_policy::apply_query(rewritten, target, rule.query_mode(), rule.query_string());
        }
    }
    v2::RouteRule result;
    result.set_action("proxy");
    result.set_origin_group(website.default_origin_group());
    auto merge_headers = [](auto* destination, const auto& changes) {
        for (const auto& change : changes) {
            bool replaced = false;
            for (auto& existing : *destination) {
                if (route_header_equals(existing.name(), change.name())) {
                    existing = change;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                *destination->Add() = change;
            }
        }
    };
    for (const auto& rule : website.route_rules()) {
        if (rule.action() != "proxy" || !route_matches(rule, method, rewritten, host, headers, patterns)) {
            continue;
        }
        result.set_id(rule.id());
        result.set_origin_group(rule.origin_group());
        *result.mutable_origin_ids() = rule.origin_ids();
        merge_headers(result.mutable_request_headers(), rule.request_headers());
        merge_headers(result.mutable_response_headers(), rule.response_headers());
    }
    // Existing codecs apply this literal result exactly once, including on retries.
    const auto query_at = rewritten.find('?');
    result.set_match_type("exact");
    result.set_rewrite_mode("replace_path");
    result.set_rewrite_path(rewritten);
    result.set_query_mode(query_at == std::string::npos ? "drop" : "preserve");
    return result;
}

template <typename header_range_type>
inline void
apply_route_headers(header_range_type& headers,
    const google::protobuf::RepeatedPtrField<v2::HeaderMutation>& mutations) {
    if (mutations.empty()) {
        return;
    }
    headers.erase(
        std::remove_if(headers.begin(), headers.end(),
            [&](const auto& header) {
                return std::ranges::any_of(mutations, [&](const auto& mutation) {
                    return route_header_equals(header.first, mutation.name());
                });
            }),
        headers.end());
    for (const auto& mutation : mutations) {
        headers.emplace_back(mutation.name(), mutation.value());
    }
}

}  // namespace flexedge::node
