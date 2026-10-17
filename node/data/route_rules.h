#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

struct route_match_context final {
    route_match_context(std::string_view method, std::string_view target,
        std::string_view host = {},
        std::span<const std::pair<std::string_view, std::string_view>> headers = {},
        const route_patterns* patterns = nullptr)
        : method_(method),
          target_(target),
          path_(target.substr(0, target.find('?'))),
          hostname_(flexedge::route_policy::hostname(host)),
          headers_(headers),
          patterns_(patterns) {}

    [[nodiscard]] route_match_context with_target(std::string_view target) const {
        route_match_context next(*this);
        next.target_ = target;
        next.path_ = target.substr(0, target.find('?'));
        next.query_.reset();
        next.query_parsed_ = false;
        return next;
    }

    [[nodiscard]] const std::optional<flexedge::route_policy::query_values>& query() const {
        if (!query_parsed_) {
            query_ = flexedge::route_policy::get_query_values(target_);
            query_parsed_ = true;
        }
        return query_;
    }

    std::string_view method_{};
    std::string_view target_{};
    std::string_view path_{};
    std::string hostname_;
    std::span<const std::pair<std::string_view, std::string_view>> headers_{};
    const route_patterns* patterns_{};

private:
    mutable std::optional<flexedge::route_policy::query_values> query_{};
    mutable bool query_parsed_{false};
};

[[nodiscard]] inline bool route_method_matches(const v2::RouteRule& rule,
    std::string_view method) noexcept {
    return rule.methods().empty() ||
           std::ranges::any_of(rule.methods(),
               [&](const auto& candidate) { return candidate == method; });
}

[[nodiscard]] inline bool route_matches(const v2::RouteRule& rule,
    const route_match_context& context) {
    namespace policy = flexedge::route_policy;
    if (!rule.enabled() || !route_method_matches(rule, context.method_)) {
        return false;
    }
    if (!rule.hostnames().empty() &&
        std::ranges::none_of(rule.hostnames(), [&](const auto& candidate) {
            return policy::hostname(candidate) == context.hostname_;
        })) {
        return false;
    }
    static const std::optional<policy::query_values> empty_query{};
    for (const auto& condition : rule.conditions()) {
        const auto& query = condition.source() == "query" ? context.query() : empty_query;
        if (!policy::condition_matches(condition.source(), condition.name(), condition.op(),
                condition.value(), context.headers_, query)) {
            return false;
        }
    }
    if (rule.match_type() == "regex") {
        const auto pattern = route_pattern(rule, context.patterns_);
        return pattern &&
               RE2::PartialMatch(
                   absl::string_view(context.path_.data(), context.path_.size()), *pattern);
    }
    if (rule.match_type() == "suffix") {
        return context.path_.ends_with(rule.path());
    }
    if (rule.match_type() == "exact") {
        return context.path_ == rule.path();
    }
    return context.path_.starts_with(rule.path()) &&
           (rule.path() == "/" || rule.path().ends_with('/') ||
               context.path_.size() == rule.path().size() ||
               context.path_[rule.path().size()] == '/');
}

class evaluated_route_policy final {
public:
    [[nodiscard]] const std::string& id() const noexcept {
        return id_;
    }
    [[nodiscard]] const std::string& action() const noexcept {
        return action_;
    }
    [[nodiscard]] const std::string& origin_group() const noexcept {
        return origin_group_;
    }
    [[nodiscard]] const std::vector<std::string>& origin_ids() const noexcept {
        return origin_ids_;
    }
    [[nodiscard]] const std::string& redirect_url() const noexcept {
        return redirect_url_;
    }
    [[nodiscard]] std::uint32_t redirect_status() const noexcept {
        return redirect_status_;
    }
    [[nodiscard]] const std::string& rewritten_target() const noexcept {
        return rewritten_target_;
    }
    [[nodiscard]] const std::vector<v2::HeaderMutation>& request_headers() const noexcept {
        return request_headers_;
    }
    [[nodiscard]] const std::vector<v2::HeaderMutation>& response_headers() const noexcept {
        return response_headers_;
    }

    std::string id_;
    std::string action_{"proxy"};
    std::string origin_group_;
    std::vector<std::string> origin_ids_;
    std::string redirect_url_;
    std::uint32_t redirect_status_{};
    std::string rewritten_target_;
    std::vector<v2::HeaderMutation> request_headers_;
    std::vector<v2::HeaderMutation> response_headers_;
};

namespace detail {

[[nodiscard]] inline std::string rewrite_route_target(std::string_view target, const v2::RouteRule& rule,
    const route_patterns* patterns = nullptr) {
    const auto path = target.substr(0, target.find('?'));
    auto result = flexedge::route_policy::rewrite_path(path, rule.path(), rule.rewrite_path(), rule.rewrite_mode());
    if (rule.match_type() == "regex" && (rule.rewrite_mode() == "replace_path" ||
                                            (rule.rewrite_mode().empty() && !rule.rewrite_path().empty()))) {
        const auto pattern = route_pattern(rule, patterns);
        if (!pattern) {
            return {};
        }
        auto expanded = flexedge::route_policy::expand_captures(result, path, *pattern);
        if (!expanded) {
            return {};
        }
        result = std::move(*expanded);
    }
    return flexedge::route_policy::apply_query(result, target, rule.query_mode(), rule.query_string());
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

}  // namespace detail

// Each phase matches immutable input. Own the resolved policy through origin retries.
[[nodiscard]] inline std::optional<evaluated_route_policy> evaluate_route_rules(
    const v2::Website& website, std::string_view method, std::string_view target,
    std::string_view host = {},
    std::span<const std::pair<std::string_view, std::string_view>> headers = {},
    const route_patterns* patterns = nullptr) {
    const route_match_context incoming(method, target, host, headers, patterns);
    for (const auto& rule : website.route_rules()) {
        if (rule.action() != "redirect" || !route_matches(rule, incoming)) {
            continue;
        }
        auto location = detail::route_redirect_location(target, rule, patterns);
        if (location.empty()) {
            return std::nullopt;
        }
        evaluated_route_policy result;
        result.id_ = rule.id();
        result.action_ = "redirect";
        result.redirect_url_ = std::move(location);
        result.redirect_status_ = rule.redirect_status();
        result.response_headers_.assign(
            rule.response_headers().begin(), rule.response_headers().end());
        return result;
    }
    std::string rewritten(target);
    for (const auto& rule : website.route_rules()) {
        if (rule.action() == "redirect" || !route_matches(rule, incoming)) {
            continue;
        }
        const auto mode = rule.rewrite_mode().empty()
                              ? (rule.rewrite_path().empty() ? "none" : "replace_path")
                              : rule.rewrite_mode();
        if (mode != "none") {
            const auto candidate = detail::rewrite_route_target(target, rule, patterns);
            if (candidate.empty()) {
                return std::nullopt;
            }
            // Path-only changes retain the query selected by an earlier rule.
            rewritten = candidate.substr(0, candidate.find('?')) +
                        (rewritten.find('?') == std::string::npos
                                ? ""
                                : rewritten.substr(rewritten.find('?')));
        }
        if (rule.query_mode() == "drop" || rule.query_mode() == "replace") {
            rewritten = flexedge::route_policy::apply_query(
                rewritten, target, rule.query_mode(), rule.query_string());
        }
    }
    evaluated_route_policy result;
    result.action_ = "proxy";
    result.origin_group_ = website.default_origin_group();
    result.rewritten_target_ = std::move(rewritten);
    const auto rewritten_context = result.rewritten_target_ == target
                                       ? incoming
                                       : incoming.with_target(result.rewritten_target_);
    auto merge_headers = [](std::vector<v2::HeaderMutation>& destination, const auto& changes) {
        for (const auto& change : changes) {
            const auto found = std::ranges::find_if(destination, [&](const auto& existing) {
                return flexedge::route_policy::ascii_equal(existing.name(), change.name());
            });
            if (found != destination.end()) {
                *found = change;
            } else {
                destination.push_back(change);
            }
        }
    };
    for (const auto& rule : website.route_rules()) {
        if (rule.action() != "proxy" || !route_matches(rule, rewritten_context)) {
            continue;
        }
        result.id_ = rule.id();
        result.origin_group_ = rule.origin_group();
        result.origin_ids_.assign(rule.origin_ids().begin(), rule.origin_ids().end());
        merge_headers(result.request_headers_, rule.request_headers());
        merge_headers(result.response_headers_, rule.response_headers());
    }
    return result;
}

template <typename header_range_type, typename mutation_range_type>
inline void apply_route_headers(header_range_type& headers, const mutation_range_type& mutations) {
    if (mutations.empty()) {
        return;
    }
    headers.erase(
        std::remove_if(headers.begin(), headers.end(),
            [&](const auto& header) {
                return std::ranges::any_of(mutations, [&](const auto& mutation) {
                    return flexedge::route_policy::ascii_equal(header.first, mutation.name());
                });
            }),
        headers.end());
    for (const auto& mutation : mutations) {
        headers.emplace_back(mutation.name(), mutation.value());
    }
}

}  // namespace flexedge::node
