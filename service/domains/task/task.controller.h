#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/task/task.service.h"
#include "service/middleware/auth.h"

namespace service::task {

class TaskController final : public ruvia::Controller<TaskController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/tasks", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        const auto [page, pageSize, skip] = service::common::requirePagination(c);

        std::optional<std::string> status;
        if (const auto value = c.req().query("status")) {
            constexpr std::array<std::string_view, 4> allowed{"pending", "running", "retry",
                                                              "completed"};
            if (std::find(allowed.begin(), allowed.end(), *value) == allowed.end()) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "status 不是有效的任务状态", 400);
            }
            status = std::string(*value);
        }

        std::optional<std::string> resourceType;
        std::optional<std::string> resourceId;
        const auto resourceTypeValue = c.req().query("resource_type");
        const auto resourceIdValue = c.req().query("resource_id");
        if (resourceTypeValue || resourceIdValue) {
            if (!resourceTypeValue || !resourceIdValue) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "resource_type 和 resource_id 必须同时提供", 400);
            }
            constexpr std::array<std::string_view, 4> allowedTypes{"provider", "dns_zone",
                                                                   "certificate", "website"};
            if (std::find(allowedTypes.begin(), allowedTypes.end(), *resourceTypeValue) ==
                allowedTypes.end()) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "resource_type 不是有效的资源类型", 400);
            }
            resourceId = service::common::parseUuid(resourceIdValue);
            if (!resourceId) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "resource_id 必须是 UUID", 400);
            }
            resourceType = std::string(*resourceTypeValue);
        }

        co_return c.json(service::common::ok<TaskPageResponse>(
            c, co_await taskService().list(c, service::middleware::currentTenantId(c), page,
                                           pageSize, skip, status, resourceType, resourceId)));
    }
};

} // namespace service::task
