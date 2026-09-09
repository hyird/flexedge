#pragma once

#include "service/features/node_dispatch/notifications.h"
#include "service/features/live_resource/fanout.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
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
#include "service/domains/node/endpoint_claim.store.h"
#include "service/domains/node/node.mapper.h"
#include "service/domains/node/node.types.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/line.h"
#include "service/features/node_config/model.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_dispatch/target.store.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::node {

class NodeCommandService final {
  public:
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
            co_await replaceEndpointClaims(transaction, tenantId, nodeId, storedConfig.endpoints);
            co_await service::cluster_dns::reconcileCluster(transaction, tenantId, clusterId);
            co_await transaction.commit();
            service::node_dispatch::notifications::published(tenantId);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::nodes, nodeId);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::clusters, clusterId);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks);
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
            co_await replaceEndpointClaims(transaction, tenantId, id, storedConfig.endpoints);
            co_await service::cluster_dns::reconcileCluster(transaction, tenantId, clusterId);
            if (previousClusterId != clusterId) {
                co_await service::node_dispatch::excludePendingNodeClusterTargets(transaction, tenantId, id, previousClusterId);
                co_await service::cluster_dns::reconcileCluster(transaction, tenantId,
                                                                previousClusterId);
            }
            if (status == "disabled") {
                co_await service::node_dispatch::excludePendingNodeTargets(transaction, tenantId, id);
            }
            co_await transaction.commit();
            service::node_dispatch::notifications::published(tenantId);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::nodes, id);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::clusters, clusterId);
            if (previousClusterId != clusterId)
                service::live_resource::hub().publish(tenantId, service::live_resource::Resource::clusters, previousClusterId);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks);
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
        co_await clearEndpointClaims(transaction, tenantId, id);
        co_await service::node_dispatch::excludePendingNodeTargets(transaction, tenantId, id);
        co_await service::cluster_dns::reconcileCluster(transaction, tenantId, clusterId);
        co_await transaction.commit();
        service::node_dispatch::notifications::published(tenantId);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::nodes, id);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::clusters, clusterId);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks);
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
        co_await service::node_dispatch::excludePendingNodeTargets(transaction, tenantId, id);
        co_await service::cluster_dns::reconcileCluster(transaction, tenantId, clusterId);
        co_await transaction.commit();
        service::node_dispatch::notifications::published(tenantId);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::nodes, id);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::clusters, clusterId);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks);
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
                    return service::dns_sync::isEnabledLine(item) && *item.code == lineCode;
                });
            if (line == runtime->lines.end()) {
                service::common::throwAppError(NodeError::DNS_LINE_INVALID);
            }
        }
        co_return;
    }


};

inline NodeCommandService& nodeCommandService() {
    static NodeCommandService service;
    return service;
}

} // namespace service::node
