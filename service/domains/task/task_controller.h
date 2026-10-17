#pragma once

#include <ruvia/web/controller.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/http.h"
#include "service/domains/task/task_service.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"

namespace service::task {

// Task center: one task per (resource_type, resource_id), addressed by both.
// The list and the task are read over GET and streamed while a live view is
// open; stored traces never change, so a trace is read over GET only.
class task_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/tasks", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(task_controller)
    RUVIA_GET("/", list);
    RUVIA_GET_SSE("/stream", list_stream);
    RUVIA_GET("/:type/:id", detail);
    RUVIA_GET_SSE("/:type/:id/stream", detail_stream);
    RUVIA_GET("/:type/:id/attempts/:attempt_id/trace", attempt_trace);
    RUVIA_ROUTES_END
private:
    struct list_query final {
        std::int64_t page_{};
        std::int64_t size_{};
        std::int64_t skip_{};
        list_filter filter_;
    };

    // The `:type` path parameter: a task resource type.
    static std::string require_type(ruvia::context& c) {
        const auto value = c.req().param("type").value_or("");
        for (const auto option : {"website", "dns_zone", "certificate", "provider", "node", "cache"}) {
            if (value == option) {
                return std::string(value);
            }
        }
        service::common::throw_app_error(service::common::validation_error_code, "任务类型不正确", 400);
    }

    static list_query parse_list(ruvia::context& c) {
        const auto [page, size, skip] = service::common::require_pagination(c, 20, 100);
        return {.page_ = page,
            .size_ = size,
            .skip_ = skip,
            .filter_ = {
                .status_ = service::common::require_enum_query(c, "status",
                    {"queued", "running", "retrying", "failed", "completed", "active",
                        "attention"},
                    "任务筛选条件不正确")
                    .value_or(""),
                .type_ = service::common::require_enum_query(c, "type",
                    {"website", "dns_zone", "certificate", "provider", "node", "cache"}, "任务筛选条件不正确")
                    .value_or(""),
                .keyword_ = service::common::require_keyword(c.req().query("keyword")).value_or(""),
            }};
    }

    static ruvia::task<task_page_response> read_list(auto& read, const std::string& tenant, const list_query& q) {
        auto data = co_await get_task_service().list(read, tenant, q.page_, q.size_, q.skip_, q.filter_);
        co_return service::common::ok<task_page_response>(read, std::move(data));
    }

    static ruvia::task<task_response> read_detail(auto& read, const std::string& tenant, const std::string& type,
        const std::string& id) {
        auto data = co_await get_task_service().detail(read, tenant, type, id);
        co_return service::common::ok<task_response>(read, std::move(data));
    }

    static ruvia::task<task_history_response> read_history(auto& read, const std::string& tenant,
        const std::string& type, const std::string& id) {
        auto data = co_await get_task_service().history(read, tenant, type, id);
        co_return service::common::ok<task_history_response>(read, std::move(data));
    }

    ruvia::task<ruvia::http_response> list(ruvia::context& c) {
        co_return c.json(co_await read_list(c, service::middleware::current_tenant_id(c), parse_list(c)));
    }

    // Live list of the tasks page.
    ruvia::task<void> list_stream(ruvia::context& c) {
        const auto q = parse_list(c);
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::tasks, {},
            service::live_resource::query_key("list", q.page_, q.size_, q.filter_.status_, q.filter_.type_,
                q.filter_.keyword_));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, q](auto& read) -> ruvia::task<task_page_response> {
                co_return co_await read_list(read, tenant, q);
            });
    }

    ruvia::task<ruvia::http_response> detail(ruvia::context& c) {
        const auto type = require_type(c);
        co_return c.json(co_await read_detail(c, service::middleware::current_tenant_id(c), type,
            service::common::require_uuid_param(c, "id")));
    }

    // The page's single live connection while a task is open: `snapshot`
    // (the task), `history` (its execution history; initial and recovery
    // snapshot) and one `attempt` per attempt recorded afterwards.
    ruvia::task<void> detail_stream(ruvia::context& c) {
        namespace live = service::live_resource;
        const auto type = require_type(c);
        const auto id = service::common::require_uuid_param(c, "id");
        const auto tenant = service::middleware::current_tenant_id(c);
        std::vector<live::subscription_part> parts;
        parts.push_back({live::resource::tasks, id, live::query_key("detail", type)});
        parts.push_back({live::resource::tasks, id, live::query_key("history", type), true});
        auto sub = live::get_hub(c).subscribe(c.worker(), tenant, std::move(parts));
        std::vector<live::snapshot_part> snapshots;
        snapshots.push_back(live::make_snapshot_part("snapshot",
            [tenant, type, id](auto& read) -> ruvia::task<task_response> {
                co_return co_await read_detail(read, tenant, type, id);
            }));
        auto history_part = live::make_snapshot_part("history",
            [tenant, type, id](auto& read) -> ruvia::task<task_history_response> {
                co_return co_await read_history(read, tenant, type, id);
            });
        history_part.update_event_ = "attempt";
        history_part.update_ = [tenant, type, id](auto& read,
                                   std::vector<live::task_attempt_notice> notices)
            -> ruvia::task<std::vector<std::string>> {
            co_return co_await get_task_service().recorded_attempts(read, tenant, type, id, notices);
        };
        snapshots.push_back(std::move(history_part));
        co_await live::stream_snapshots(c, std::move(sub), std::move(snapshots));
    }

    // One attempt's stored trace.
    ruvia::task<ruvia::http_response> attempt_trace(ruvia::context& c) {
        const auto type = require_type(c);
        const auto id = service::common::require_uuid_param(c, "id");
        const auto attempt_id = service::common::parse_int64(c.req().param("attempt_id"));
        if (!attempt_id || *attempt_id < 1) {
            service::common::throw_app_error(service::common::validation_error_code, "执行记录标识不正确", 400);
        }
        const auto tenant = service::middleware::current_tenant_id(c);
        auto data = co_await get_task_service().attempt_trace(c, tenant, type, id, *attempt_id);
        co_return c.json(service::common::ok<attempt_trace_response>(c, std::move(data)));
    }
};
}  // namespace service::task
