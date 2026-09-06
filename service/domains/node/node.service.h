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
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/node/node.error.h"
#include "service/domains/node/node.mapper.h"
#include "service/domains/node/node_read.service.h"
#include "service/domains/node/node.types.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/log_ingest/tail.h"
#include "service/features/node_config/model.h"
#include "service/features/node_dispatch/queue.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::node {

class NodeService final {
  public:
    ruvia::Task<NodePageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                      std::int64_t page, std::int64_t pageSize, std::int64_t skip,
                                      const std::optional<std::string>& keyword,
                                      const std::optional<std::string>& clusterId,
                                      const std::optional<std::string>& status,
                                      const std::optional<std::string>& registrationStatus,
                                      const std::optional<std::string>& connectionStatus) {
        co_return co_await nodeReadService().list(c, tenantId, page, pageSize, skip, keyword,
                                                  clusterId, status, registrationStatus,
                                                  connectionStatus);
    }

    ruvia::Task<NodeCredentialsDto> create(ruvia::Context& c, const std::string& tenantId,
                                           const ruvia::ValidatedJson<NodeSaveInput>& config) {
        const auto normalized = normalize(config.value());
        if (!normalized) {
            throwCorruptNodeConfig();
        }
        const auto& clusterId = normalized->clusterId;
        const auto& name = normalized->name;
        const auto& status = normalized->status;
        const auto& storedConfig = normalized->config;
        const auto configJson = serializeNodeConfig(c, storedConfig);

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
        co_return co_await nodeReadService().credentials(c, tenantId, id);
    }

    ruvia::Task<NodeLogTailDataDto>
    logs(ruvia::Context& c, const std::string& tenantId, const std::string& id, std::int64_t limit,
         const std::optional<service::log_ingest::TailCursor>& after) {
        co_return co_await nodeReadService().logs(c, tenantId, id, limit, after);
    }

    ruvia::Task<void> update(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision,
                             const ruvia::ValidatedJson<NodeSaveInput>& config) {
        const auto normalized = normalize(config.value());
        if (!normalized) {
            throwCorruptNodeConfig();
        }
        const auto& clusterId = normalized->clusterId;
        const auto& name = normalized->name;
        const auto& status = normalized->status;
        const auto& storedConfig = normalized->config;
        const auto configJson = serializeNodeConfig(c, storedConfig);

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
            throwCorruptNodeConfig();
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
};

inline NodeService& nodeService() {
    static NodeService service;
    return service;
}

} // namespace service::node
