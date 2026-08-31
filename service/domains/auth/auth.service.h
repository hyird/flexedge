#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/auth/auth.error.h"
#include "service/domains/auth/auth.types.h"
#include "service/utils/auth_session.h"
#include "service/utils/password.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::auth {

class AuthService {
  public:
    static AuthService& instance() {
        static AuthService svc;
        return svc;
    }

    ruvia::Task<AuthSessionDto> login(ruvia::Context& c, const LoginBody& req) {
        const auto& usernameInput = req.get<"username">();
        const auto& passwordInput = req.get<"password">();
        if (!usernameInput || !passwordInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "用户名和密码不能为空", 400);
        }
        const std::string username(usernameInput->view());
        const auto password = passwordInput->view();

        if (const auto remaining = co_await remainingLockSeconds(c, username); remaining > 0) {
            const auto minutes = (remaining + 59) / 60;
            service::common::throwAppError(
                AuthError::TOO_MANY_ATTEMPTS.code,
                "登录失败次数过多，请" + std::to_string(minutes) + "分钟后再试", 429);
        }

        auto db = c.db();
        const auto admins = co_await db.query(
            "SELECT admin.id, admin.username, admin.password_hash, admin.nickname, admin.status "
            "FROM sys_admin admin WHERE admin.username = $1 AND admin.deleted_at IS NULL AND "
            "EXISTS (SELECT 1 FROM sys_tenant tenant WHERE tenant.status = 'enabled' AND "
            "tenant.deleted_at IS NULL) LIMIT 1",
            username);
        if (admins.empty()) {
            (void)service::utils::comparePassword(password, service::utils::kDummyPasswordHash);
            const int failureCount = co_await recordFailure(c, username);
            const int remaining = 5 - failureCount;
            if (remaining > 0) {
                service::common::throwAppError(
                    AuthError::PASSWORD_INCORRECT.code,
                    "用户名或密码错误，还剩" + std::to_string(remaining) + "次尝试机会", 401);
            }
            service::common::throwAppError(AuthError::TOO_MANY_ATTEMPTS);
        }

        const auto& row = admins.front();
        const std::string adminId(row[0].value().value_or(""));
        const std::string persistedUsername(row[1].value().value_or(""));
        const std::string passwordHash(row[2].value().value_or(""));
        const std::string nickname =
            !row[3].value().has_value() ? std::string{} : std::string(row[3].value().value_or(""));
        const std::string status(row[4].value().value_or(""));

        if (!service::utils::comparePassword(password, passwordHash)) {
            const int failureCount = co_await recordFailure(c, username);
            const int remaining = 5 - failureCount;
            if (remaining > 0) {
                service::common::throwAppError(
                    AuthError::PASSWORD_INCORRECT.code,
                    "用户名或密码错误，还剩" + std::to_string(remaining) + "次尝试机会", 401);
            }
            service::common::throwAppError(AuthError::TOO_MANY_ATTEMPTS);
        }
        if (status == "disabled") {
            service::common::throwAppError(AuthError::USER_DISABLED);
        }

        (void)co_await c.db().execute("DELETE FROM sys_auth_login_throttle WHERE username = $1",
                                      username);

        const auto session = co_await createSession(c, adminId, std::nullopt);
        setSessionCookie(c, session);
        AuthSessionDto result(c);
        result.set<"user">(buildAdminInfo(c, adminId, persistedUsername, nickname, status));
        co_return result;
    }

    ruvia::Task<AuthSessionDto> refresh(ruvia::Context& c) {
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
        result.set<"user">(buildAdminInfo(c, adminId, username, nickname, status));
        co_return result;
    }

    ruvia::Task<AuthUserInfoDto> getCurrentUser(ruvia::Context& c, const std::string& adminId) {
        auto db = c.db();
        const auto admins = co_await db.query(
            "SELECT admin.username, admin.nickname, admin.status FROM sys_admin admin WHERE "
            "admin.id = $1 AND admin.deleted_at IS NULL AND EXISTS (SELECT 1 FROM sys_tenant "
            "tenant WHERE tenant.status = 'enabled' AND tenant.deleted_at IS NULL) LIMIT 1",
            adminId);
        if (admins.empty()) {
            service::common::throwAppError(AuthError::USER_NOT_FOUND);
        }
        const auto& row = admins.front();
        const std::string username(row[0].value().value_or(""));
        const std::string nickname =
            !row[1].value().has_value() ? std::string{} : std::string(row[1].value().value_or(""));
        const std::string status(row[2].value().value_or(""));
        if (status == "disabled") {
            service::common::throwAppError(AuthError::USER_DISABLED);
        }
        co_return buildAdminInfo(c, adminId, username, nickname, status);
    }

    ruvia::Task<void> logout(ruvia::Context& c) {
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
    AuthService() = default;

    static ruvia::Task<std::int64_t> remainingLockSeconds(ruvia::Context& c,
                                                          const std::string& username) {
        const auto rows = co_await c.db().query(
            "SELECT CEIL(EXTRACT(EPOCH FROM locked_until - NOW()))::bigint FROM "
            "sys_auth_login_throttle WHERE username = $1 AND locked_until > NOW() LIMIT 1",
            username);
        co_return rows.empty() ? std::int64_t{0} : rows.front()[0].as<std::int64_t>().value_or(0);
    }

    static ruvia::Task<int> recordFailure(ruvia::Context& c, const std::string& username) {
        const auto rows = co_await c.db().query(
            "INSERT INTO sys_auth_login_throttle (username, failure_count, window_started_at, "
            "locked_until, updated_at) VALUES ($1, 1, NOW(), NULL, NOW()) ON CONFLICT (username) "
            "DO UPDATE SET failure_count = CASE WHEN "
            "sys_auth_login_throttle.window_started_at < NOW() - INTERVAL '15 minutes' THEN 1 "
            "ELSE sys_auth_login_throttle.failure_count + 1 END, window_started_at = CASE WHEN "
            "sys_auth_login_throttle.window_started_at < NOW() - INTERVAL '15 minutes' THEN NOW() "
            "ELSE sys_auth_login_throttle.window_started_at END, locked_until = CASE WHEN "
            "sys_auth_login_throttle.window_started_at >= NOW() - INTERVAL '15 minutes' AND "
            "sys_auth_login_throttle.failure_count + 1 >= 5 THEN NOW() + INTERVAL '15 minutes' "
            "ELSE NULL END, updated_at = NOW() RETURNING failure_count",
            username);
        co_return rows.empty() ? 1
                               : static_cast<int>(rows.front()[0].as<std::int64_t>().value_or(1));
    }

    static ruvia::Task<SessionCredential>
    createSession(ruvia::Context& c, const std::string& adminId,
                  const std::optional<std::string>& familyId) {
        service::utils::SensitiveString secret(service::utils::randomToken());
        const auto rows = co_await c.db().query(
            "INSERT INTO sys_auth_session (family_id, admin_id, credential_hash, "
            "expires_at) VALUES (COALESCE($1::uuid, gen_random_uuid()), $2, $3, NOW() + "
            "CAST($4 AS BIGINT) * INTERVAL '1 second') RETURNING id",
            familyId ? ruvia::DbValue{std::string_view(*familyId)} : ruvia::DbValue{nullptr},
            adminId, service::utils::tokenHash(secret.view()), authSessionExpiresIn().count());
        if (rows.empty()) {
            throw std::runtime_error("failed to create authentication session");
        }
        co_return SessionCredential{std::string(rows.front()[0].value().value_or("")),
                                    std::move(secret)};
    }

    static AuthUserInfoDto buildAdminInfo(ruvia::Context& c, const std::string& adminId,
                                          const std::string& username, const std::string& nickname,
                                          const std::string& status) {
        AuthUserInfoDto info(c);
        info.set<"id">(adminId);
        info.set<"username">(username);
        info.set<"nickname">(nickname);
        info.set<"status">(status);
        return info;
    }
};

inline AuthService& authService() { return AuthService::instance(); }

} // namespace service::auth
