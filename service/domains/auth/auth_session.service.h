#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/domains/auth/auth.error.h"
#include "service/domains/auth/auth.types.h"
#include "service/domains/auth/auth_user.mapper.h"
#include "service/utils/auth_session.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::auth {

class AuthSessionService final {
  public:
    ruvia::Task<void> start(ruvia::Context& c, const std::string& adminId) const {
        const auto session = co_await create(c, adminId);
        setSessionCookie(c, session);
        co_return;
    }

    ruvia::Task<AuthSessionDto> refresh(ruvia::Context& c) const {
        const auto presented = readSessionCookie(c);
        if (!presented) {
            service::common::throwAppError(AuthError::UNAUTHORIZED);
        }

        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "SELECT session.family_id, session.admin_id, session.credential_hash, "
            "session.revoked_at IS NOT NULL, session.expires_at <= NOW(), admin.username, "
            "admin.nickname, admin.status FROM sys_auth_session session INNER JOIN sys_admin "
            "admin ON admin.id = session.admin_id AND admin.deleted_at IS NULL WHERE "
            "session.id::text = $1 AND EXISTS (SELECT 1 FROM sys_tenant tenant WHERE "
            "tenant.status = 'enabled' AND tenant.deleted_at IS NULL) LIMIT 1 FOR UPDATE OF "
            "session",
            presented->id);
        if (rows.empty()) {
            co_await transaction.rollback();
            service::common::throwAppError(AuthError::SESSION_INVALID);
        }

        const auto& row = rows.front();
        const auto familyId = std::string(row[0].value().value_or(""));
        const bool secretMatches =
            service::utils::tokenHashMatches(presented->secret.view(), row[2].value().value_or(""));
        if (!secretMatches) {
            co_await transaction.rollback();
            deleteSessionCookie(c);
            service::common::throwAppError(AuthError::SESSION_INVALID);
        }
        const bool sessionInvalid =
            row[3].as<bool>().value_or(true) || row[4].as<bool>().value_or(true);
        if (sessionInvalid) {
            (void)co_await transaction.execute(
                "UPDATE sys_auth_session SET revoked_at = COALESCE(revoked_at, NOW()), "
                "updated_at = NOW() WHERE family_id = $1",
                familyId);
            co_await transaction.commit();
            deleteSessionCookie(c);
            service::common::throwAppError(AuthError::SESSION_INVALID);
        }

        const auto adminId = std::string(row[1].value().value_or(""));
        const auto username = std::string(row[5].value().value_or(""));
        const auto nickname = std::string(row[6].value().value_or(""));
        const auto status = std::string(row[7].value().value_or("disabled"));
        if (status != "enabled") {
            (void)co_await transaction.execute(
                "UPDATE sys_auth_session SET revoked_at = COALESCE(revoked_at, NOW()), "
                "updated_at = NOW() WHERE family_id = $1",
                familyId);
            co_await transaction.commit();
            deleteSessionCookie(c);
            service::common::throwAppError(AuthError::USER_DISABLED);
        }

        (void)co_await transaction.execute(
            "UPDATE sys_auth_session SET rotated_at = NOW(), revoked_at = NOW(), "
            "last_used_at = NOW(), updated_at = NOW() WHERE id::text = $1",
            presented->id);
        service::utils::SensitiveString nextSecret(service::utils::randomToken());
        const auto nextHash = service::utils::tokenHash(nextSecret.view());
        const auto created = co_await transaction.query(
            "INSERT INTO sys_auth_session (family_id, admin_id, credential_hash, "
            "expires_at) VALUES ($1, $2, $3, NOW() + CAST($4 AS BIGINT) * INTERVAL '1 "
            "second') RETURNING id",
            familyId, adminId, nextHash, authSessionExpiresIn().count());
        const SessionCredential nextSession{std::string(created.front()[0].value().value_or("")),
                                            std::move(nextSecret)};
        co_await transaction.commit();
        setSessionCookie(c, nextSession);

        AuthSessionDto result(c);
        result.set<"user">(authUserInfo(c, adminId, username, nickname, status));
        co_return result;
    }

    ruvia::Task<void> logout(ruvia::Context& c) const {
        const auto presented = readSessionCookie(c);
        if (presented) {
            (void)co_await c.db().execute(
                "UPDATE sys_auth_session SET revoked_at = COALESCE(revoked_at, NOW()), "
                "updated_at = NOW() WHERE family_id = (SELECT family_id FROM "
                "sys_auth_session WHERE id::text = $1 AND credential_hash = $2 LIMIT 1)",
                presented->id, service::utils::tokenHash(presented->secret.view()));
        }
        deleteSessionCookie(c);
        co_return;
    }

  private:
    static ruvia::Task<SessionCredential> create(ruvia::Context& c, const std::string& adminId) {
        service::utils::SensitiveString secret(service::utils::randomToken());
        const auto rows = co_await c.db().query(
            "INSERT INTO sys_auth_session (family_id, admin_id, credential_hash, "
            "expires_at) VALUES (COALESCE($1::uuid, gen_random_uuid()), $2, $3, NOW() + "
            "CAST($4 AS BIGINT) * INTERVAL '1 second') RETURNING id",
            ruvia::DbValue{nullptr}, adminId, service::utils::tokenHash(secret.view()),
            authSessionExpiresIn().count());
        if (rows.empty()) {
            throw std::runtime_error("failed to create authentication session");
        }
        co_return SessionCredential{std::string(rows.front()[0].value().value_or("")),
                                    std::move(secret)};
    }
};

inline const AuthSessionService& authSessionService() {
    static const AuthSessionService service;
    return service;
}

} // namespace service::auth
