#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

namespace service::website {

struct WebsiteSnapshot {
    std::string configJson;
    std::string clusterId;
    std::int64_t revision;
};

inline ruvia::Task<std::optional<std::int64_t>> findWebsiteRevision(
    ruvia::DbTransaction& transaction, const std::string& tenantId, const std::string& id) {
    const auto rows = co_await transaction.query(
        "SELECT revision FROM sys_website WHERE id = $1 AND tenant_id = $2 AND deleted_at IS NULL",
        id, tenantId);
    if (rows.empty()) co_return std::nullopt;
    co_return rows.front()[0].as<std::int64_t>().value();
}

inline ruvia::Task<std::optional<std::int64_t>> lockWebsiteRevision(
    ruvia::DbTransaction& transaction, const std::string& tenantId, const std::string& id) {
    const auto rows = co_await transaction.query(
        "SELECT revision FROM sys_website WHERE id = $1 AND tenant_id = $2 AND "
        "deleted_at IS NULL LIMIT 1 FOR UPDATE", id, tenantId);
    if (rows.empty()) co_return std::nullopt;
    co_return rows.front()[0].as<std::int64_t>().value();
}

inline ruvia::Task<std::optional<WebsiteSnapshot>> lockWebsiteSnapshot(
    ruvia::DbTransaction& transaction, const std::string& tenantId, const std::string& id) {
    const auto rows = co_await transaction.query(
        "SELECT config::text, cluster_id, revision FROM sys_website WHERE id = $1 AND tenant_id = $2 "
        "AND deleted_at IS NULL LIMIT 1 FOR UPDATE", id, tenantId);
    if (rows.empty()) co_return std::nullopt;
    co_return WebsiteSnapshot{std::string(rows.front()[0].value().value()),
                              std::string(rows.front()[1].value().value()),
                              rows.front()[2].as<std::int64_t>().value()};
}

inline ruvia::Task<std::string> insertWebsite(ruvia::DbTransaction& transaction,
    const std::string& tenantId, const std::string& clusterId, const std::string& status,
    const std::string& configJson) {
    const auto rows = co_await transaction.query(
        "INSERT INTO sys_website (tenant_id, cluster_id, status, revision, config, runtime, "
        "created_at, updated_at) VALUES ($1, $2, $3, 1, $4::jsonb, '{}'::jsonb, NOW(), NOW()) RETURNING id",
        tenantId, clusterId, status, configJson);
    co_return std::string(rows.front()[0].value().value());
}

inline ruvia::Task<std::optional<std::int64_t>> updateWebsite(
    ruvia::DbTransaction& transaction, const std::string& tenantId, const std::string& id,
    std::int64_t expectedRevision, const std::string& clusterId, const std::string& status,
    const std::string& configJson) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_website SET cluster_id = $1, status = $2, config = $3::jsonb, "
        "revision = revision + 1, updated_at = NOW() WHERE id = $4 AND tenant_id = $5 "
        "AND revision = $6 AND deleted_at IS NULL RETURNING revision",
        clusterId, status, configJson, id, tenantId, expectedRevision);
    if (rows.empty()) co_return std::nullopt;
    co_return rows.front()[0].as<std::int64_t>().value();
}

inline ruvia::Task<std::optional<WebsiteSnapshot>> softDeleteWebsite(
    ruvia::DbTransaction& transaction, const std::string& tenantId, const std::string& id,
    std::int64_t expectedRevision) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_website SET deleted_at = NOW(), revision = revision + 1, updated_at = NOW() "
        "WHERE id = $1 AND tenant_id = $2 AND revision = $3 AND deleted_at IS NULL "
        "RETURNING config::text, cluster_id, revision", id, tenantId, expectedRevision);
    if (rows.empty()) co_return std::nullopt;
    co_return WebsiteSnapshot{std::string(rows.front()[0].value().value()),
                              std::string(rows.front()[1].value().value()),
                              rows.front()[2].as<std::int64_t>().value()};
}

} // namespace service::website
