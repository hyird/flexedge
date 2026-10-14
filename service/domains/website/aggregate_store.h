#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <optional>
#include <string>

namespace service::website {

struct website_snapshot {
    std::string config_json_;
    std::string cluster_id_;
    std::int64_t revision_;
};

inline ruvia::task<std::optional<std::int64_t>> find_website_revision(
    ruvia::db_transaction& transaction, const std::string& tenant_id, const std::string& id) {
    const auto rows = co_await transaction.query(
        "SELECT revision FROM sys_website WHERE id = $1 AND tenant_id = $2 AND deleted_at IS NULL",
        id, tenant_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return rows.front()[0].as<std::int64_t>().value();
}

inline ruvia::task<std::optional<std::int64_t>> lock_website_revision(
    ruvia::db_transaction& transaction, const std::string& tenant_id, const std::string& id) {
    const auto rows = co_await transaction.query(
        "SELECT revision FROM sys_website WHERE id = $1 AND tenant_id = $2 AND "
        "deleted_at IS NULL LIMIT 1 FOR UPDATE",
        id, tenant_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return rows.front()[0].as<std::int64_t>().value();
}

inline ruvia::task<std::optional<website_snapshot>> lock_website_snapshot(
    ruvia::db_transaction& transaction, const std::string& tenant_id, const std::string& id) {
    const auto rows = co_await transaction.query(
        "SELECT config::text, cluster_id, revision FROM sys_website WHERE id = $1 AND tenant_id = $2 "
        "AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        id, tenant_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return website_snapshot{std::string(rows.front()[0].value().value()),
        std::string(rows.front()[1].value().value()),
        rows.front()[2].as<std::int64_t>().value()};
}

inline ruvia::task<std::string> insert_website(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& cluster_id, const std::string& status,
    const std::string& config_json) {
    const auto rows = co_await transaction.query(
        "INSERT INTO sys_website (tenant_id, cluster_id, status, revision, config, runtime, "
        "created_at, updated_at) VALUES ($1, $2, $3, 1, $4::jsonb, '{}'::jsonb, NOW(), NOW()) RETURNING id",
        tenant_id, cluster_id, status, config_json);
    co_return std::string(rows.front()[0].value().value());
}

inline ruvia::task<std::optional<std::int64_t>> update_website(
    ruvia::db_transaction& transaction, const std::string& tenant_id, const std::string& id,
    std::int64_t expected_revision, const std::string& cluster_id, const std::string& status,
    const std::string& config_json) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_website SET cluster_id = $1, status = $2, config = $3::jsonb, "
        "revision = revision + 1, updated_at = NOW() WHERE id = $4 AND tenant_id = $5 "
        "AND revision = $6 AND deleted_at IS NULL RETURNING revision",
        cluster_id, status, config_json, id, tenant_id, expected_revision);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return rows.front()[0].as<std::int64_t>().value();
}

inline ruvia::task<std::optional<website_snapshot>> soft_delete_website(
    ruvia::db_transaction& transaction, const std::string& tenant_id, const std::string& id,
    std::int64_t expected_revision) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_website SET deleted_at = NOW(), revision = revision + 1, updated_at = NOW() "
        "WHERE id = $1 AND tenant_id = $2 AND revision = $3 AND deleted_at IS NULL "
        "RETURNING config::text, cluster_id, revision",
        id, tenant_id, expected_revision);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return website_snapshot{std::string(rows.front()[0].value().value()),
        std::string(rows.front()[1].value().value()),
        rows.front()[2].as<std::int64_t>().value()};
}

}  // namespace service::website
