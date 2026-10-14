#pragma once

#include <ruvia/web/controller.h>

#include <initializer_list>
#include <string>
#include <string_view>

#include "service/common/http.h"
#include "service/domains/task/task_service.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"

namespace service::task {
class task_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/tasks", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(task_controller)
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/:id/stream", detail);
    RUVIA_GET_SSE("/:id/history/stream", history);
    RUVIA_GET_SSE("/:id/attempts/:attempt_id/trace/stream", attempt_trace);
    RUVIA_ROUTES_END
private:
    static std::string filter(ruvia::context& c, std::string_view name,
        std::initializer_list<std::string_view> allowed) {
        const auto value = c.req().query(name).value_or("");
        if (value.empty()) {
            return {};
        }
        for (const auto option : allowed) {
            if (value == option) {
                return std::string(value);
            }
        }
        service::common::throw_app_error(service::common::validation_error_code, "任务筛选条件不正确",
            400);
    }
    ruvia::task<void> list(ruvia::context& c) {
        const auto [page, size, skip] = service::common::require_pagination(c);
        const auto type =
            filter(c, "type", {"provider", "dns_zone", "certificate", "website", "node"});
        const auto status = filter(
            c, "status",
            {"queued", "running", "retrying", "completed", "recovered", "failed", "superseded"});
        const auto days = filter(c, "days", {"0", "1", "7"});
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        const auto tenant = service::middleware::current_tenant_id(c);
        const auto days_value = days.empty() ? 0LL : std::stoll(days);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::tasks, {},
            service::live_resource::query_key("list", page, size, type, status,
                keyword.value_or(""), days_value));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, page, size, skip, type, status, keyword = keyword.value_or(""), days = days_value](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_task_service().list(read, tenant, page, size, skip, type, status, keyword, days);
                co_return std::string(ruvia::to_json(service::common::ok<task_page_response>(read, std::move(data)), {.resource_ = read.pool()}));
            });
    }
    ruvia::task<void> history(ruvia::context& c) {
        const auto id = service::common::require_uuid_param(c, "id");
        const auto version = service::common::parse_int64(c.req().query("version").value_or(""));
        if (!version || *version < 1) {
            service::common::throw_app_error(service::common::validation_error_code, "任务版本不正确",
                400);
        }
        // Optional: node publish tasks share the release id, the node selects one target.
        std::string resource_id;
        if (const auto value = c.req().query("resource_id").value_or(""); !value.empty()) {
            const auto parsed = service::common::parse_uuid(value);
            if (!parsed) {
                service::common::throw_app_error(service::common::validation_error_code, "任务标识不正确",
                    400);
            }
            resource_id = std::string(*parsed);
        }
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::tasks, id,
            service::live_resource::query_key("history", version.value(), resource_id));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, id, resource_id, version = *version](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_task_service().history(read, tenant, id, resource_id, version);
                co_return std::string(ruvia::to_json(service::common::ok<task_history_response>(read, std::move(data)), {.resource_ = read.pool()}));
            });
    }
    // One-shot snapshot of one attempt's trace; traces never change once
    // stored, so the client closes the stream after the first frame.
    ruvia::task<void> attempt_trace(ruvia::context& c) {
        const auto id = service::common::require_uuid_param(c, "id");
        const auto attempt_id = service::common::parse_int64(c.req().param("attempt_id"));
        if (!attempt_id || *attempt_id < 1) {
            service::common::throw_app_error(service::common::validation_error_code, "执行记录标识不正确",
                400);
        }
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::tasks, id,
            service::live_resource::query_key("trace", *attempt_id));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, id, attempt_id = *attempt_id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_task_service().attempt_trace(read, tenant, id, attempt_id);
                co_return std::string(ruvia::to_json(service::common::ok<task_attempt_trace_response>(read, std::move(data)), {.resource_ = read.pool()}));
            });
    }
    ruvia::task<void> detail(ruvia::context& c) {
        const auto id = service::common::require_uuid_param(c, "id");
        const auto resource_id =
            service::common::parse_uuid(c.req().query("resource_id").value_or(""));
        const auto version = service::common::parse_int64(c.req().query("version").value_or(""));
        if (!resource_id || !version || *version < 1) {
            service::common::throw_app_error(service::common::validation_error_code, "任务标识不正确",
                400);
        }
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::tasks, id,
            service::live_resource::query_key("detail", *resource_id, *version));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, id, resource_id = *resource_id, version = *version](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_task_service().detail(read, tenant, id, resource_id, version);
                co_return std::string(ruvia::to_json(service::common::ok<task_response>(read, std::move(data)), {.resource_ = read.pool()}));
            });
    }
};
}  // namespace service::task
