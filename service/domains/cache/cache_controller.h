#pragma once
#include <ruvia/web/controller.h>
#include <ruvia/web/request_validation.h>

#include "service/common/http.h"
#include "service/domains/cache/cache_operation_service.h"
#include "service/domains/cache/cache_policy_service.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"
namespace service::cache {
class cache_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/cache", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(cache_controller)
    RUVIA_GET("/policies", list);
    RUVIA_GET("/websites/options", websites);
    RUVIA_GET("/policies/options", options);
    RUVIA_GET("/policies/:id", detail);
    RUVIA_POST("/policies", create, ruvia::json_body<policy_input>);
    RUVIA_PUT("/policies/:id", update, ruvia::json_body<policy_input>);
    RUVIA_DELETE("/policies/:id", remove);
    RUVIA_GET("/operations", operations);
    RUVIA_GET_SSE("/operations/stream", operations_stream);
    RUVIA_POST("/operations", submit, ruvia::json_body<operation_input>);
    RUVIA_POST("/operations/:id/retry", retry);
    RUVIA_ROUTES_END
private:
    static std::string tenant(ruvia::context& c) {
        return service::middleware::current_tenant_id(c);
    }
    static std::string operation(ruvia::context& c) {
        return service::common::require_enum_query(c, "operation", {"prewarm", "refresh", "clean"}, "缓存操作不正确").value_or("prewarm");
    }
    ruvia::task<ruvia::http_response> websites(ruvia::context& c) {
        const auto id = service::common::optional_uuid_query(c, "policy_id").value_or("");
        auto data = co_await operations_.websites(c, tenant(c), id);
        co_return c.json(service::common::ok<website_options_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> list(ruvia::context& c) {
        const auto [page, size, skip] = service::common::require_pagination(c);
        auto data = co_await policies_.list(c, tenant(c), page, size, skip,
            service::common::require_keyword(c.req().query("keyword")).value_or(""),
            service::common::require_enum_query(c, "status", {"enabled", "disabled"}, "缓存状态不正确").value_or(""));
        co_return c.json(service::common::ok<policy_page_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> options(ruvia::context& c) {
        auto page = co_await policies_.list(c, tenant(c), 1, 1000, 0, "", "");
        policy_options_response result({.resource_ = c.pool()});
        result.set<"code">(0);
        result.set<"message">("ok");
        result.set<"data">(std::move(page.get<"list">()));
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
        co_return c.json(service::common::operation(c, "缓存策略已删除"));
    }
    ruvia::task<ruvia::http_response> submit(ruvia::context& c) {
        auto data = co_await operations_.create(c, tenant(c), c.req().validated<operation_input>());
        co_return c.json(service::common::ok<operation_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> operations(ruvia::context& c) {
        auto data = co_await operations_.list(c, tenant(c), operation(c));
        co_return c.json(service::common::ok<operations_response>(c, std::move(data)));
    }
    ruvia::task<void> operations_stream(ruvia::context& c) {
        const auto owner = tenant(c);
        const auto kind = operation(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), owner, service::live_resource::resource::tasks, {}, service::live_resource::query_key("cache-operations", kind));
        co_await service::live_resource::stream_snapshot(c, std::move(sub), [this, owner, kind](auto& read) -> ruvia::task<operations_response> {
            auto data = co_await operations_.list(read, owner, kind);
            co_return service::common::ok<operations_response>(read, std::move(data));
        });
    }
    ruvia::task<ruvia::http_response> retry(ruvia::context& c) {
        co_await operations_.retry(c, tenant(c), service::common::require_uuid_param(c, "id"));
        co_return c.json(service::common::operation(c, "任务已重新排队"));
    }
    cache_policy_service policies_{};
    cache_operation_service operations_{};
};
}  // namespace service::cache
