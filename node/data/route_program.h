#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "common/route_match.h"
#include "node/runtime/compiled_config.h"

namespace flexedge::node {

namespace detail {

enum class route_action { proxy,
    redirect,
    rewrite };
enum class route_match_kind { prefix,
    exact,
    suffix,
    regex };
enum class route_rewrite_kind { none,
    replace_path,
    strip_prefix,
    replace_prefix };
enum class route_query_kind { preserve,
    drop,
    replace };
enum class route_condition_source { header,
    query };
enum class route_condition_op { equals,
    not_equals,
    exists,
    absent };

struct compiled_route_condition final {
    route_condition_source source_{};
    route_condition_op op_{};
    std::string name_;
    std::string value_;
};

// Literal text and capture references are tokenized once, including escaped dollars.
using route_capture_piece = std::variant<std::string, std::uint8_t>;
using route_capture_template = std::vector<route_capture_piece>;

inline route_capture_template compile_capture_template(std::string_view text, int groups) {
    if (!flexedge::route_policy::capture_template(text, groups)) {
        throw invalid_website("invalid route capture template");
    }
    route_capture_template result;
    std::string literal;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '$' && index + 1 < text.size() && text[index + 1] == '$') {
            literal.push_back('$');
            ++index;
        } else if (text[index] == '$' && index + 3 < text.size() && text[index + 1] == '{' &&
                   text[index + 2] >= '0' && text[index + 2] <= '9' && text[index + 3] == '}') {
            if (!literal.empty()) {
                result.emplace_back(std::move(literal));
                literal.clear();
            }
            result.emplace_back(static_cast<std::uint8_t>(text[index + 2] - '0'));
            index += 3;
        } else {
            literal.push_back(text[index]);
        }
    }
    if (!literal.empty()) {
        result.emplace_back(std::move(literal));
    }
    return result;
}

struct compiled_route_rule final {
    std::string id_;
    route_action action_{};
    route_match_kind match_{};
    route_rewrite_kind rewrite_{};
    route_query_kind query_{};
    std::string path_;
    bool prefix_open_{};
    std::shared_ptr<const RE2> pattern_;
    std::vector<std::string> methods_;
    std::vector<std::string> hostnames_;
    std::vector<compiled_route_condition> conditions_;
    std::string rewrite_path_;
    std::string query_string_;
    std::string redirect_url_;
    route_capture_template rewrite_template_;
    route_capture_template redirect_template_;
    std::uint32_t redirect_status_{};
    std::string origin_group_;
    std::vector<std::string> origin_ids_;
    std::vector<v2::HeaderMutation> request_headers_;
    std::vector<v2::HeaderMutation> response_headers_;
};

struct compiled_website_routes final {
    std::string default_origin_group_;
    std::vector<compiled_route_rule> rules_;
    std::vector<std::size_t> redirects_;
    std::vector<std::size_t> transforms_;
    std::vector<std::size_t> proxies_;
};

inline compiled_website_routes compile_website_routes(const v2::Website& website) {
    compiled_website_routes result;
    result.default_origin_group_ = website.default_origin_group();
    result.rules_.reserve(static_cast<std::size_t>(website.route_rules_size()));
    for (const auto& source : website.route_rules()) {
        if (!source.enabled()) {
            continue;
        }
        compiled_route_rule rule;
        rule.id_ = source.id();
        if (source.action() == "proxy") {
            rule.action_ = route_action::proxy;
        } else if (source.action() == "redirect") {
            rule.action_ = route_action::redirect;
        } else if (source.action() == "rewrite") {
            rule.action_ = route_action::rewrite;
        } else {
            throw invalid_website("invalid route action");
        }
        if (source.match_type() == "prefix") {
            rule.match_ = route_match_kind::prefix;
        } else if (source.match_type() == "exact") {
            rule.match_ = route_match_kind::exact;
        } else if (source.match_type() == "suffix") {
            rule.match_ = route_match_kind::suffix;
        } else if (source.match_type() == "regex") {
            rule.match_ = route_match_kind::regex;
            rule.pattern_ = flexedge::route_policy::compile_pattern(source.path());
            if (!rule.pattern_) {
                throw invalid_website("invalid route regex");
            }
            rule.rewrite_template_ = compile_capture_template(source.rewrite_path(), rule.pattern_->NumberOfCapturingGroups());
            rule.redirect_template_ = compile_capture_template(source.redirect_url(), rule.pattern_->NumberOfCapturingGroups());
        } else {
            throw invalid_website("invalid route match type");
        }
        const auto& mode = source.rewrite_mode();
        if (mode == "none" || (mode.empty() && source.rewrite_path().empty())) {
            rule.rewrite_ = route_rewrite_kind::none;
        } else if (mode == "replace_path" || mode.empty()) {
            rule.rewrite_ = route_rewrite_kind::replace_path;
        } else if (mode == "strip_prefix") {
            rule.rewrite_ = route_rewrite_kind::strip_prefix;
        } else if (mode == "replace_prefix") {
            rule.rewrite_ = route_rewrite_kind::replace_prefix;
        } else {
            throw invalid_website("invalid route rewrite mode");
        }
        if (source.query_mode().empty() || source.query_mode() == "preserve") {
            rule.query_ = route_query_kind::preserve;
        } else if (source.query_mode() == "drop") {
            rule.query_ = route_query_kind::drop;
        } else if (source.query_mode() == "replace") {
            rule.query_ = route_query_kind::replace;
        } else {
            throw invalid_website("invalid route query mode");
        }
        rule.path_ = source.path();
        rule.prefix_open_ = rule.path_ == "/" || rule.path_.ends_with('/');
        rule.methods_.assign(source.methods().begin(), source.methods().end());
        rule.hostnames_.reserve(static_cast<std::size_t>(source.hostnames_size()));
        for (const auto& host : source.hostnames()) {
            rule.hostnames_.push_back(flexedge::route_policy::hostname(host));
        }
        rule.conditions_.reserve(static_cast<std::size_t>(source.conditions_size()));
        for (const auto& condition : source.conditions()) {
            compiled_route_condition compiled;
            if (condition.source() == "header") {
                compiled.source_ = route_condition_source::header;
            } else if (condition.source() == "query") {
                compiled.source_ = route_condition_source::query;
            } else {
                throw invalid_website("invalid route condition source");
            }
            if (condition.op() == "equals") {
                compiled.op_ = route_condition_op::equals;
            } else if (condition.op() == "not_equals") {
                compiled.op_ = route_condition_op::not_equals;
            } else if (condition.op() == "exists") {
                compiled.op_ = route_condition_op::exists;
            } else if (condition.op() == "absent") {
                compiled.op_ = route_condition_op::absent;
            } else {
                throw invalid_website("invalid route condition operator");
            }
            compiled.name_ = condition.name();
            if (compiled.source_ == route_condition_source::header) {
                std::ranges::transform(compiled.name_, compiled.name_.begin(), [](unsigned char ch) {
                    return flexedge::route_policy::lower_ascii(ch);
                });
            }
            compiled.value_ = condition.value();
            rule.conditions_.push_back(std::move(compiled));
        }
        rule.rewrite_path_ = source.rewrite_path();
        rule.query_string_ = source.query_string();
        rule.redirect_url_ = source.redirect_url();
        rule.redirect_status_ = source.redirect_status();
        rule.origin_group_ = source.origin_group();
        rule.origin_ids_.assign(source.origin_ids().begin(), source.origin_ids().end());
        rule.request_headers_.assign(source.request_headers().begin(), source.request_headers().end());
        rule.response_headers_.assign(source.response_headers().begin(), source.response_headers().end());
        const auto index = result.rules_.size();
        if (rule.action_ == route_action::redirect) {
            result.redirects_.push_back(index);
        } else {
            if (rule.rewrite_ != route_rewrite_kind::none || rule.query_ != route_query_kind::preserve) {
                result.transforms_.push_back(index);
            }
            if (rule.action_ == route_action::proxy) {
                result.proxies_.push_back(index);
            }
        }
        result.rules_.push_back(std::move(rule));
    }
    return result;
}

}  // namespace detail

// Construct and retain on one data worker only: RE2 objects never cross worker boundaries.
// Website pointers are identity keys, not an execution-time protobuf dependency. A session
// pins the source configuration and this program together in its worker execution view.
class route_program final {
public:
    explicit route_program(const v2::Website& website) {
        websites_.emplace(&website, detail::compile_website_routes(website));
    }

    explicit route_program(const compiled_config& config) {
        websites_.reserve(config.websites().size());
        for (const auto* website : config.websites()) {
            if (website->enabled()) {
                websites_.emplace(website, detail::compile_website_routes(*website));
            }
        }
    }

    [[nodiscard]] const detail::compiled_website_routes& website(const v2::Website& source) const {
        const auto found = websites_.find(&source);
        if (found == websites_.end()) {
            throw std::logic_error("website does not belong to route program");
        }
        return found->second;
    }

    route_program(const route_program&) = delete;
    route_program& operator=(const route_program&) = delete;
    route_program(route_program&&) = default;
    route_program& operator=(route_program&&) = default;

private:
    std::unordered_map<const v2::Website*, detail::compiled_website_routes> websites_;
};

}  // namespace flexedge::node
