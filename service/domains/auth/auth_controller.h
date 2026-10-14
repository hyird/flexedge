#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>

#include <utility>

#include "service/common/http.h"
#include "service/domains/auth/auth_schema.h"
#include "service/domains/auth/auth_service.h"
#include "service/domains/auth/auth_session_service.h"
#include "service/middleware/auth.h"

namespace service::auth {

class auth_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/auth")
    RUVIA_ROUTES_BEGIN(auth_controller)
    RUVIA_POST("/login", login, ruvia::json_body<login_body>, login_validator);
    RUVIA_POST("/refresh", refresh);
    RUVIA_POST("/logout", logout);
    RUVIA_GET("/me", me, service::middleware::auth_middleware);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> login(ruvia::context& c) {
        auto data = co_await get_auth_service().login(c, c.req().validated<login_body>());
        co_return c.json(service::common::ok<auth_session_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> refresh(ruvia::context& c) {
        auto data = co_await get_auth_session_service().refresh(c);
        co_return c.json(service::common::ok<auth_session_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> logout(ruvia::context& c) {
        co_await get_auth_session_service().logout(c);
        co_return c.json(service::common::operation(c, "退出成功"));
    }

    ruvia::task<ruvia::http_response> me(ruvia::context& c) {
        const auto& principal = service::middleware::current_principal(c);
        auto data = co_await get_auth_service().get_current_user(c, principal.admin_id_);
        co_return c.json(service::common::ok<current_user_response>(c, std::move(data)));
    }
};

}  // namespace service::auth
