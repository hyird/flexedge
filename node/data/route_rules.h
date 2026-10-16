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
#include "node/proto/website_validation.h"

namespace flexedge::node {

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
