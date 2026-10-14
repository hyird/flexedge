#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db.h>

#include <optional>
#include <string>

#include "service/domains/auth/auth_user.h"

namespace service::auth {

struct login_user_record final {
    auth_user_record user_;
    std::string password_hash_;
};

inline ruvia::task<std::optional<login_user_record>> find_login_user(
    ruvia::db_handle db, const std::string& username) {
    const auto rows = co_await db.query(
        "SELECT admin.id, admin.username, admin.password_hash, admin.nickname, admin.status "
        "FROM sys_admin admin WHERE admin.username = $1 AND admin.deleted_at IS NULL AND "
        "EXISTS (SELECT 1 FROM sys_tenant tenant WHERE tenant.status = 'enabled' AND "
        "tenant.deleted_at IS NULL) LIMIT 1",
        username);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return login_user_record{
        {std::string(row[0].value().value_or("")), std::string(row[1].value().value_or("")),
            std::string(row[3].value().value_or("")), std::string(row[4].value().value_or(""))},
        std::string(row[2].value().value_or(""))};
}

inline ruvia::task<std::optional<auth_user_record>> find_auth_user(
    ruvia::db_handle db, const std::string& admin_id) {
    const auto rows = co_await db.query(
        "SELECT admin.username, admin.nickname, admin.status FROM sys_admin admin WHERE "
        "admin.id = $1 AND admin.deleted_at IS NULL AND EXISTS (SELECT 1 FROM sys_tenant "
        "tenant WHERE tenant.status = 'enabled' AND tenant.deleted_at IS NULL) LIMIT 1",
        admin_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return auth_user_record{admin_id, std::string(row[0].value().value_or("")),
        std::string(row[1].value().value_or("")),
        std::string(row[2].value().value_or(""))};
}

}  // namespace service::auth
