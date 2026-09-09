#pragma once

#include <optional>
#include <string>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>
#include "service/domains/auth/auth_user.h"

namespace service::auth {

struct LoginUserRecord final {
    AuthUserRecord user;
    std::string passwordHash;
};

inline ruvia::Task<std::optional<LoginUserRecord>> findLoginUser(
    ruvia::DbHandle db, const std::string& username) {
    const auto rows = co_await db.query(
        "SELECT admin.id, admin.username, admin.password_hash, admin.nickname, admin.status "
        "FROM sys_admin admin WHERE admin.username = $1 AND admin.deleted_at IS NULL AND "
        "EXISTS (SELECT 1 FROM sys_tenant tenant WHERE tenant.status = 'enabled' AND "
        "tenant.deleted_at IS NULL) LIMIT 1", username);
    if (rows.empty()) co_return std::nullopt;
    const auto& row = rows.front();
    co_return LoginUserRecord{
        {std::string(row[0].value().value_or("")), std::string(row[1].value().value_or("")),
         std::string(row[3].value().value_or("")), std::string(row[4].value().value_or(""))},
        std::string(row[2].value().value_or(""))};
}

inline ruvia::Task<std::optional<AuthUserRecord>> findAuthUser(
    ruvia::DbHandle db, const std::string& adminId) {
    const auto rows = co_await db.query(
        "SELECT admin.username, admin.nickname, admin.status FROM sys_admin admin WHERE "
        "admin.id = $1 AND admin.deleted_at IS NULL AND EXISTS (SELECT 1 FROM sys_tenant "
        "tenant WHERE tenant.status = 'enabled' AND tenant.deleted_at IS NULL) LIMIT 1", adminId);
    if (rows.empty()) co_return std::nullopt;
    const auto& row = rows.front();
    co_return AuthUserRecord{adminId, std::string(row[0].value().value_or("")),
                            std::string(row[1].value().value_or("")),
                            std::string(row[2].value().value_or(""))};
}

} // namespace service::auth
