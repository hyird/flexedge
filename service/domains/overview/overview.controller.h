#pragma once

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/Controller.h>
#include <utility>

#include "service/common/http.h"
#include "service/domains/overview/overview.service.h"
#include "service/middleware/auth.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"

namespace service::overview {

class OverviewController final : public ruvia::Controller<OverviewController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/overview", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/stream", get);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<void> get(ruvia::Context& c) {
        const auto tenant = service::middleware::currentTenantId(c);
        auto sub = service::live_resource::hub().subscribe(c.worker(), tenant,
            service::live_resource::Resource::overview, {},
            service::live_resource::queryKey("overview"));
        co_await service::live_resource::streamSnapshot(c, std::move(sub),
            [tenant](auto& read) -> ruvia::Task<std::string> {
                auto data = co_await service::overview::overviewService().get(read, tenant);
                co_return std::string(ruvia::toJson(service::common::ok<OverviewResponse>(
                    read, std::move(data)), {.resource = read.resource()}));
            });
    }
};

} // namespace service::overview
