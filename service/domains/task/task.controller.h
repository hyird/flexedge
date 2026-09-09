#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <ruvia/web/Controller.h>
#include "service/common/http.h"
#include "service/domains/task/task.service.h"
#include "service/middleware/auth.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"

namespace service::task {
class TaskController final : public ruvia::Controller<TaskController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/tasks", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/:id/stream", detail);
    RUVIA_GET_SSE("/:id/history/stream", history);
    RUVIA_ROUTES_END
  private:
    static std::string filter(ruvia::Context& c, std::string_view name,
                              std::initializer_list<std::string_view> allowed) {
        const auto value = c.req().query(name).value_or("");
        if (value.empty())
            return {};
        for (const auto option : allowed)
            if (value == option)
                return std::string(value);
        service::common::throwAppError(service::common::kValidationErrorCode, "任务筛选条件不正确",
                                       400);
    }
    ruvia::Task<void> list(ruvia::Context& c) {
        const auto [page, size, skip] = service::common::requirePagination(c);
        const auto type =
            filter(c, "type", {"provider", "dns_zone", "certificate", "website", "node"});
        const auto status = filter(
            c, "status",
            {"queued", "running", "retrying", "completed", "recovered", "failed", "superseded"});
        const auto days = filter(c, "days", {"0", "1", "7"});
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        const auto tenant = service::middleware::currentTenantId(c);
        const auto daysValue = days.empty() ? 0LL : std::stoll(days);
        auto sub = service::live_resource::hub().subscribe(c.worker(), tenant,
            service::live_resource::Resource::tasks, {},
            service::live_resource::queryKey("list", page, size, type, status,
                                             keyword.value_or(""), daysValue));
        co_await service::live_resource::streamSnapshot(c, std::move(sub),
            [tenant, page, size, skip, type, status, keyword = keyword.value_or(""), days = daysValue](auto& read) -> ruvia::Task<std::string> {
                auto data = co_await taskService().list(read, tenant, page, size, skip, type, status, keyword, days);
                co_return std::string(ruvia::toJson(service::common::ok<TaskPageResponse>(read, std::move(data)), {.resource = read.resource()}));
            });
    }
    ruvia::Task<void> history(ruvia::Context& c) {
        const auto id = service::common::requireUuidParam(c, "id");
        const auto version = service::common::parseInt64(c.req().query("version").value_or(""));
        if (!version || *version < 1) {
            service::common::throwAppError(service::common::kValidationErrorCode, "任务版本不正确",
                                           400);
        }
        const auto tenant = service::middleware::currentTenantId(c);
        auto sub = service::live_resource::hub().subscribe(c.worker(), tenant,
            service::live_resource::Resource::tasks, id,
            service::live_resource::queryKey("history", version.value()));
        co_await service::live_resource::streamSnapshot(c, std::move(sub),
            [tenant, id, version = *version](auto& read) -> ruvia::Task<std::string> {
                auto data = co_await taskService().history(read, tenant, id, version);
                co_return std::string(ruvia::toJson(service::common::ok<TaskHistoryResponse>(read, std::move(data)), {.resource = read.resource()}));
            });
    }
    ruvia::Task<void> detail(ruvia::Context& c) {
        const auto id = service::common::requireUuidParam(c, "id");
        const auto resourceId =
            service::common::parseUuid(c.req().query("resource_id").value_or(""));
        const auto version = service::common::parseInt64(c.req().query("version").value_or(""));
        if (!resourceId || !version || *version < 1) {
            service::common::throwAppError(service::common::kValidationErrorCode, "任务标识不正确",
                                           400);
        }
        const auto tenant = service::middleware::currentTenantId(c);
        auto sub = service::live_resource::hub().subscribe(c.worker(), tenant,
            service::live_resource::Resource::tasks, id,
            service::live_resource::queryKey("detail", *resourceId, *version));
        co_await service::live_resource::streamSnapshot(c, std::move(sub),
            [tenant, id, resourceId = *resourceId, version = *version](auto& read) -> ruvia::Task<std::string> {
                auto data = co_await taskService().detail(read, tenant, id, resourceId, version);
                co_return std::string(ruvia::toJson(service::common::ok<TaskResponse>(read, std::move(data)), {.resource = read.resource()}));
            });
    }
};
} // namespace service::task
