#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/request_expression.h"
#include "node/data/request_match_list.h"

namespace flexedge::node {

struct rule_match_request final {
    std::string_view target_{};
    std::string_view method_{};
    std::string_view host_{};
    bool secure_{false};
    std::span<const std::pair<std::string, std::string>> headers_{};
    std::string_view client_ip_{}, country_{}, asn_{};
};

// This program is compiled separately by each data Worker. The source config
// owns no regex, cache, callback or other execution object.
class request_expression_program final {
public:
    using list_map = std::unordered_map<std::string, std::shared_ptr<const request_match_list>>;
    explicit request_expression_program(std::string_view expression,
        flexedge::rules::expression_context context = flexedge::rules::expression_context::cache,
        const list_map& lists = {}) {
        auto source = flexedge::rules::request_expression_parser(expression, context).parse();
        for (auto& instruction : source) {
            compiled_instruction compiled{.source_ = std::move(instruction)};
            const auto& condition = compiled.source_;
            if (!condition.list_.empty()) {
                const auto found = lists.find(condition.list_);
                if (found == lists.end()) {
                    throw std::invalid_argument("security list does not exist");
                }
                compiled.list_ = found->second;
                const auto& type = compiled.list_->type();
                if ((condition.field_ == "ip.src" && type != "ip") ||
                    (condition.field_ == "ip.src.asnum" && type != "asn") ||
                    (condition.field_ == "ip.src.country" && type != "country") ||
                    (condition.field_ == "http.user_agent" && type != "user_agent") ||
                    (condition.field_ != "ip.src" && condition.field_ != "ip.src.asnum" &&
                        condition.field_ != "ip.src.country" && condition.field_ != "http.user_agent")) {
                    throw std::invalid_argument("security list type does not match field");
                }
            } else if (condition.field_ == "ip.src") {
                if (condition.comparison_ != "eq" && condition.comparison_ != "ne" && condition.comparison_ != "in") {
                    throw std::invalid_argument("unsupported IP comparison");
                }
                compiled.list_ = std::make_shared<const request_match_list>("ip", condition.values_);
            }
            if (condition.comparison_ == "matches" || condition.comparison_.ends_with("wildcard")) {
                RE2::Options options;
                options.set_log_errors(false);
                options.set_max_mem(1 << 20);
                options.set_dot_nl(true);
                options.set_case_sensitive(condition.comparison_ != "wildcard");
                std::string pattern;
                if (condition.comparison_ == "matches") {
                    pattern = condition.values_.front();
                } else {
                    bool escaped = false;
                    for (const auto byte : condition.values_.front()) {
                        if (!escaped && byte == '\\') {
                            escaped = true;
                            continue;
                        }
                        if (!escaped && byte == '*') {
                            pattern += ".*";
                        } else {
                            if (std::string_view("\\.^$|()[]{}*+?").find(byte) != std::string_view::npos) {
                                pattern += '\\';
                            }
                            pattern += byte;
                        }
                        escaped = false;
                    }
                }
                compiled.pattern_ = std::make_unique<const RE2>(pattern, options);
                if (!compiled.pattern_->ok()) {
                    throw std::invalid_argument("匹配表达式正则编译失败");
                }
            }
            program_.push_back(std::move(compiled));
        }
    }

    [[nodiscard]] bool matches(const rule_match_request& request) const {
        std::array<bool, 256> stack{};
        std::size_t size{};
        for (const auto& instruction : program_) {
            const auto& source = instruction.source_;
            switch (source.op_) {
                case flexedge::rules::expression_op::constant:
                    stack[size++] = !source.values_.empty();
                    break;
                case flexedge::rules::expression_op::compare:
                    stack[size++] = compare(instruction, request);
                    break;
                case flexedge::rules::expression_op::logical_not:
                    stack[size - 1] = !stack[size - 1];
                    break;
                case flexedge::rules::expression_op::logical_and:
                    --size;
                    stack[size - 1] = stack[size - 1] && stack[size];
                    break;
                case flexedge::rules::expression_op::logical_xor:
                    --size;
                    stack[size - 1] = stack[size - 1] != stack[size];
                    break;
                case flexedge::rules::expression_op::logical_or:
                    --size;
                    stack[size - 1] = stack[size - 1] || stack[size];
                    break;
            }
        }
        return size == 1 && stack[0];
    }

private:
    struct compiled_instruction final {
        flexedge::rules::expression_instruction source_{};
        std::unique_ptr<const RE2> pattern_{};
        std::shared_ptr<const request_match_list> list_{};
    };
    static std::optional<std::string_view> first_header(const rule_match_request& request, std::string_view name) {
        for (const auto& header : request.headers_) {
            if (flexedge::route_policy::ascii_equal(header.first, name)) {
                return header.second;
            }
        }
        return std::nullopt;
    }
    static bool compare(const compiled_instruction& instruction, const rule_match_request& request) {
        const auto& condition = instruction.source_;
        const auto path = request.target_.substr(0, request.target_.find('?'));
        std::string owned;
        std::optional<std::string_view> value;
        if (condition.field_ == "ip.src") {
            value = request.client_ip_;
        } else if (condition.field_ == "ip.src.country") {
            value = request.country_;
        } else if (condition.field_ == "ip.src.asnum") {
            value = request.asn_;
        } else if (condition.field_ == "http.host") {
            value = request.host_;
        } else if (condition.field_ == "http.request.method") {
            value = request.method_;
        } else if (condition.field_ == "http.request.uri.path" || condition.field_ == "raw.http.request.uri.path") {
            value = path;
        } else if (condition.field_ == "http.request.uri.query" || condition.field_ == "raw.http.request.uri.query") {
            const auto query = request.target_.find('?');
            value = query == std::string_view::npos ? std::string_view{} : request.target_.substr(query + 1);
        } else if (condition.field_ == "http.request.uri.path.extension") {
            const auto dot = path.rfind('.');
            const auto slash = path.rfind('/');
            value = dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) ? std::string_view{} : path.substr(dot + 1);
        } else if (condition.field_ == "http.cookie") {
            value = first_header(request, "Cookie").value_or(std::string_view{});
        } else if (condition.field_ == "http.user_agent") {
            value = first_header(request, "User-Agent").value_or(std::string_view{});
        } else if (condition.field_ == "http.referer") {
            value = first_header(request, "Referer").value_or(std::string_view{});
        } else if (condition.field_ == "http.request.headers") {
            value = first_header(request, condition.header_);
        } else if (condition.field_ == "http.request.full_uri") {
            owned = request.secure_ ? "https://" : "http://";
            owned += first_header(request, "Host").value_or(request.host_);
            owned += request.target_;
            value = owned;
        }
        if (!value) {
            return false;
        }
        if (condition.lower_) {
            owned = *value;
            std::ranges::transform(owned, owned.begin(), [](unsigned char c) { return flexedge::route_policy::lower_ascii(c); });
            value = owned;
        }
        const auto& op = condition.comparison_;
        if (instruction.list_) {
            const auto matched = instruction.list_->contains(*value);
            return op == "ne" ? !matched : matched;
        }
        if (op == "in") {
            return std::ranges::find(condition.values_, *value) != condition.values_.end();
        }
        const auto& expected = condition.values_.front();
        if (op == "eq") {
            return *value == expected;
        }
        if (op == "ne") {
            return *value != expected;
        }
        if (op == "contains") {
            return value->find(expected) != std::string_view::npos;
        }
        if (op == "starts_with") {
            return value->starts_with(expected);
        }
        if (op == "ends_with") {
            return value->ends_with(expected);
        }
        return op == "matches" ? RE2::PartialMatch(*value, *instruction.pattern_) : RE2::FullMatch(*value, *instruction.pattern_);
    }

    std::vector<compiled_instruction> program_{};
};

}  // namespace flexedge::node
