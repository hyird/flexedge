#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <ruvia/web/Controller.h>
#include "service/common/http.h"
#include "service/domains/task/task.service.h"
#include "service/middleware/auth.h"

namespace service::task {
class TaskController final : public ruvia::Controller<TaskController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/tasks", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list);
    RUVIA_GET("/:id", detail);
    RUVIA_GET("/:id/history", history);
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
    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        const auto [page, size, skip] = service::common::requirePagination(c);
        const auto type =
            filter(c, "type", {"provider", "dns_zone", "certificate", "website", "node"});
        const auto status = filter(
            c, "status",
            {"queued", "running", "retrying", "completed", "recovered", "failed", "superseded"});
        const auto days = filter(c, "days", {"0", "1", "7"});
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        co_return c.json(service::common::ok<TaskPageResponse>(
            c, co_await taskService().list(c, service::middleware::currentTenantId(c), page, size,
                                           skip, type, status, keyword.value_or(""),
                                           days.empty() ? 0 : std::stoll(days))));
    }
    ruvia::Task<ruvia::HttpResponse> history(ruvia::Context& c) {
        const auto id = service::common::requireUuidParam(c, "id");
        const auto version = service::common::parseInt64(c.req().query("version").value_or(""));
        if (!version || *version < 1) {
            service::common::throwAppError(service::common::kValidationErrorCode, "任务版本不正确",
                                           400);
        }
        co_return c.json(service::common::ok<TaskHistoryResponse>(
            c, co_await taskService().history(c, service::middleware::currentTenantId(c), id,
                                              *version)));
    }
    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        const auto id = service::common::requireUuidParam(c, "id");
        const auto resourceId =
            service::common::parseUuid(c.req().query("resource_id").value_or(""));
        const auto version = service::common::parseInt64(c.req().query("version").value_or(""));
        if (!resourceId || !version || *version < 1) {
            service::common::throwAppError(service::common::kValidationErrorCode, "任务标识不正确",
                                           400);
        }
        co_return c.json(service::common::ok<TaskResponse>(
            c, co_await taskService().detail(c, service::middleware::currentTenantId(c), id,
                                             *resourceId, *version)));
    }
};
} // namespace service::task
