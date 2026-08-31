#pragma once

#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/utils/auth_session.h"
#include "service/utils/token.h"

namespace service::middleware {

struct AuthenticatedPrincipal final {
    std::string admin_id;
    std::string system_tenant_id;
};

inline const AuthenticatedPrincipal& currentPrincipal(ruvia::Context& c) {
    return c.requestState<AuthenticatedPrincipal>();
}

inline const std::string& currentTenantId(ruvia::Context& c) {
    return currentPrincipal(c).system_tenant_id;
}

class AuthMiddleware final : public ruvia::Middleware<AuthMiddleware> {
  public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        const auto presented = service::auth::readSessionCookie(c);
        if (!presented) {
            service::common::throwAppError(service::common::kAuthUnauthorizedErrorCode, "未登录",
                                           401);
        }
        const auto sessions = co_await c.db().query(
            "SELECT session.admin_id, tenant.id, session.credential_hash FROM "
            "sys_auth_session session INNER JOIN sys_admin admin ON admin.id = "
            "session.admin_id CROSS JOIN sys_tenant tenant WHERE session.id::text = $1 AND "
            "session.revoked_at IS NULL AND session.expires_at > NOW() AND admin.status = "
            "'enabled' AND admin.deleted_at IS NULL AND tenant.status = 'enabled' AND "
            "tenant.deleted_at IS NULL ORDER BY tenant.sort ASC LIMIT 1",
            presented->id);
        if (sessions.empty()) {
            service::auth::deleteSessionCookie(c);
            service::common::throwAppError(service::common::kAuthSessionInvalidErrorCode,
                                           "会话已失效", 401);
        }
        const auto& credentialHash = sessions.front()[2].value();
        if (!credentialHash ||
            !service::utils::tokenHashMatches(presented->secret.view(), *credentialHash)) {
            service::auth::deleteSessionCookie(c);
            service::common::throwAppError(service::common::kAuthSessionInvalidErrorCode,
                                           "会话已失效", 401);
        }
        const AuthenticatedPrincipal authenticated{
            .admin_id = std::string(sessions.front()[0].value().value_or("")),
            .system_tenant_id = std::string(sessions.front()[1].value().value_or("")),
        };
        const auto binding = c.bindRequestState(authenticated);
        co_await next();
    }
};

} // namespace service::middleware
