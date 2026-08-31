#pragma once

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/domains/overview/overview.service.h"
#include "service/middleware/auth.h"

namespace service::overview {

class OverviewController final : public ruvia::Controller<OverviewController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/overview", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", get);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> get(ruvia::Context& c) {
        co_return c.json(service::common::ok<OverviewResponse>(
            c, co_await overviewService().get(c, service::middleware::currentTenantId(c))));
    }
};

} // namespace service::overview
