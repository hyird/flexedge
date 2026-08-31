#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

namespace service::sync_runtime {

// A sync task is a coalesced, current-state marker. It is deliberately not a
// historical workflow node: the aggregate remains the source of truth and
// version fences decide whether a worker result is still current.
struct MarkerReference final {
    std::string_view tenantId;
    std::string_view markerId;
    std::int64_t version{};
};

struct RunningMarkerLease final {
    MarkerReference marker;
    std::string_view owner;
};

inline constexpr std::int64_t kRetryDelaySeconds{15};
inline constexpr std::int64_t kLeaseTimeoutSeconds{60};
inline constexpr std::string_view kLeaseRecoveryRetryError{"同步租约超时，已回收重试"};

[[nodiscard]] inline std::string_view resourceColumn(std::string_view resourceType) {
    if (resourceType == "provider") {
        return "provider_id";
    }
    if (resourceType == "dns_zone") {
        return "dns_zone_id";
    }
    if (resourceType == "certificate") {
        return "certificate_id";
    }
    if (resourceType == "website") {
        return "website_id";
    }
    throw std::invalid_argument("unsupported sync marker resource type");
}

inline ruvia::Task<std::string> upsertMarker(ruvia::DbTransaction& transaction,
                                             std::string_view tenantId,
                                             std::string_view resourceType,
                                             std::string_view resourceId,
                                             std::string_view operation, std::int64_t version) {
    if (version <= 0) {
        throw std::invalid_argument("sync marker version must be positive");
    }
    const auto column = resourceColumn(resourceType);
    const auto rows = co_await transaction.query(
        "INSERT INTO sys_sync_task (tenant_id, resource_type, " + std::string(column) +
            ", operation, version, processed_version, is_done, is_ok, error, count_fails, "
            "next_attempt_at, created_at, updated_at) VALUES ($1, $2, $3, $4, $5, 0, FALSE, "
            "FALSE, '', 0, NOW(), NOW(), NOW()) ON CONFLICT (tenant_id, resource_type, "
            "resource_id) DO UPDATE SET version = GREATEST(sys_sync_task.version, "
            "EXCLUDED.version), operation = CASE WHEN EXCLUDED.version >= sys_sync_task.version "
            "THEN EXCLUDED.operation ELSE sys_sync_task.operation END, is_done = CASE WHEN "
            "EXCLUDED.version >= sys_sync_task.version THEN FALSE ELSE sys_sync_task.is_done END, "
            "is_ok = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN FALSE ELSE "
            "sys_sync_task.is_ok END, processed_version = CASE WHEN EXCLUDED.version >= "
            "sys_sync_task.version THEN 0 ELSE sys_sync_task.processed_version END, error = CASE "
            "WHEN EXCLUDED.version >= sys_sync_task.version THEN '' ELSE sys_sync_task.error END, "
            "count_fails = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN 0 ELSE "
            "sys_sync_task.count_fails END, next_attempt_at = CASE WHEN EXCLUDED.version >= "
            "sys_sync_task.version THEN NOW() ELSE sys_sync_task.next_attempt_at END, updated_at = "
            "CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN NOW() ELSE "
            "sys_sync_task.updated_at END, lease_owner = CASE WHEN EXCLUDED.version > "
            "sys_sync_task.version THEN NULL ELSE sys_sync_task.lease_owner END, lease_until = "
            "CASE WHEN EXCLUDED.version > sys_sync_task.version THEN NULL ELSE "
            "sys_sync_task.lease_until END RETURNING id",
        tenantId, resourceType, resourceId, operation, version);
    if (rows.empty()) {
        throw std::runtime_error("sync marker could not be created");
    }
    co_return std::string(rows.front()[0].value().value_or(""));
}

inline ruvia::Task<void> removeMarker(ruvia::DbTransaction& transaction, std::string_view tenantId,
                                      std::string_view resourceType, std::string_view resourceId) {
    const auto column = resourceColumn(resourceType);
    (void)co_await transaction.execute(
        "DELETE FROM sys_sync_task WHERE tenant_id = $1 AND resource_type = $2 AND " +
            std::string(column) + " = $3",
        tenantId, resourceType, resourceId);
    co_return;
}

template <typename Database>
inline ruvia::Task<void> recoverStaleRunning(Database& database, std::string_view resourceType) {
    (void)co_await database.execute(
        "UPDATE sys_sync_task SET is_done = FALSE, is_ok = FALSE, count_fails = count_fails + 1, "
        "error = $2, next_attempt_at = NOW() + CAST($3 AS BIGINT) * INTERVAL '1 second', "
        "lease_owner = NULL, lease_until = NULL, updated_at = NOW() WHERE resource_type = $1 "
        "AND lease_until IS NOT NULL AND lease_until <= NOW()",
        resourceType, std::string_view{kLeaseRecoveryRetryError}, kRetryDelaySeconds);
    co_return;
}

template <typename Database>
inline ruvia::Task<bool> renewRunningLease(Database& database, const RunningMarkerLease& lease) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET lease_until = NOW() + INTERVAL '60 seconds' WHERE "
        "tenant_id = $1 AND id = $2 AND version = $3 AND lease_owner = $4 AND "
        "lease_until IS NOT NULL",
        lease.marker.tenantId, lease.marker.markerId, lease.marker.version, lease.owner);
    co_return result.affectedRows() != 0;
}

template <typename Database>
inline ruvia::Task<bool> completeRunning(Database& database, const RunningMarkerLease& lease) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET is_done = CASE WHEN version = $3 THEN TRUE ELSE FALSE END, "
        "is_ok = CASE WHEN version = $3 THEN TRUE ELSE FALSE END, "
        "processed_version = CASE WHEN version = $3 THEN $3 ELSE processed_version END, "
        "error = CASE WHEN version = $3 THEN '' ELSE error END, count_fails = CASE WHEN version = "
        "$3 THEN 0 ELSE count_fails END, lease_owner = NULL, lease_until = NULL, "
        "next_attempt_at = NOW(), updated_at = NOW() WHERE tenant_id = $1 AND id = $2 AND "
        "lease_owner = $4 AND lease_until IS NOT NULL",
        lease.marker.tenantId, lease.marker.markerId, lease.marker.version, lease.owner);
    co_return result.affectedRows() != 0;
}

template <typename Database>
inline ruvia::Task<bool> releaseRunning(Database& database, const RunningMarkerLease& lease) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET lease_owner = NULL, lease_until = NULL, next_attempt_at = "
        "NOW(), updated_at = NOW() WHERE tenant_id = $1 AND id = $2 AND lease_owner = $3 AND "
        "lease_until IS NOT NULL",
        lease.marker.tenantId, lease.marker.markerId, lease.owner);
    co_return result.affectedRows() != 0;
}

template <typename Database>
inline ruvia::Task<bool> removeRunning(Database& database, const RunningMarkerLease& lease) {
    const auto result = co_await database.execute(
        "DELETE FROM sys_sync_task WHERE tenant_id = $1 AND id = $2 AND version = $3 AND "
        "lease_owner = $4 AND lease_until IS NOT NULL",
        lease.marker.tenantId, lease.marker.markerId, lease.marker.version, lease.owner);
    co_return result.affectedRows() != 0;
}

template <typename Database>
inline ruvia::Task<bool> failRunning(Database& database, const RunningMarkerLease& lease,
                                     std::string_view error) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET is_done = FALSE, is_ok = FALSE, count_fails = count_fails + 1, "
        "error = $5, next_attempt_at = NOW() + CAST($6 AS BIGINT) * INTERVAL '1 second', "
        "lease_owner = NULL, lease_until = NULL, updated_at = NOW() WHERE tenant_id = $1 AND "
        "id = $2 AND version = $3 AND lease_owner = $4 AND lease_until IS NOT NULL",
        lease.marker.tenantId, lease.marker.markerId, lease.marker.version, lease.owner, error,
        kRetryDelaySeconds);
    co_return result.affectedRows() != 0;
}

} // namespace service::sync_runtime
