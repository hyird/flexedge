#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "service/domains/auth/auth_user.h"
#include "service/domains/auth/session_credential.h"
#include "service/utils/token.h"

namespace service::auth {

enum class session_rotation_error { unknown_session,
    credential_mismatch,
    session_expired_or_revoked,
    user_disabled };

struct rotated_session final {
    session_credential credential_;
    auth_user_record user_;
};

inline ruvia::task<std::variant<rotated_session, session_rotation_error>> rotate_session(
    ruvia::db_handle db, const session_credential& presented, std::chrono::seconds expires_in) {
    auto transaction = co_await db.begin_transaction();
    const auto rows = co_await transaction.query(
        "SELECT session.family_id, session.admin_id, session.credential_hash, "
        "session.revoked_at IS NOT NULL, session.expires_at <= NOW(), admin.username, "
        "admin.nickname, admin.status FROM sys_auth_session session INNER JOIN sys_admin "
        "admin ON admin.id = session.admin_id AND admin.deleted_at IS NULL WHERE "
        "session.id::text = $1 AND EXISTS (SELECT 1 FROM sys_tenant tenant WHERE "
        "tenant.status = 'enabled' AND tenant.deleted_at IS NULL) LIMIT 1 FOR UPDATE OF "
        "session",
        presented.id_);
    if (rows.empty()) {
        co_await transaction.rollback();
        co_return session_rotation_error::unknown_session;
    }

    const auto& row = rows.front();
    const auto family_id = std::string(row[0].value().value_or(""));
    const bool secret_matches =
        service::utils::token_hash_matches(presented.secret_.view(), row[2].value().value_or(""));
    if (!secret_matches) {
        co_await transaction.rollback();
        co_return session_rotation_error::credential_mismatch;
    }
    const bool session_invalid =
        row[3].as<bool>().value_or(true) || row[4].as<bool>().value_or(true);
    if (session_invalid) {
        (void)co_await transaction.execute(
            "UPDATE sys_auth_session SET revoked_at = COALESCE(revoked_at, NOW()), "
            "updated_at = NOW() WHERE family_id = $1",
            family_id);
        co_await transaction.commit();
        co_return session_rotation_error::session_expired_or_revoked;
    }

    const auto admin_id = std::string(row[1].value().value_or(""));
    const auto username = std::string(row[5].value().value_or(""));
    const auto nickname = std::string(row[6].value().value_or(""));
    const auto status = std::string(row[7].value().value_or("disabled"));
    if (status != "enabled") {
        (void)co_await transaction.execute(
            "UPDATE sys_auth_session SET revoked_at = COALESCE(revoked_at, NOW()), "
            "updated_at = NOW() WHERE family_id = $1",
            family_id);
        co_await transaction.commit();
        co_return session_rotation_error::user_disabled;
    }

    (void)co_await transaction.execute(
        "UPDATE sys_auth_session SET rotated_at = NOW(), revoked_at = NOW(), "
        "last_used_at = NOW(), updated_at = NOW() WHERE id::text = $1",
        presented.id_);
    service::utils::sensitive_string next_secret(service::utils::random_token());
    const auto next_hash = service::utils::token_hash(next_secret.view());
    const auto created = co_await transaction.query(
        "INSERT INTO sys_auth_session (family_id, admin_id, credential_hash, "
        "expires_at) VALUES ($1, $2, $3, NOW() + CAST($4 AS BIGINT) * INTERVAL '1 "
        "second') RETURNING id",
        family_id, admin_id, next_hash, expires_in.count());
    session_credential next_session{std::string(created.front()[0].value().value_or("")),
        std::move(next_secret)};
    co_await transaction.commit();
    co_return rotated_session{std::move(next_session), {admin_id, username, nickname, status}};
}

inline ruvia::task<session_credential> create_session_credential(
    ruvia::db_handle db, const std::string& admin_id, std::chrono::seconds expires_in) {
    service::utils::sensitive_string secret(service::utils::random_token());
    const auto rows = co_await db.query(
        "INSERT INTO sys_auth_session (family_id, admin_id, credential_hash, "
        "expires_at) VALUES (COALESCE($1::uuid, gen_random_uuid()), $2, $3, NOW() + "
        "CAST($4 AS BIGINT) * INTERVAL '1 second') RETURNING id",
        ruvia::db_value{nullptr}, admin_id, service::utils::token_hash(secret.view()),
        expires_in.count());
    if (rows.empty()) {
        throw std::runtime_error("failed to create authentication session");
    }
    co_return session_credential{std::string(rows.front()[0].value().value_or("")),
        std::move(secret)};
}

inline ruvia::task<void> revoke_session_family(
    ruvia::db_handle db, const session_credential& presented) {
    (void)co_await db.execute(
        "UPDATE sys_auth_session SET revoked_at = COALESCE(revoked_at, NOW()), "
        "updated_at = NOW() WHERE family_id = (SELECT family_id FROM "
        "sys_auth_session WHERE id::text = $1 AND credential_hash = $2 LIMIT 1)",
        presented.id_, service::utils::token_hash(presented.secret_.view()));
}

}  // namespace service::auth
