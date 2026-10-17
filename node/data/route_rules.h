#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/route_match.h"
#include "common/route_policy.h"
#include "node/data/route_program.h"

namespace flexedge::node {

namespace detail {

struct route_match_context final {
    route_match_context(std::string_view method, std::string_view target,
        std::string_view host = {},
        std::span<const std::pair<std::string_view, std::string_view>> headers = {})
        : method_(method),
          target_(target),
          path_(target.substr(0, target.find('?'))),
          hostname_(host),
          headers_(headers) {}

    [[nodiscard]] route_match_context with_target(std::string_view target) const {
        return route_match_context(method_, target, hostname_, headers_);
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
    std::string_view hostname_{};
    std::span<const std::pair<std::string_view, std::string_view>> headers_{};

private:
    mutable std::optional<flexedge::route_policy::query_values> query_{};
    mutable bool query_parsed_{false};
};

[[nodiscard]] inline bool route_condition_matches(const compiled_route_condition& condition,
    const route_match_context& context) {
    bool present = false;
    bool equal = false;
    if (condition.source_ == route_condition_source::header) {
        for (const auto& [name, value] : context.headers_) {
            if (flexedge::route_policy::ascii_equal(name, condition.name_)) {
                present = true;
                equal = equal || flexedge::route_policy::trim_ows(value) == condition.value_;
            }
        }
    } else {
        const auto& query = context.query();
        if (!query) {
            return false;
        }
        for (const auto& [name, value] : *query) {
            if (name == condition.name_) {
                present = true;
                equal = equal || value == condition.value_;
            }
        }
    }
    switch (condition.op_) {
        case route_condition_op::equals:
            return equal;
        case route_condition_op::not_equals:
            return present && !equal;
        case route_condition_op::exists:
            return present;
        case route_condition_op::absent:
            return !present;
    }
    throw std::logic_error("invalid compiled route condition operator");
}

[[nodiscard]] inline bool route_matches(const compiled_route_rule& rule,
    const route_match_context& context) {
    if (!rule.methods_.empty() &&
        std::ranges::find(rule.methods_, context.method_) == rule.methods_.end()) {
        return false;
    }
    if (!rule.hostnames_.empty() &&
        std::ranges::find(rule.hostnames_, context.hostname_) == rule.hostnames_.end()) {
        return false;
    }
    if (std::ranges::any_of(rule.conditions_, [&](const auto& condition) {
            return !route_condition_matches(condition, context);
        })) {
        return false;
    }
    switch (rule.match_) {
        case route_match_kind::regex:
            return RE2::PartialMatch(
                absl::string_view(context.path_.data(), context.path_.size()), *rule.pattern_);
        case route_match_kind::suffix:
            return context.path_.ends_with(rule.path_);
        case route_match_kind::exact:
            return context.path_ == rule.path_;
        case route_match_kind::prefix:
            return context.path_.starts_with(rule.path_) &&
                   (rule.prefix_open_ || context.path_.size() == rule.path_.size() ||
                       context.path_[rule.path_.size()] == '/');
    }
    throw std::logic_error("invalid compiled route match kind");
}

}  // namespace detail

// Header names and values borrow the immutable route program, never a temporary
// protobuf or an earlier evaluation result.
class route_header_view final {
public:
    explicit route_header_view(const v2::HeaderMutation& mutation) noexcept
        : name_(mutation.name()),
          value_(mutation.value()) {}

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }
    [[nodiscard]] std::string_view value() const noexcept {
        return value_;
    }

private:
    std::string_view name_;
    std::string_view value_;
};

// Metadata/header views borrow `route_program`; an unchanged target borrows the
// request buffer. Both must outlive this result, including retries and serialization.
// Owned targets live in the variant itself, so moving a result cannot leave a view
// pointing into the moved-from result's small-string storage.
class evaluated_route_policy final {
public:
    explicit evaluated_route_policy(const detail::compiled_website_routes& plan) noexcept
        : origin_group_(plan.default_origin_group_),
          origin_plan_(&plan.origin_plans_.front()) {}

    [[nodiscard]] std::string_view id() const noexcept {
        return id_;
    }
    [[nodiscard]] std::string_view action() const noexcept {
        return action_;
    }
    [[nodiscard]] std::string_view origin_group() const noexcept {
        return origin_group_;
    }
    [[nodiscard]] std::span<const std::string> origin_ids() const noexcept {
        return origin_ids_;
    }
    [[nodiscard]] const origin_plan& origins() const noexcept {
        return *origin_plan_;
    }
    [[nodiscard]] std::string_view redirect_url() const noexcept {
        return redirect_url_;
    }
    [[nodiscard]] std::uint32_t redirect_status() const noexcept {
        return redirect_status_;
    }
    [[nodiscard]] std::string_view rewritten_target() const noexcept {
        return std::visit([](const auto& target) -> std::string_view { return target; },
            rewritten_target_);
    }
    [[nodiscard]] std::span<const route_header_view> request_headers() const noexcept {
        return request_headers_;
    }
    [[nodiscard]] std::span<const route_header_view> response_headers() const noexcept {
        return response_headers_;
    }

    std::string_view id_;
    std::string_view action_{"proxy"};
    std::string_view origin_group_;
    std::span<const std::string> origin_ids_;
    const origin_plan* origin_plan_;
    std::string redirect_url_;
    std::uint32_t redirect_status_{};
    std::variant<std::string_view, std::string> rewritten_target_;
    std::vector<route_header_view> request_headers_;
    std::vector<route_header_view> response_headers_;
};

namespace detail {

[[nodiscard]] inline std::optional<std::string> expand_route_captures(
    const route_capture_template& value, std::string_view path, const RE2& pattern) {
    std::array<absl::string_view, 10> captures{};
    if (!pattern.Match(absl::string_view(path.data(), path.size()), 0, path.size(),
            RE2::UNANCHORED, captures.data(), pattern.NumberOfCapturingGroups() + 1)) {
        return {};
    }
    std::string result;
    for (const auto& piece : value) {
        if (const auto* literal = std::get_if<std::string>(&piece)) {
            result.append(*literal);
        } else {
            const auto& capture = captures[std::get<std::uint8_t>(piece)];
            if (!capture.empty()) {
                result.append(capture.data(), capture.size());
            }
        }
        if (result.size() > 16384) {
            return {};
        }
    }
    if (!flexedge::route_policy::clean_text(result, 16384) || result.find(' ') != std::string::npos) {
        return {};
    }
    return result;
}

[[nodiscard]] inline std::string apply_route_query(std::string_view destination,
    std::string_view incoming, route_query_kind mode, std::string_view replacement) {
    const auto fragment_at = destination.find('#');
    const auto fragment = fragment_at == std::string_view::npos ? std::string_view{} : destination.substr(fragment_at);
    const auto base = destination.substr(0, fragment_at);
    std::string result;
    if (mode == route_query_kind::drop || mode == route_query_kind::replace) {
        result = base.substr(0, base.find('?'));
        if (mode == route_query_kind::replace && !replacement.empty()) {
            result.push_back('?');
            result.append(replacement);
        }
    } else {
        result = base;
        const auto question = incoming.find('?');
        if (base.find('?') == std::string_view::npos && question != std::string_view::npos) {
            result.append(incoming.substr(question));
        }
    }
    result.append(fragment);
    return result;
}

[[nodiscard]] inline std::optional<std::string> rewrite_route_path(std::string_view path,
    const compiled_route_rule& rule) {
    switch (rule.rewrite_) {
        case route_rewrite_kind::none:
            return std::string(path);
        case route_rewrite_kind::replace_path:
            if (rule.match_ == route_match_kind::regex) {
                return expand_route_captures(rule.rewrite_template_, path, *rule.pattern_);
            }
            return rule.rewrite_path_;
        case route_rewrite_kind::strip_prefix:
        case route_rewrite_kind::replace_prefix: {
            if (!path.starts_with(rule.path_)) {
                return std::string(path);
            }
            auto suffix = path.substr(rule.path_.size());
            std::string result = rule.rewrite_ == route_rewrite_kind::strip_prefix ? "/" : rule.rewrite_path_;
            if (suffix.empty()) {
                return result.empty() ? "/" : result;
            }
            if (result.ends_with('/') && suffix.starts_with('/')) {
                suffix.remove_prefix(1);
            } else if (!result.ends_with('/') && !suffix.starts_with('/')) {
                result.push_back('/');
            }
            result.append(suffix);
            return result;
        }
    }
    throw std::logic_error("invalid compiled route rewrite kind");
}

[[nodiscard]] inline std::string route_redirect_location(std::string_view target,
    const compiled_route_rule& rule) {
    std::string_view destination = rule.redirect_url_;
    std::optional<std::string> expanded;
    if (rule.match_ == route_match_kind::regex) {
        expanded = expand_route_captures(rule.redirect_template_, target.substr(0, target.find('?')), *rule.pattern_);
        if (!expanded) {
            return {};
        }
        destination = *expanded;
    }
    return apply_route_query(destination, target, rule.query_, rule.query_string_);
}

}  // namespace detail

// Each phase matches immutable input; retain the program and request through retries.
[[nodiscard]] inline std::optional<evaluated_route_policy> evaluate_route_rules(
    const route_program& program, const v2::Website& website,
    std::string_view method, std::string_view target, std::string_view host = {},
    std::span<const std::pair<std::string_view, std::string_view>> headers = {}) {
    const auto& plan = program.website(website);
    if (plan.rules_.empty()) {
        evaluated_route_policy result(plan);
        result.rewritten_target_ = target;
        return result;
    }
    const auto normalized_host = flexedge::route_policy::hostname(host);
    const detail::route_match_context incoming(method, target, normalized_host, headers);
    for (const auto index : plan.redirects_) {
        const auto& rule = plan.rules_[index];
        if (!detail::route_matches(rule, incoming)) {
            continue;
        }
        auto location = detail::route_redirect_location(target, rule);
        if (location.empty()) {
            return std::nullopt;
        }
        evaluated_route_policy result(plan);
        result.id_ = rule.id_;
        result.action_ = "redirect";
        result.redirect_url_ = std::move(location);
        result.redirect_status_ = rule.redirect_status_;
        result.response_headers_.reserve(rule.response_headers_.size());
        for (const auto& mutation : rule.response_headers_) {
            result.response_headers_.emplace_back(mutation);
        }
        return result;
    }
    evaluated_route_policy result(plan);
    result.rewritten_target_ = target;
    for (const auto index : plan.transforms_) {
        const auto& rule = plan.rules_[index];
        if (!detail::route_matches(rule, incoming)) {
            continue;
        }
        if (rule.rewrite_ != detail::route_rewrite_kind::none) {
            const auto path = detail::rewrite_route_path(incoming.path_, rule);
            if (!path) {
                return std::nullopt;
            }
            auto candidate = detail::apply_route_query(*path, target, rule.query_, rule.query_string_);
            if (candidate.empty()) {
                return std::nullopt;
            }
            // Every transform matches the original input; path changes retain the query
            // selected by an earlier rule instead of chaining transformations.
            const auto rewritten = result.rewritten_target();
            const auto query_at = rewritten.find('?');
            const auto candidate_query = candidate.find('?');
            if (candidate_query != std::string::npos) {
                candidate.erase(candidate_query);
            }
            if (query_at != std::string_view::npos) {
                candidate.append(rewritten.substr(query_at));
            }
            result.rewritten_target_ = std::move(candidate);
        }
        if (rule.query_ != detail::route_query_kind::preserve) {
            result.rewritten_target_ = detail::apply_route_query(
                result.rewritten_target(), target, rule.query_, rule.query_string_);
        }
    }
    std::optional<detail::route_match_context> rewritten_context;
    if (result.rewritten_target() != target) {
        rewritten_context.emplace(incoming.with_target(result.rewritten_target()));
    }
    const auto& proxy_context = rewritten_context ? *rewritten_context : incoming;
    auto merge_headers = [](std::vector<route_header_view>& destination, const auto& changes) {
        for (const auto& change : changes) {
            const auto found = std::ranges::find_if(destination, [&](const auto& existing) {
                return flexedge::route_policy::ascii_equal(existing.name(), change.name());
            });
            if (found != destination.end()) {
                *found = route_header_view(change);
            } else {
                destination.emplace_back(change);
            }
        }
    };
    for (const auto index : plan.proxies_) {
        const auto& rule = plan.rules_[index];
        if (!detail::route_matches(rule, proxy_context)) {
            continue;
        }
        result.id_ = rule.id_;
        result.origin_group_ = rule.origin_group_;
        result.origin_ids_ = rule.origin_ids_;
        result.origin_plan_ = &plan.origin_plans_[rule.origin_plan_index_];
        merge_headers(result.request_headers_, rule.request_headers_);
        merge_headers(result.response_headers_, rule.response_headers_);
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
