#pragma once

#include <ruvia/web/controller.h>

#include <initializer_list>
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

// Task center (docs/task-center.md): tasks are user operations and system
// actions, subtasks the background work they caused.
class task_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/operations", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(task_controller)
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/:id/stream", detail);
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
        service::common::throw_app_error(service::common::validation_error_code, "任务筛选条件不正确", 400);
    }

    // Live list. The popover and the overview read its first frame once.
    ruvia::task<void> list(ruvia::context& c) {
        const auto [page, size, skip] = service::common::require_pagination(c, 20, 100);
        list_filter filter_value{
            .status_ = filter(c, "status",
                {"queued", "running", "retrying", "failed", "partial_failed", "attention_required", "completed",
                    "merged", "cancelled", "active", "attention"}),
            .action_ = std::string(c.req().query("action").value_or("")),
            .target_type_ =
                filter(c, "target_type", {"website", "cluster", "node", "dns_zone", "certificate", "provider"}),
            .actor_ = filter(c, "actor", {"me", "system"}),
            .admin_id_ = service::middleware::current_principal(c).admin_id_,
            .days_ = 0,
            .keyword_ = service::common::require_keyword(c.req().query("keyword")).value_or(""),
        };
        if (filter_value.action_.size() > 48) {
            service::common::throw_app_error(service::common::validation_error_code, "任务筛选条件不正确", 400);
        }
        const auto days = filter(c, "days", {"0", "1", "7"});
        filter_value.days_ = days.empty() ? 0 : std::stoll(days);
        const auto tenant = service::middleware::current_tenant_id(c);
        // "me" depends on the session's administrator: part of the cache key.
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::operations, {},
            service::live_resource::query_key("list", page, size, filter_value.status_, filter_value.action_,
                filter_value.target_type_, filter_value.actor_,
                filter_value.actor_ == "me" ? filter_value.admin_id_ : std::string{}, filter_value.days_,
                filter_value.keyword_));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, page, size, skip, filter_value](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_task_service().list(read, tenant, page, size, skip, filter_value);
                co_return std::string(ruvia::to_json(service::common::ok<operation_page_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    // The page's single live connection while a task is open: `snapshot`
    // (the task with its subtasks) and, with `subtask_id`, that subtask's
    // execution history as `history` (initial and recovery snapshot) plus
    // one `attempt` per attempt recorded afterwards.
    ruvia::task<void> detail(ruvia::context& c) {
        namespace live = service::live_resource;
        const auto id = service::common::require_uuid_param(c, "id");
        std::string subtask_id;
        if (const auto value = c.req().query("subtask_id"); value && !value->empty()) {
            const auto parsed = service::common::parse_uuid(*value);
            if (!parsed) {
                service::common::throw_app_error(service::common::validation_error_code, "subtask_id 必须是 UUID",
                    400);
            }
            subtask_id = std::string(*parsed);
        }
        const auto tenant = service::middleware::current_tenant_id(c);
        std::vector<live::subscription_part> parts;
        parts.push_back({live::resource::operations, std::string(id), live::query_key("detail", subtask_id)});
        if (!subtask_id.empty()) {
            parts.push_back(
                {live::resource::operations, std::string(id), live::query_key("history", subtask_id), true});
        }
        auto sub = live::get_hub(c).subscribe(c.worker(), tenant, std::move(parts));
        std::vector<live::snapshot_part> snapshots;
        snapshots.push_back({.event_ = "snapshot",
            .fetch_ = [tenant, id, subtask_id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_task_service().detail(read, tenant, id, subtask_id);
                co_return std::string(ruvia::to_json(
                    service::common::ok<operation_detail_response>(read, std::move(data)), {.resource_ = read.pool()}));
            }});
        if (!subtask_id.empty()) {
            snapshots.push_back({.event_ = "history",
                .fetch_ = [tenant, id, subtask_id](auto& read) -> ruvia::task<std::string> {
                    auto data = co_await get_task_service().history(read, tenant, id, subtask_id);
                    co_return std::string(ruvia::to_json(
                        service::common::ok<subtask_history_response>(read, std::move(data)), {.resource_ = read.pool()}));
                },
                .update_event_ = "attempt",
                .update_ = [tenant, subtask_id](auto& read,
                               std::vector<live::task_attempt_notice> notices) -> ruvia::task<std::vector<std::string>> {
                    co_return co_await get_task_service().recorded_attempts(read, tenant, subtask_id, notices);
                }});
        }
        co_await live::stream_snapshots(c, std::move(sub), std::move(snapshots));
    }

    // One-shot snapshot of one attempt's trace; traces never change once
    // stored, so the client closes the stream after the first frame.
    ruvia::task<void> attempt_trace(ruvia::context& c) {
        const auto id = service::common::require_uuid_param(c, "id");
        const auto attempt_id = service::common::parse_int64(c.req().param("attempt_id"));
        if (!attempt_id || *attempt_id < 1) {
            service::common::throw_app_error(service::common::validation_error_code, "执行记录标识不正确", 400);
        }
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::operations, id,
            service::live_resource::query_key("trace", *attempt_id));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, id, attempt_id = *attempt_id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_task_service().attempt_trace(read, tenant, id, attempt_id);
                co_return std::string(ruvia::to_json(service::common::ok<attempt_trace_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }
};
}  // namespace service::task
