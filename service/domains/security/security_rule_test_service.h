#pragma once
#include "node/data/request_expression.h"
#include "service/domains/security/security_reference.h"
#include "service/domains/security/security_types.h"

namespace service::security {
class security_rule_test_service final {
public:
    static ruvia::task<rule_test_dto> test(ruvia::context& c, const std::string& tenant, const std::string& policy, const std::string& rule_id, const rule_test_input& input) {
        const auto website = service::common::parse_uuid(input.get<"website_id">().view());
        if (!website || input.get<"count">().value_ < 1 || input.get<"count">().value_ > 10000000) {
            service::common::throw_app_error(21003, "测试请求不正确", 422);
        }
        const auto sites = co_await c.db().query("SELECT id FROM sys_website WHERE tenant_id=$1 AND id=$2 AND security_policy_id=$3 AND deleted_at IS NULL", tenant, *website, policy);
        if (sites.empty()) {
            service::common::throw_app_error(21003, "网站未应用此规则集", 422);
        }
        auto tx = co_await c.db().begin_transaction();
        if (!(co_await find_policy(tx, tenant, policy))) {
            service::common::throw_app_error(21002, "规则集不存在", 404);
        }
        const auto source = co_await published_policy(tx, tenant, policy);
        co_await tx.commit();
        const auto& sample = input.get<"request">();
        for (const auto value : {sample.get<"ip">().view(), sample.get<"country">().view(), sample.get<"asn">().view(), sample.get<"method">().view(), sample.get<"host">().view(), sample.get<"path">().view(), sample.get<"user_agent">().view()}) {
            if (!flexedge::route_policy::clean_text(value, 2048)) {
                service::common::throw_app_error(21003, "测试字段不正确", 422);
            }
        }
        flexedge::node::request_expression_program::list_map lists;
        for (const auto& list : source.lists()) {
            std::vector<flexedge::node::request_list_value> values;
            for (const auto& member : list.members()) {
                values.push_back({member.value(), member.expires_unix_ms()});
            }
            lists.emplace(list.name(), std::make_shared<const flexedge::node::request_match_list>(list.type(), values));
        }
        const std::vector<std::pair<std::string, std::string>> headers{{"user-agent", std::string(sample.get<"user_agent">().view())}};
        const flexedge::node::rule_match_request request{sample.get<"path">().view(), sample.get<"method">().view(), sample.get<"host">().view(), false, headers, sample.get<"ip">().view(), sample.get<"country">().view(), sample.get<"asn">().view()};
        bool found{}, matched{}, enabled{};
        for (const auto& rule : source.custom_rules()) {
            if (rule.id() == rule_id) {
                found = true;
                enabled = rule.enabled();
                matched = flexedge::node::request_expression_program(rule.expression(), flexedge::rules::expression_context::security, lists).matches(request);
            }
        }
        for (const auto& rule : source.cc_rules()) {
            if (rule.id() == rule_id) {
                found = true;
                enabled = rule.enabled();
                matched = input.get<"count">().value_ > rule.requests() && flexedge::node::request_expression_program(rule.expression(), flexedge::rules::expression_context::security, lists).matches(request);
            }
        }
        if (!found) {
            service::common::throw_app_error(21002, "规则不存在", 404);
        }
        rule_test_dto result({.resource_ = c.pool()});
        result.set<"matched">(matched);
        result.set<"message">(matched ? (enabled ? "请求符合已保存的规则条件" : "请求符合规则条件；此规则尚未启用") : "请求未达到已保存规则的触发条件");
        co_return result;
    }
};
}  // namespace service::security
