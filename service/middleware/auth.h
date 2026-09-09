#pragma once

#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/domains/auth/session_cookie.h"
#include "service/domains/auth/auth_session_read.service.h"
#include "service/domains/auth/auth.error.h"

namespace service::middleware {

inline const service::auth::AuthenticatedPrincipal& currentPrincipal(ruvia::Context& c) {
    return c.requestState<service::auth::AuthenticatedPrincipal>();
}

inline const std::string& currentTenantId(ruvia::Context& c) {
    return currentPrincipal(c).system_tenant_id;
}

class AuthMiddleware final : public ruvia::Middleware<AuthMiddleware> {
  public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        const auto presented = service::auth::readSessionCookie(c);
        if (!presented) {
            service::common::throwAppError(service::auth::AuthError::UNAUTHORIZED);
        }
        const auto authenticated = co_await service::auth::resolveSessionPrincipal(c.db(), *presented);
        if (!authenticated) {
            service::auth::deleteSessionCookie(c);
            service::common::throwAppError(service::auth::AuthError::SESSION_INVALID);
        }
        const auto binding = c.bindRequestState(*authenticated);
        co_await next();
    }
};

} // namespace service::middleware
