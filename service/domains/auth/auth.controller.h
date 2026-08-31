#pragma once

#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/domains/auth/auth.schema.h"
#include "service/domains/auth/auth.service.h"

namespace service::auth {

class AuthController final : public ruvia::Controller<AuthController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/auth")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/login", login, LoginValidator);
    RUVIA_POST("/refresh", refresh);
    RUVIA_POST("/logout", logout);
    RUVIA_GET("/me", me, service::middleware::AuthMiddleware);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> login(ruvia::Context& c) {
        auto data = co_await authService().login(c, c.req().validated<LoginBody>());
        co_return c.json(service::common::ok<AuthSessionResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> refresh(ruvia::Context& c) {
        auto data = co_await authService().refresh(c);
        co_return c.json(service::common::ok<AuthSessionResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> logout(ruvia::Context& c) {
        co_await authService().logout(c);
        co_return c.json(service::common::operation(c, "退出成功"));
    }

    ruvia::Task<ruvia::HttpResponse> me(ruvia::Context& c) {
        const auto& principal = service::middleware::currentPrincipal(c);
        auto data = co_await authService().getCurrentUser(c, principal.admin_id);
        co_return c.json(service::common::ok<CurrentUserResponse>(c, std::move(data)));
    }
};

} // namespace service::auth
