#include "node/proto/security_validation.h"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <charconv>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <asio/ip/address.hpp>

#include "common/request_expression.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

void validate_security(const v2::SecurityPolicy& policy) {
    if (policy.id().empty() || policy.id().size() > 100 || policy.revision() < 1 ||
        policy.name().size() > 256 || policy.custom_rules_size() > 100 || policy.cc_rules_size() > 100) {
        throw std::invalid_argument("invalid security policy");
    }
    std::unordered_set<std::string> ids;
    if (policy.lists_size() > 100 || policy.ByteSizeLong() > 4 * 1024 * 1024) {
        throw std::invalid_argument("security policy exceeds limits");
    }
    std::unordered_map<std::string, std::string> lists;
    std::size_t entries{};
    const auto validate_ip = [](std::string_view item) {
        const auto slash = item.find('/');
        std::error_code error;
        const auto ip = asio::ip::make_address(item.substr(0, slash), error);
        if (error || (ip.is_v6() && ip.to_v6().scope_id())) {
            throw std::invalid_argument("invalid IP or CIDR");
        }
        if (slash != std::string_view::npos) {
            const auto suffix = item.substr(slash + 1);
            unsigned prefix{};
            const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), prefix);
            if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() || prefix > (ip.is_v4() ? 32u : 128u)) {
                throw std::invalid_argument("invalid CIDR prefix");
            }
        }
    };
    for (const auto& list : policy.lists()) {
        if (list.name().empty() || list.name().size() > 100 ||
            list.name().find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
            !lists.emplace(list.name(), list.type()).second ||
            (list.type() != "ip" && list.type() != "asn" && list.type() != "country" && list.type() != "user_agent")) {
            throw std::invalid_argument("invalid security list");
        }
        entries += static_cast<std::size_t>(list.entries_size() + list.members_size());
        if (entries > 100000) {
            throw std::invalid_argument("too many security list entries");
        }
        const auto validate_item = [&](const std::string& item) {
            if (item.empty() || item.size() > 2048 || !flexedge::route_policy::clean_text(item, 2048)) {
                throw std::invalid_argument("invalid security list value");
            }
            if (list.type() == "ip") {
                validate_ip(item);
            }
            if (list.type() == "country" && (item.size() != 2 || item[0] < 'A' || item[0] > 'Z' || item[1] < 'A' || item[1] > 'Z')) {
                throw std::invalid_argument("invalid security country code");
            }
            if (list.type() == "asn") {
                std::uint32_t asn{};
                const auto parsed = std::from_chars(item.data(), item.data() + item.size(), asn);
                if (parsed.ec != std::errc{} || parsed.ptr != item.data() + item.size() || !asn || std::to_string(asn) != item) {
                    throw std::invalid_argument("invalid ASN");
                }
            }
        };
        for (const auto& item : list.entries()) {
            validate_item(item);
        }
        for (const auto& item : list.members()) {
            validate_item(item.value());
        }
    }
    const auto validate_expression = [&](std::string_view text) {
        for (const auto& instruction : flexedge::rules::request_expression_parser(text, flexedge::rules::expression_context::security).parse()) {
            if (instruction.field_ == "ip.src.asnum") {
                throw std::invalid_argument("ASN matching is unavailable: no ASN database is configured");
            }
            if (!instruction.list_.empty()) {
                const auto found = lists.find(instruction.list_);
                const auto type = instruction.field_ == "ip.src" ? "ip" : instruction.field_ == "ip.src.asnum"  ? "asn"
                                                                      : instruction.field_ == "ip.src.country"  ? "country"
                                                                      : instruction.field_ == "http.user_agent" ? "user_agent"
                                                                                                                : "";
                if (found == lists.end() || found->second != type) {
                    throw std::invalid_argument("security list field/type mismatch");
                }
            }
            if (instruction.field_ == "ip.src") {
                if (instruction.lower_ || (instruction.comparison_ != "eq" && instruction.comparison_ != "ne" && instruction.comparison_ != "in")) {
                    throw std::invalid_argument("invalid IP comparison");
                }
                for (const auto& value : instruction.values_) {
                    validate_ip(value);
                }
            }
        }
    };
    for (const auto& rule : policy.custom_rules()) {
        if (!flexedge::route_policy::clean_text(rule.name(), 256)) {
            throw std::invalid_argument("invalid rule name");
        }
        std::unordered_set<std::string> skips;
        for (const auto& stage : rule.skip_stages()) {
            if ((stage != "custom" && stage != "rate" && stage != "managed") || !skips.insert(stage).second) {
                throw std::invalid_argument("invalid skip stage");
            }
        }
        validate_expression(rule.expression());
        if (rule.response_content_type() == "application/json" && !rule.response_body().empty()) {
            google::protobuf::Value json;
            if (!google::protobuf::util::JsonStringToMessage(rule.response_body(), &json).ok()) {
                throw std::invalid_argument("invalid custom JSON response");
            }
        }
        if (rule.id().empty() || rule.id().size() > 100 || !ids.insert(rule.id()).second ||
            (rule.action() != "block" && rule.action() != "log" && rule.action() != "skip") ||
            rule.response_body().size() > 2048 ||
            !flexedge::route_policy::valid_utf8(rule.response_body()) ||
            (rule.response_status() && (rule.response_status() < 400 || rule.response_status() > 499)) ||
            (!rule.response_content_type().empty() && rule.response_content_type() != "text/plain" &&
                rule.response_content_type() != "text/html" && rule.response_content_type() != "application/json" &&
                rule.response_content_type() != "application/xml")) {
            throw std::invalid_argument("invalid custom security rule");
        }
    }
    for (const auto& rule : policy.cc_rules()) {
        if (!flexedge::route_policy::clean_text(rule.name(), 256)) {
            throw std::invalid_argument("invalid CC rule name");
        }
        validate_expression(rule.expression());
        if (rule.response_body().size() > 2048 || !flexedge::route_policy::valid_utf8(rule.response_body()) ||
            (rule.response_status() && (rule.response_status() < 400 || rule.response_status() > 499)) ||
            (!rule.response_content_type().empty() && rule.response_content_type() != "text/plain" && rule.response_content_type() != "text/html" && rule.response_content_type() != "application/json" && rule.response_content_type() != "application/xml")) {
            throw std::invalid_argument("invalid CC response");
        }
        if (rule.response_content_type() == "application/json" && !rule.response_body().empty()) {
            google::protobuf::Value json;
            if (!google::protobuf::util::JsonStringToMessage(rule.response_body(), &json).ok()) {
                throw std::invalid_argument("invalid CC JSON response");
            }
        }
        if (rule.id().empty() || rule.id().size() > 100 || !ids.insert(rule.id()).second ||
            (rule.action() != "block" && rule.action() != "log") ||
            !rule.requests() || rule.requests() > 1000000000 || !rule.window_seconds() ||
            rule.window_seconds() > 3600 || !rule.mitigation_seconds() || rule.mitigation_seconds() > 86400) {
            throw std::invalid_argument("invalid CC security rule");
        }
    }
    if (policy.has_managed()) {
        const auto& managed = policy.managed();
        if (managed.paranoia_level() < 1 || managed.paranoia_level() > 4 ||
            managed.inbound_threshold() < 1 || managed.inbound_threshold() > 1000 ||
            managed.body_limit_bytes() < 1024 || managed.body_limit_bytes() > 8 * 1024 * 1024 ||
            managed.excluded_rules_size() > 512) {
            throw std::invalid_argument("invalid managed security settings");
        }
        std::unordered_set<std::uint32_t> excluded;
        for (const auto id : managed.excluded_rules()) {
            if (id < 910000 || id > 949999 || !excluded.insert(id).second) {
                throw std::invalid_argument("invalid managed security rule exclusion");
            }
        }
    }
}

}  // namespace flexedge::node
