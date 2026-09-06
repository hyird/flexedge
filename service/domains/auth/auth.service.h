#pragma once

#include <cstdint>
#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/auth/auth.error.h"
#include "service/domains/auth/auth_session.service.h"
#include "service/domains/auth/auth.types.h"
#include "service/domains/auth/auth_user.mapper.h"
#include "service/utils/password.h"

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

        co_await authSessionService().start(c, adminId);
        AuthSessionDto result(c);
        result.set<"user">(authUserInfo(c, adminId, persistedUsername, nickname, status));
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
        co_return authUserInfo(c, adminId, username, nickname, status);
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
};

inline AuthService& authService() { return AuthService::instance(); }

} // namespace service::auth
