#include <ruvia/web/controller.h>
#include <ruvia/web/request_validation.h>

#include "security_catalog.h"
#include "service/domains/security/security_list_service.h"
#include "service/domains/security/security_policy_service.h"
#include "service/domains/security/security_read_service.h"
#include "service/domains/security/security_rule_test_service.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"

namespace service::security {
class security_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/security", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(security_controller)
    RUVIA_GET("/policies", list);
    RUVIA_GET("/managed-rules", managed_rules);
    RUVIA_GET("/websites", websites);
    RUVIA_GET_SSE("/stream", stream);
    RUVIA_POST("/policies/:id/rules/:rule_id/test", test_rule, ruvia::json_body<rule_test_input>);
    RUVIA_GET("/lists", lists);
    RUVIA_GET("/lists/:id", list_detail);
    RUVIA_POST("/lists", create_list, ruvia::json_body<list_input>);
    RUVIA_PUT("/lists/:id", update_list, ruvia::json_body<list_input>);
    RUVIA_GET("/policies/options", options);
    RUVIA_GET("/policies/:id", detail);
    RUVIA_POST("/policies", create, ruvia::json_body<policy_input>);
    RUVIA_PUT("/policies/:id", update, ruvia::json_body<policy_input>);
    RUVIA_DELETE("/policies/:id", remove);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> websites(ruvia::context& c) {
        auto data = co_await security_read_service::websites(c, tenant(c));
        co_return c.json(service::common::ok<websites_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> test_rule(ruvia::context& c) {
        const auto rule_id = c.req().param("rule_id").value_or("");
        if (rule_id.empty() || !flexedge::route_policy::clean_text(rule_id, 100)) {
            service::common::throw_app_error(21003, "规则标识不正确", 422);
        }
        auto data = co_await security_rule_test_service::test(c, tenant(c), service::common::require_uuid_param(c, "id"), std::string(rule_id), c.req().validated<rule_test_input>());
        co_return c.json(service::common::ok<rule_test_response>(c, std::move(data)));
    }
    ruvia::task<void> stream(ruvia::context& c) {
        namespace live = service::live_resource;
        const auto scope = tenant(c);
        std::vector<live::subscription_part> parts;
        parts.push_back({live::resource::security_history, {}, live::query_key("security.telemetry")});
        auto sub = live::get_hub(c).subscribe(c.worker(), scope, std::move(parts));
        co_await live::stream_snapshot(c, std::move(sub), [scope](auto& read) -> ruvia::task<telemetry_response> {
            auto data = co_await security_read_service::telemetry(read, scope);
            co_return service::common::ok<telemetry_response>(read, std::move(data));
        });
    }
    ruvia::task<ruvia::http_response> lists(ruvia::context& c) {
        auto data = co_await lists_.list(c, tenant(c));
        co_return c.json(service::common::ok<lists_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> list_detail(ruvia::context& c) {
        auto data = co_await lists_.detail(c, tenant(c), service::common::require_uuid_param(c, "id"));
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<list_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> create_list(ruvia::context& c) {
        auto data = co_await lists_.save(c, tenant(c), {}, 0, c.req().validated<list_input>());
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<list_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> update_list(ruvia::context& c) {
        auto data = co_await lists_.save(c, tenant(c), service::common::require_uuid_param(c, "id"), service::common::require_expected_revision(c), c.req().validated<list_input>());
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<list_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> managed_rules(ruvia::context& c) {
        ruvia::array<managed_rule_dto> entries({.resource_ = c.pool()});
        for (const auto& source : flexedge::security::crs_catalog) {
            auto& entry = entries.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            entry.set<"id">(source.id_);
            entry.set<"name">(source.name_);
            entry.set<"category">(source.category_);
            entry.set<"paranoia_level">(source.paranoia_level_);
        }
        co_return c.json(service::common::ok<managed_rules_response>(c, std::move(entries)));
    }
    static std::string tenant(ruvia::context& c) {
        return service::middleware::current_tenant_id(c);
    }
    ruvia::task<ruvia::http_response> list(ruvia::context& c) {
        const auto [page, size, skip] = service::common::require_pagination(c);
        auto data = co_await policies_.list(c, tenant(c), page, size, skip, service::common::require_keyword(c.req().query("keyword")).value_or(""));
        co_return c.json(service::common::ok<policy_page_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> options(ruvia::context& c) {
        auto data = co_await policies_.options(c, tenant(c));
        policy_options_response result({.resource_ = c.pool()});
        result.set<"code">(0);
        result.set<"message">("ok");
        result.set<"data">(std::move(data));
        co_return c.json(result);
    }
    ruvia::task<ruvia::http_response> detail(ruvia::context& c) {
        auto data = co_await policies_.detail(c, tenant(c), service::common::require_uuid_param(c, "id"));
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<policy_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        auto data = co_await policies_.save(c, tenant(c), {}, 0, c.req().validated<policy_input>());
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<policy_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        auto data = co_await policies_.save(c, tenant(c), service::common::require_uuid_param(c, "id"), service::common::require_expected_revision(c), c.req().validated<policy_input>());
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<policy_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        co_await policies_.remove(c, tenant(c), service::common::require_uuid_param(c, "id"), service::common::require_expected_revision(c));
        co_return c.json(service::common::operation(c, "规则集已删除"));
    }
    security_policy_service policies_;
    security_list_service lists_;
};
}  // namespace service::security
