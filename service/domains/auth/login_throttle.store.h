#pragma once

#include <cstdint>
#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>

namespace service::auth {

inline ruvia::Task<std::int64_t> remainingLoginLockSeconds(ruvia::DbHandle db,
                                                       const std::string& username) {
    const auto rows = co_await db.query(
        "SELECT CEIL(EXTRACT(EPOCH FROM locked_until - NOW()))::bigint FROM "
        "sys_auth_login_throttle WHERE username = $1 AND locked_until > NOW() LIMIT 1",
        username);
    co_return rows.empty() ? std::int64_t{0} : rows.front()[0].as<std::int64_t>().value_or(0);
}

inline ruvia::Task<int> recordLoginFailure(ruvia::DbHandle db, const std::string& username) {
    const auto rows = co_await db.query(
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

inline ruvia::Task<void> clearLoginFailures(ruvia::DbHandle db, const std::string& username) {
    (void)co_await db.execute("DELETE FROM sys_auth_login_throttle WHERE username = $1", username);
}

} // namespace service::auth
