#pragma once

#include <optional>
#include <string>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>
#include "service/domains/auth/session_credential.h"
#include "service/utils/token.h"

namespace service::auth {

struct AuthenticatedPrincipal final {
    std::string admin_id;
    std::string system_tenant_id;
};

inline ruvia::Task<std::optional<AuthenticatedPrincipal>> resolveSessionPrincipal(
    ruvia::DbHandle db, const SessionCredential& credential) {
    const auto sessions = co_await db.query(
        "SELECT session.admin_id, tenant.id, session.credential_hash FROM "
        "sys_auth_session session INNER JOIN sys_admin admin ON admin.id = "
        "session.admin_id CROSS JOIN sys_tenant tenant WHERE session.id::text = $1 AND "
        "session.revoked_at IS NULL AND session.expires_at > NOW() AND admin.status = "
        "'enabled' AND admin.deleted_at IS NULL AND tenant.status = 'enabled' AND "
        "tenant.deleted_at IS NULL ORDER BY tenant.sort ASC LIMIT 1",
        credential.id);
    if (sessions.empty()) co_return std::nullopt;
    const auto& credentialHash = sessions.front()[2].value();
    if (!credentialHash || !service::utils::tokenHashMatches(credential.secret.view(), *credentialHash)) {
        co_return std::nullopt;
    }
    co_return AuthenticatedPrincipal{
        .admin_id = std::string(sessions.front()[0].value().value_or("")),
        .system_tenant_id = std::string(sessions.front()[1].value().value_or("")),
    };
}

} // namespace service::auth
