#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/middleware.h>
#include <ruvia/web/next.h>

#include <string>
#include <string_view>

#include "service/common/http.h"
#include "service/domains/auth/auth_error.h"
#include "service/features/auth_session/session_cookie.h"
#include "service/features/auth_session/session_read_service.h"
#include "service/middleware/rate_limit.h"
#include "service/middleware/request_identity.h"

namespace service::middleware {

class auth_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next) {
        const auto presented = service::auth::read_session_cookie(c);
        if (!presented) {
            service::common::throw_app_error(service::auth::auth_error::unauthorized);
        }
        const auto authenticated = co_await service::auth::resolve_session_principal(c.db(), *presented);
        if (!authenticated) {
            service::auth::delete_session_cookie(c);
            service::common::throw_app_error(service::auth::auth_error::session_invalid);
        }
        const auto binding = c.bind_request_state(*authenticated);
        co_await require_principal_budget(c, *authenticated);
        co_await next();
    }
};

}  // namespace service::middleware
