#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db.h>

#include <optional>
#include <string>

#include "service/domains/auth/session_credential.h"
#include "service/utils/token.h"

namespace service::auth {

struct authenticated_principal final {
    std::string admin_id_;
    std::string system_tenant_id_;
};

inline ruvia::task<std::optional<authenticated_principal>> resolve_session_principal(
    ruvia::db_handle db, const session_credential& credential) {
    const auto sessions = co_await db.query(
        "SELECT session.admin_id, tenant.id, session.credential_hash FROM "
        "sys_auth_session session INNER JOIN sys_admin admin ON admin.id = "
        "session.admin_id CROSS JOIN sys_tenant tenant WHERE session.id::text = $1 AND "
        "session.revoked_at IS NULL AND session.expires_at > NOW() AND admin.status = "
        "'enabled' AND admin.deleted_at IS NULL AND tenant.status = 'enabled' AND "
        "tenant.deleted_at IS NULL ORDER BY tenant.sort ASC LIMIT 1",
        credential.id_);
    if (sessions.empty()) {
        co_return std::nullopt;
    }
    const auto& credential_hash = sessions.front()[2].value();
    if (!credential_hash || !service::utils::token_hash_matches(credential.secret_.view(), *credential_hash)) {
        co_return std::nullopt;
    }
    co_return authenticated_principal{
        .admin_id_ = std::string(sessions.front()[0].value().value_or("")),
        .system_tenant_id_ = std::string(sessions.front()[1].value().value_or("")),
    };
}

}  // namespace service::auth
