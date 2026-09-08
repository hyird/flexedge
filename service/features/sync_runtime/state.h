#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/features/sync_event/fanout.h"

namespace service::sync_runtime {

enum class MarkerResourceType {
    provider,
    dnsZone,
    certificate,
    website,
};

enum class MarkerOperation {
    verify,
    sync,
    syncLocal,
    syncRemote,
    remove,
    issue,
    renew,
    apply,
};

[[nodiscard]] inline constexpr std::string_view resourceTypeName(MarkerResourceType resourceType) {
    switch (resourceType) {
    case MarkerResourceType::provider:
        return "provider";
    case MarkerResourceType::dnsZone:
        return "dns_zone";
    case MarkerResourceType::certificate:
        return "certificate";
    case MarkerResourceType::website:
        return "website";
    }
    throw std::invalid_argument("unsupported sync marker resource type");
}

[[nodiscard]] inline constexpr std::string_view markerOperationName(MarkerOperation operation) {
    switch (operation) {
    case MarkerOperation::verify:
        return "verify";
    case MarkerOperation::sync:
        return "sync";
    case MarkerOperation::syncLocal:
        return "sync_local";
    case MarkerOperation::syncRemote:
        return "sync_remote";
    case MarkerOperation::remove:
        return "delete";
    case MarkerOperation::issue:
        return "issue";
    case MarkerOperation::renew:
        return "renew";
    case MarkerOperation::apply:
        return "apply";
    }
    throw std::invalid_argument("unsupported sync marker operation");
}

[[nodiscard]] inline std::optional<MarkerOperation>
parseMarkerOperation(std::string_view operation) {
    if (operation == "verify") {
        return MarkerOperation::verify;
    }
    if (operation == "sync") {
        return MarkerOperation::sync;
    }
    if (operation == "sync_local") {
        return MarkerOperation::syncLocal;
    }
    if (operation == "sync_remote") {
        return MarkerOperation::syncRemote;
    }
    if (operation == "delete") {
        return MarkerOperation::remove;
    }
    if (operation == "issue") {
        return MarkerOperation::issue;
    }
    if (operation == "renew") {
        return MarkerOperation::renew;
    }
    if (operation == "apply") {
        return MarkerOperation::apply;
    }
    return std::nullopt;
}

[[nodiscard]] inline MarkerOperation requireMarkerOperation(std::string_view operation) {
    const auto parsed = parseMarkerOperation(operation);
    if (!parsed) {
        throw std::invalid_argument("invalid sync marker operation");
    }
    return *parsed;
}

[[nodiscard]] inline constexpr bool supportsMarkerOperation(MarkerResourceType resourceType,
                                                            MarkerOperation operation) {
    switch (resourceType) {
    case MarkerResourceType::provider:
        return operation == MarkerOperation::verify;
    case MarkerResourceType::dnsZone:
        return operation == MarkerOperation::sync || operation == MarkerOperation::syncLocal ||
               operation == MarkerOperation::syncRemote || operation == MarkerOperation::remove;
    case MarkerResourceType::certificate:
        return operation == MarkerOperation::issue || operation == MarkerOperation::renew;
    case MarkerResourceType::website:
        return operation == MarkerOperation::apply;
    }
    return false;
}

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

struct RunningResultTransition final {
    bool markerTransitioned{};
    bool eventRecorded{};
};

inline constexpr std::int64_t kRetryDelaySeconds{15};
inline constexpr std::int64_t kLeaseTimeoutSeconds{60};
inline constexpr std::string_view kLeaseRecoveryRetryError{"同步租约超时，已回收重试"};
inline constexpr std::string_view kEventOutcomeCompleted{"completed"};
inline constexpr std::string_view kEventOutcomeFailed{"failed"};

[[nodiscard]] inline RunningMarkerLease makeRunningLease(std::string_view tenantId,
                                                         std::string_view markerId,
                                                         std::int64_t version,
                                                         std::string_view owner) {
    if (tenantId.empty() || markerId.empty() || owner.empty() || version <= 0) {
        throw std::invalid_argument("invalid running marker lease");
    }
    return {.marker = {.tenantId = tenantId, .markerId = markerId, .version = version},
            .owner = owner};
}

namespace detail {

inline void publishRecordedResultEvent(const RunningMarkerLease& lease) {
    service::sync_event::fanout::hub().publish(lease.marker.tenantId);
}

} // namespace detail

template <typename Transaction>
inline ruvia::Task<void> commitAndPublishResultEvent(Transaction& transaction,
                                                     const RunningMarkerLease& lease,
                                                     const RunningResultTransition& transition) {
    co_await transaction.commit();
    if (transition.eventRecorded) {
        detail::publishRecordedResultEvent(lease);
    }
    co_return;
}

[[nodiscard]] inline std::string_view resourceColumn(MarkerResourceType resourceType) {
    switch (resourceType) {
    case MarkerResourceType::provider:
        return "provider_id";
    case MarkerResourceType::dnsZone:
        return "dns_zone_id";
    case MarkerResourceType::certificate:
        return "certificate_id";
    case MarkerResourceType::website:
        return "website_id";
    }
    throw std::invalid_argument("unsupported sync marker resource type");
}

inline ruvia::Task<std::string> upsertMarker(ruvia::DbTransaction& transaction,
                                             std::string_view tenantId,
                                             MarkerResourceType resourceType,
                                             std::string_view resourceId, MarkerOperation operation,
                                             std::int64_t version) {
    if (version <= 0) {
        throw std::invalid_argument("sync marker version must be positive");
    }
    if (!supportsMarkerOperation(resourceType, operation)) {
        throw std::invalid_argument("sync marker operation is not valid for this resource");
    }
    const auto column = resourceColumn(resourceType);
    const auto resourceTypeValue = resourceTypeName(resourceType);
    const auto operationValue = markerOperationName(operation);
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
        tenantId, resourceTypeValue, resourceId, operationValue, version);
    if (rows.empty()) {
        throw std::runtime_error("sync marker could not be created");
    }
    co_return std::string(rows.front()[0].value().value_or(""));
}

inline ruvia::Task<void> removeMarker(ruvia::DbTransaction& transaction, std::string_view tenantId,
                                      MarkerResourceType resourceType,
                                      std::string_view resourceId) {
    const auto column = resourceColumn(resourceType);
    (void)co_await transaction.execute(
        "DELETE FROM sys_sync_task WHERE tenant_id = $1 AND resource_type = $2 AND " +
            std::string(column) + " = $3",
        tenantId, resourceTypeName(resourceType), resourceId);
    co_return;
}

template <typename Database>
inline ruvia::Task<void> recoverStaleRunning(Database& database, MarkerResourceType resourceType) {
    (void)co_await database.execute(
        "UPDATE sys_sync_task SET is_done = FALSE, is_ok = FALSE, count_fails = count_fails + 1, "
        "error = $2, next_attempt_at = NOW() + CAST($3 AS BIGINT) * INTERVAL '1 second', "
        "lease_owner = NULL, lease_until = NULL, updated_at = NOW() WHERE resource_type = $1 "
        "AND lease_until IS NOT NULL AND lease_until <= NOW()",
        resourceTypeName(resourceType), std::string_view{kLeaseRecoveryRetryError},
        kRetryDelaySeconds);
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
inline ruvia::Task<bool> recordRunningResultEvent(Database& database,
                                                  const RunningMarkerLease& lease,
                                                  std::string_view outcome) {
    if (outcome != kEventOutcomeCompleted && outcome != kEventOutcomeFailed) {
        throw std::invalid_argument("invalid sync marker event outcome");
    }
    const auto result = co_await database.execute(
        "INSERT INTO sys_sync_event (tenant_id, task_id, resource_type, resource_id, operation, "
        "version, outcome, error) SELECT tenant_id, id, resource_type, resource_id, operation, version, "
        "$4::varchar(16), LEFT(error, 1000) FROM sys_sync_task WHERE tenant_id = $1 AND id = $2 "
        "AND version = $3 AND "
        "(($4::varchar(16) = 'completed'::varchar(16) AND is_done AND is_ok) OR "
        "($4::varchar(16) = 'failed'::varchar(16) AND NOT is_done AND NOT is_ok AND "
        "count_fails > 0))",
        lease.marker.tenantId, lease.marker.markerId, lease.marker.version, outcome);
    co_return result.affectedRows() != 0;
}

template <typename Database> inline ruvia::Task<void> pruneResultEvents(Database& database) {
    (void)co_await database.execute(
        "DELETE FROM sys_sync_event WHERE emitted_at < NOW() - INTERVAL '7 days'");
    co_return;
}

template <typename Database>
inline ruvia::Task<RunningResultTransition>
completeRunningAndRecordEvent(Database& database, const RunningMarkerLease& lease) {
    if (!co_await completeRunning(database, lease)) {
        co_return RunningResultTransition{};
    }
    co_return RunningResultTransition{
        .markerTransitioned = true,
        .eventRecorded = co_await recordRunningResultEvent(database, lease, kEventOutcomeCompleted),
    };
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

template <typename Database>
inline ruvia::Task<RunningResultTransition>
failRunningAndRecordEvent(Database& database, const RunningMarkerLease& lease,
                          std::string_view error) {
    if (!co_await failRunning(database, lease, error)) {
        co_return RunningResultTransition{};
    }
    co_return RunningResultTransition{
        .markerTransitioned = true,
        .eventRecorded = co_await recordRunningResultEvent(database, lease, kEventOutcomeFailed),
    };
}

} // namespace service::sync_runtime
