#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/node/node.error.h"
#include "service/domains/node/node.types.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/log_ingest/tail.h"
#include "service/features/node_config/model.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_runtime/model.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::node {

class NodeService {
  public:
    ruvia::Task<NodePageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                      std::int64_t page, std::int64_t pageSize, std::int64_t skip,
                                      const std::optional<std::string>& keyword,
                                      const std::optional<std::string>& clusterId,
                                      const std::optional<std::string>& status,
                                      const std::optional<std::string>& registrationStatus,
                                      const std::optional<std::string>& connectionStatus) {
        std::string where =
            " FROM sys_node node INNER JOIN sys_cluster cluster ON cluster.tenant_id = "
            "node.tenant_id AND cluster.id = node.cluster_id WHERE node.tenant_id = $1 AND "
            "node.deleted_at IS NULL AND cluster.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            where += " AND node.name ILIKE $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*pattern));
        }
        if (clusterId) {
            where += " AND node.cluster_id = $" + std::to_string(params.size() + 1);
            params.emplace_back(*clusterId);
        }
        if (status) {
            where += " AND node.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }
        if (registrationStatus) {
            where += " AND node.registration_status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*registrationStatus));
        }
        if (connectionStatus) {
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where +=
                " AND (CASE WHEN node.registration_status <> 'registered' THEN 'unregistered' "
                "WHEN node.status = 'enabled' AND node.last_heartbeat_at >= NOW() - INTERVAL '90 "
                "seconds' THEN 'online' ELSE 'offline' END) = " +
                placeholder;
            params.emplace_back(std::string_view(*connectionStatus));
        }

        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows = co_await c.db().query(
            "SELECT node.id, node.cluster_id, cluster.name, node.revision, "
            "node.node_spec_revision, "
            "node.config::text, node.runtime::text, node.registration_status, CASE WHEN "
            "node.registration_status <> 'registered' THEN 'unregistered' WHEN node.status = "
            "'enabled' AND node.last_heartbeat_at >= NOW() - INTERVAL '90 seconds' THEN "
            "'online' ELSE 'offline' END, TO_CHAR(node.last_heartbeat_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), node.applied_node_spec_revision, "
            "TO_CHAR(node.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "TO_CHAR(node.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), node.name, node.status, "
            "node.active_release_id, node.active_manifest_digest" +
                where + " ORDER BY node.sort ASC LIMIT " + std::to_string(pageSize) + " OFFSET " +
                std::to_string(skip),
            params);

        NodePageDataDto result(c);
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            fillNode(c, items.emplace_back(c), row);
        }
        co_return result;
    }

    ruvia::Task<NodeCredentialsDto> create(ruvia::Context& c, const std::string& tenantId,
                                           const ruvia::ValidatedJson<NodeSaveInput>& config) {
        const auto normalized = normalize(config.value());
        if (!normalized) {
            throwCorruptConfig();
        }
        const auto& clusterId = normalized->clusterId;
        const auto& name = normalized->name;
        const auto& status = normalized->status;
        const auto& storedConfig = normalized->config;
        const auto configJson = serializeConfig(c, storedConfig);

        NodeCredentialsDto result(c);
        try {
            auto transaction = co_await c.db().beginTransaction();
            const auto agentId = service::utils::randomToken().substr(0, 32);
            service::utils::SensitiveString secret(service::utils::randomToken());
            const auto secretHash = service::utils::tokenHash(secret.view());
            const auto secretEnvelope = service::utils::sealSecret(secret.view());
            co_await requireCluster(transaction, tenantId, clusterId, storedConfig);
            co_await service::node_dispatch::ensureClusterRelease(transaction, tenantId, clusterId);
            const auto rows = co_await transaction.query(
                "INSERT INTO sys_node (tenant_id, cluster_id, name, status, "
                "registration_status, revision, node_spec_revision, applied_node_spec_revision, "
                "agent_id, node_secret_hash, node_secret_envelope, config, runtime, created_at, "
                "updated_at) "
                "VALUES ($1, $2, $3, $4, 'pending', 1, 1, 0, $5, $6, $7, $8::jsonb, "
                "'{}'::jsonb, "
                "NOW(), NOW()) RETURNING id",
                service::common::dbParams(
                    ruvia::DbValue{tenantId}, ruvia::DbValue{clusterId}, ruvia::DbValue{name},
                    ruvia::DbValue{status}, ruvia::DbValue{agentId}, ruvia::DbValue{secretHash},
                    ruvia::DbValue{secretEnvelope}, ruvia::DbValue{std::string_view(configJson)}));
            const auto nodeId = std::string(rows.front()[0].value().value_or(""));
            co_await replaceEndpointClaims(transaction, tenantId, nodeId, storedConfig);
            co_await service::cluster_dns::reconcileCluster(transaction, tenantId, clusterId);
            co_await transaction.commit();
            result.set<"nodeId">(agentId);
            result.set<"secret">(secret.view());
            result.set<"revision">(1);
        } catch (const ruvia::DbError& error) {
            if (service::common::isUniqueConstraintViolation(error, "uk_node_name") ||
                service::common::isUniqueConstraintViolation(error, "uq_node_endpoint_claim")) {
                service::common::throwAppError(NodeError::EXISTS);
            }
            throw;
        }
        co_return result;
    }

    ruvia::Task<NodeCredentialsDto> credentials(ruvia::Context& c, const std::string& tenantId,
                                                const std::string& id) {
        const auto rows = co_await c.db().query(
            "SELECT revision, agent_id, node_secret_envelope FROM sys_node WHERE "
            "id = $1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(NodeError::NOT_FOUND);
        }
        const auto envelope = rows.front()[2].value();
        if (rows.front()[1].value().value_or("").empty() || !envelope || envelope->empty()) {
            service::common::throwAppError(NodeError::CREDENTIALS_UNAVAILABLE);
        }
        service::utils::SensitiveString secret(service::utils::openSecret(*envelope));
        NodeCredentialsDto result(c);
        result.set<"nodeId">(rows.front()[1].value().value_or(""));
        result.set<"secret">(secret.view());
        result.set<"revision">(rows.front()[0].as<std::int64_t>().value_or(1));
        co_return result;
    }

    ruvia::Task<NodeLogTailDataDto>
    logs(ruvia::Context& c, const std::string& tenantId, const std::string& id, std::int64_t limit,
         const std::optional<service::log_ingest::TailCursor>& after) {
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}, ruvia::DbValue{id}};
        std::string cursorPredicate;
        if (after) {
            params.emplace_back(after->ingestedUnixMicros);
            params.emplace_back(std::string_view{after->id});
            cursorPredicate =
                " AND (entry.created_at, entry.id) > (TIMESTAMPTZ 'epoch' + $3::bigint * "
                "INTERVAL '1 microsecond', $4::uuid)";
        }
        params.emplace_back(limit);
        const auto limitParameter = "$" + std::to_string(params.size());
        const auto rows = co_await c.db().query(
            "SELECT log.id, TO_CHAR(log.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "log.level, log.category, log.message, log.ingested_unix_micros FROM sys_node node "
            "LEFT JOIN LATERAL (SELECT entry.*, ROUND(EXTRACT(EPOCH FROM entry.created_at) * "
            "1000000)::bigint AS ingested_unix_micros FROM sys_node_log entry WHERE "
            "entry.tenant_id = node.tenant_id AND entry.node_id = node.id" +
                cursorPredicate + " ORDER BY entry.created_at DESC, entry.id DESC LIMIT " +
                limitParameter +
                ") log ON TRUE WHERE node.tenant_id = $1 AND node.id = $2 AND "
                "node.deleted_at IS NULL ORDER BY log.created_at DESC NULLS LAST, log.id DESC",
            params);
        if (rows.empty()) {
            service::common::throwAppError(NodeError::NOT_FOUND);
        }

        NodeLogTailDataDto result(c);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            if (!row[0].value()) {
                continue;
            }
            auto& item = items.emplace_back(c);
            item.set<"id">(row[0].value().value_or(""));
            item.set<"occurredAt">(row[1].value().value_or(""));
            item.set<"level">(row[2].value().value_or(""));
            item.set<"category">(row[3].value().value_or(""));
            item.set<"message">(row[4].value().value_or(""));
        }
        if (!items.empty()) {
            result.set<"cursor">(service::log_ingest::encodeTailCursor(
                rows.front()[5].as<std::int64_t>().value_or(0),
                rows.front()[0].value().value_or("")));
        }
        co_return result;
    }

    ruvia::Task<void> update(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision,
                             const ruvia::ValidatedJson<NodeSaveInput>& config) {
        const auto normalized = normalize(config.value());
        if (!normalized) {
            throwCorruptConfig();
        }
        const auto& clusterId = normalized->clusterId;
        const auto& name = normalized->name;
        const auto& status = normalized->status;
        const auto& storedConfig = normalized->config;
        const auto configJson = serializeConfig(c, storedConfig);

        try {
            auto transaction = co_await c.db().beginTransaction();
            co_await requireCluster(transaction, tenantId, clusterId, storedConfig);
            co_await service::node_dispatch::ensureClusterRelease(transaction, tenantId, clusterId);
            const auto currentRows = co_await transaction.query(
                "SELECT cluster_id FROM sys_node WHERE id = $1 AND tenant_id = $2 AND "
                "deleted_at IS NULL LIMIT 1 FOR UPDATE",
                id, tenantId);
            if (currentRows.empty()) {
                service::common::throwAppError(NodeError::NOT_FOUND);
            }
            const auto previousClusterId = std::string(currentRows.front()[0].value().value_or(""));
            const auto rows = co_await transaction.query(
                "UPDATE sys_node SET cluster_id = $1, name = $2, status = $3, config = "
                "$4::jsonb, revision = revision + 1, node_spec_revision = node_spec_revision + 1, "
                "node_spec_digest = NULL, desired_release_id = (SELECT current_release_id FROM "
                "sys_cluster WHERE tenant_id = $6 AND id = $1), "
                "updated_at = NOW() WHERE id = $5 AND tenant_id = $6 AND revision = $7 AND "
                "deleted_at IS "
                "NULL RETURNING revision",
                service::common::dbParams(
                    ruvia::DbValue{clusterId}, ruvia::DbValue{name}, ruvia::DbValue{status},
                    ruvia::DbValue{std::string_view(configJson)}, ruvia::DbValue{id},
                    ruvia::DbValue{tenantId}, ruvia::DbValue{expectedRevision}));
            if (rows.empty()) {
                const auto current = co_await transaction.query(
                    "SELECT revision FROM sys_node WHERE id = $1 AND tenant_id = $2 AND "
                    "deleted_at IS NULL",
                    id, tenantId);
                if (current.empty()) {
                    service::common::throwAppError(NodeError::NOT_FOUND);
                }
                service::common::throwAppError(NodeError::REVISION_CONFLICT);
            }
            co_await replaceEndpointClaims(transaction, tenantId, id, storedConfig);
            co_await service::cluster_dns::reconcileCluster(transaction, tenantId, clusterId);
            if (previousClusterId != clusterId) {
                (void)co_await transaction.execute(
                    "UPDATE sys_node_release_target target SET status = 'excluded', updated_at = "
                    "NOW() FROM sys_cluster_release release WHERE target.tenant_id = $1 AND "
                    "target.node_id = $2 AND target.status IN ('pending', 'failed') AND "
                    "release.tenant_id = target.tenant_id AND release.id = target.release_id AND "
                    "release.cluster_id = $3",
                    tenantId, id, previousClusterId);
                co_await service::cluster_dns::reconcileCluster(transaction, tenantId,
                                                                previousClusterId);
            }
            if (status == "disabled") {
                (void)co_await transaction.execute(
                    "UPDATE sys_node_release_target SET status = 'excluded', updated_at = NOW() "
                    "WHERE tenant_id = $1 AND node_id = $2 AND status IN ('pending', 'failed')",
                    tenantId, id);
            }
            co_await transaction.commit();
        } catch (const ruvia::DbError& error) {
            if (service::common::isUniqueConstraintViolation(error, "uk_node_name") ||
                service::common::isUniqueConstraintViolation(error, "uq_node_endpoint_claim")) {
                service::common::throwAppError(NodeError::EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "UPDATE sys_node SET deleted_at = NOW(), revision = revision + 1, "
            "registration_status = 'pending', node_secret_hash = NULL, "
            "node_secret_envelope = NULL, agent_id = NULL, "
            "registered_at = NULL, last_heartbeat_at = NULL, updated_at = NOW() WHERE id = $1 "
            "AND tenant_id = $2 AND revision = $3 AND deleted_at IS NULL RETURNING cluster_id",
            id, tenantId, expectedRevision);
        if (rows.empty()) {
            const auto current = co_await transaction.query(
                "SELECT revision FROM sys_node WHERE id = $1 AND tenant_id = $2 AND deleted_at "
                "IS NULL",
                id, tenantId);
            if (current.empty()) {
                service::common::throwAppError(NodeError::NOT_FOUND);
            }
            service::common::throwAppError(NodeError::REVISION_CONFLICT);
        }
        const auto clusterId = std::string(rows.front()[0].value().value_or(""));
        (void)co_await transaction.execute(
            "DELETE FROM sys_node_endpoint_claim WHERE tenant_id = $1 AND node_id = $2", tenantId,
            id);
        (void)co_await transaction.execute(
            "UPDATE sys_node_release_target SET status = 'excluded', updated_at = NOW() WHERE "
            "tenant_id = $1 AND node_id = $2 AND status IN ('pending', 'failed')",
            tenantId, id);
        co_await service::cluster_dns::reconcileCluster(transaction, tenantId, clusterId);
        co_await transaction.commit();
        co_return;
    }

    ruvia::Task<NodeCredentialsDto> resetCredentials(ruvia::Context& c, const std::string& tenantId,
                                                     const std::string& id,
                                                     std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto currentRows = co_await transaction.query(
            "SELECT revision, cluster_id FROM sys_node WHERE id = $1 AND tenant_id = $2 AND "
            "deleted_at IS NULL LIMIT 1",
            id, tenantId);
        if (currentRows.empty()) {
            service::common::throwAppError(NodeError::NOT_FOUND);
        }
        if (currentRows.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(NodeError::REVISION_CONFLICT);
        }
        const auto currentClusterId = currentRows.front()[1].value().value_or("");
        co_await service::node_dispatch::ensureClusterRelease(transaction, tenantId,
                                                              currentClusterId);
        const auto agentId = service::utils::randomToken().substr(0, 32);
        service::utils::SensitiveString secret(service::utils::randomToken());
        const auto hash = service::utils::tokenHash(secret.view());
        const auto envelope = service::utils::sealSecret(secret.view());
        const auto rows = co_await transaction.query(
            "UPDATE sys_node SET agent_id = $3, node_secret_hash = $4, node_secret_envelope = $5, "
            "registration_status = 'pending', registered_at = NULL, last_heartbeat_at = NULL, "
            "applied_node_spec_revision = 0, node_spec_digest = NULL, desired_release_id = "
            "(SELECT current_release_id FROM sys_cluster WHERE tenant_id = $2 AND id = "
            "sys_node.cluster_id), active_release_id = NULL, active_manifest_digest = NULL, "
            "last_apply_phase = NULL, last_apply_error_code = NULL, last_apply_error = NULL, "
            "last_apply_retryable = NULL, "
            "runtime = '{}'::jsonb, revision = "
            "revision + 1, updated_at = "
            "NOW() WHERE id = $1 AND "
            "tenant_id = $2 AND revision = $6 AND deleted_at IS NULL RETURNING revision, "
            "cluster_id",
            id, tenantId, agentId, std::string_view(hash), std::string_view(envelope),
            expectedRevision);
        if (rows.empty()) {
            const auto current = co_await transaction.query(
                "SELECT revision FROM sys_node WHERE id = $1 AND tenant_id = $2 AND deleted_at "
                "IS NULL",
                id, tenantId);
            if (current.empty()) {
                service::common::throwAppError(NodeError::NOT_FOUND);
            }
            service::common::throwAppError(NodeError::REVISION_CONFLICT);
        }
        const auto clusterId = std::string(rows.front()[1].value().value_or(""));
        (void)co_await transaction.execute(
            "UPDATE sys_node SET desired_release_id = (SELECT current_release_id FROM sys_cluster "
            "WHERE tenant_id = $1 AND id = $3) WHERE tenant_id = $1 AND id = $2",
            tenantId, id, clusterId);
        (void)co_await transaction.execute(
            "UPDATE sys_node_release_target SET status = 'excluded', updated_at = NOW() WHERE "
            "tenant_id = $1 AND node_id = $2 AND status IN ('pending', 'failed')",
            tenantId, id);
        co_await service::cluster_dns::reconcileCluster(transaction, tenantId, clusterId);
        co_await transaction.commit();
        NodeCredentialsDto result(c);
        result.set<"nodeId">(agentId);
        result.set<"secret">(secret.view());
        result.set<"revision">(rows.front()[0].as<std::int64_t>().value_or(expectedRevision + 1));
        co_return result;
    }

  private:
    template <typename Row> static void fillNode(ruvia::Context& c, NodeDto& item, const Row& row) {
        const std::optional<service::node_config::NodeConfigData> config =
            service::node_config::parseStored(row[5].value().value_or("{}"),
                                              {.resource = c.resource()});
        const auto runtime = service::node_runtime::parseStored(row[6].value().value_or("{}"),
                                                                {.resource = c.resource()});
        if (!config || !runtime) {
            throwCorruptConfig();
        }

        item.set<"id">(row[0].value().value_or(""));
        item.set<"clusterId">(row[1].value().value_or(""));
        item.set<"clusterName">(row[2].value().value_or(""));
        item.set<"name">(row[13].value().value_or(""));
        item.set<"status">(row[14].value().value_or(""));
        item.set<"revision">(row[3].template as<std::int64_t>().value_or(1));
        item.set<"nodeSpecRevision">(row[4].template as<std::int64_t>().value_or(1));
        item.set<"config">(toConfig(c, *config));
        item.set<"runtime">(toRuntime(c, *runtime, row));
        item.set<"createdAt">(row[11].value().value_or(""));
        item.set<"updatedAt">(row[12].value().value_or(""));
    }

    static service::node_config::NodeConfigOutput
    toConfig(ruvia::Context& c, const service::node_config::NodeConfigData& input) {
        return service::node_config::toOutput(input, {.resource = c.resource()});
    }

    static std::string serializeConfig(ruvia::Context& c,
                                       const service::node_config::NodeConfigData& input) {
        const auto output = service::node_config::toOutput(input, {.resource = c.resource()});
        const auto json = ruvia::toJson(output, {.resource = c.resource()});
        return std::string(json.data(), json.size());
    }

    template <typename Row>
    static NodeRuntimeDto toRuntime(ruvia::Context& c,
                                    const service::node_runtime::NodeRuntimeData& input,
                                    const Row& row) {
        NodeRuntimeDto output(c);
        output.set<"registrationStatus">(row[7].value().value_or("pending"));
        output.set<"connectionStatus">(row[8].value().value_or("unregistered"));
        output.set<"appliedNodeSpecRevision">(row[10].template as<std::int64_t>().value_or(0));
        if (const auto& lastHeartbeatAt = row[9].value()) {
            output.set<"lastHeartbeatAt">(*lastHeartbeatAt);
        }
        if (input.agentVersion) {
            output.set<"agentVersion">(*input.agentVersion);
        }
        if (input.cpuUsage) {
            output.set<"cpuUsage">(*input.cpuUsage);
        }
        if (input.memoryUsage) {
            output.set<"memoryUsage">(*input.memoryUsage);
        }
        if (input.trafficOutBps) {
            output.set<"trafficOutBps">(*input.trafficOutBps);
        }
        if (input.connectionCount) {
            output.set<"connectionCount">(*input.connectionCount);
        }
        if (input.load1m) {
            output.set<"load1m">(*input.load1m);
        }
        if (input.queuedLogEvents) {
            output.set<"queuedLogEvents">(*input.queuedLogEvents);
        }
        if (input.droppedLogEvents) {
            output.set<"droppedLogEvents">(*input.droppedLogEvents);
        }
        if (input.health) {
            output.set<"health">(*input.health);
        }
        if (input.lastError) {
            output.set<"lastError">(*input.lastError);
        }
        if (const auto& activeReleaseId = row[15].value()) {
            output.set<"activeReleaseId">(*activeReleaseId);
        }
        if (const auto& activeManifestDigest = row[16].value()) {
            output.set<"activeManifestDigest">(*activeManifestDigest);
        }
        return output;
    }

    static ruvia::Task<void> requireCluster(ruvia::DbTransaction& transaction,
                                            const std::string& tenantId,
                                            const std::string& clusterId,
                                            const service::node_config::NodeConfigData& config) {
        const auto rows = co_await transaction.query(
            "SELECT zone.runtime::text FROM sys_cluster cluster INNER JOIN sys_dns_zone zone ON "
            "zone.tenant_id = cluster.tenant_id AND zone.id = cluster.dns_zone_id WHERE "
            "cluster.id = $1 AND cluster.tenant_id = $2 AND cluster.status = 'enabled' AND "
            "cluster.deleted_at IS NULL AND zone.deleted_at IS NULL LIMIT 1 FOR SHARE OF cluster, "
            "zone",
            clusterId, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(NodeError::CLUSTER_UNAVAILABLE);
        }
        const auto runtime =
            service::dns_sync::parseStoredRuntime(rows.front()[0].value().value_or("{}"));
        if (!runtime) {
            throwCorruptConfig();
        }
        for (const auto& endpoint : config.endpoints) {
            const auto& lineCode = endpoint.lineCode;
            const auto line = std::ranges::find_if(
                runtime->lines, [&](const service::dns_sync::ZoneLineRuntimeData& item) {
                    return item.code && item.status && *item.code == lineCode &&
                           *item.status == "enabled";
                });
            if (line == runtime->lines.end()) {
                service::common::throwAppError(NodeError::DNS_LINE_INVALID);
            }
        }
        co_return;
    }

    static ruvia::Task<void>
    replaceEndpointClaims(ruvia::DbTransaction& transaction, const std::string& tenantId,
                          const std::string& nodeId,
                          const service::node_config::NodeConfigData& config) {
        (void)co_await transaction.execute(
            "DELETE FROM sys_node_endpoint_claim WHERE tenant_id = $1 AND node_id = $2", tenantId,
            nodeId);
        if (config.endpoints.empty()) {
            co_return;
        }
        std::vector<ruvia::DbValue> endpointInsertParams;
        endpointInsertParams.reserve(config.endpoints.size() * 4);
        std::string endpointInsertSql =
            "INSERT INTO sys_node_endpoint_claim (tenant_id, node_id, endpoint_id, "
            "ip_address) VALUES ";
        const auto appendParam = [&](ruvia::DbValue value) {
            endpointInsertSql += "$" + std::to_string(endpointInsertParams.size() + 1);
            endpointInsertParams.push_back(std::move(value));
        };
        for (const auto& endpoint : config.endpoints) {
            if (!endpointInsertParams.empty()) {
                endpointInsertSql += ", ";
            }
            endpointInsertSql += "(";
            appendParam(ruvia::DbValue{tenantId});
            endpointInsertSql += ", ";
            appendParam(ruvia::DbValue{nodeId});
            endpointInsertSql += ", ";
            appendParam(ruvia::DbValue{endpoint.id});
            endpointInsertSql += ", ";
            appendParam(ruvia::DbValue{endpoint.ipAddress});
            endpointInsertSql += "::inet)";
        }
        (void)co_await transaction.execute(endpointInsertSql, endpointInsertParams);
        co_return;
    }

    [[noreturn]] static void throwCorruptConfig() {
        service::common::throwAppError(service::common::kServerErrorCode, "聚合配置损坏", 500);
    }
};

inline NodeService& nodeService() {
    static NodeService service;
    return service;
}

} // namespace service::node
