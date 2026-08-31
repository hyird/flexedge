#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent.error.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_runtime/model.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::agent {

struct AgentPrincipal final {
    std::string nodeId;
    std::string clusterId;
    std::string tenantId;
    std::string agentId;
};

struct DesiredSummary final {
    std::int64_t nodeSpecRevision{};
    std::string releaseId;
    std::string manifestDigest;
};

struct HeartbeatReport final {
    struct OriginHealth final {
        std::string websiteId;
        std::string originId;
        std::string status;
        std::int64_t checkedAtUnixMillis{};
        std::int64_t latencyMillis{};
        std::string lastError;
    };
    std::string nodeId;
    std::int64_t appliedNodeSpecRevision{};
    std::string activeReleaseId;
    std::string activeManifestDigest;
    std::string agentVersion;
    double cpuUsage{};
    double memoryUsage{};
    std::int64_t trafficOutBps{};
    std::int64_t connectionCount{};
    double load1m{};
    std::int64_t queuedLogEvents{};
    std::int64_t droppedLogEvents{};
    std::string health;
    std::string lastError;
    std::vector<OriginHealth> originHealth;
};

class AgentService final {
  public:
    ruvia::Task<AgentPrincipal> authenticate(ruvia::Context& c, const std::string& agentId,
                                             std::string_view secret) {
        auto transaction = co_await c.db().beginTransaction();
        const auto candidates = co_await transaction.query(
            "SELECT id, cluster_id, tenant_id, registration_status, node_secret_hash FROM "
            "sys_node WHERE agent_id = $1 AND deleted_at IS NULL LIMIT 1",
            agentId);
        if (candidates.empty()) {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        const auto storedHash = candidates.front()[4].value();
        if (!storedHash || !service::utils::tokenHashMatches(secret, *storedHash)) {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        const auto initialStatus = candidates.front()[3].value().value_or("");
        if (initialStatus != "pending" && initialStatus != "registered") {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        const auto nodeId = candidates.front()[0].value().value_or("");
        const auto clusterId = candidates.front()[1].value().value_or("");
        const auto tenantId = candidates.front()[2].value().value_or("");
        co_await service::node_dispatch::ensureClusterRelease(transaction, tenantId, clusterId);
        const auto rows = co_await transaction.query(
            "SELECT id, cluster_id, tenant_id, registration_status, node_secret_hash FROM "
            "sys_node WHERE id = $1 AND cluster_id = $2 AND tenant_id = $3 AND agent_id = $4 "
            "AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            nodeId, clusterId, tenantId, agentId);
        if (rows.empty()) {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        const auto currentHash = rows.front()[4].value();
        if (!currentHash || !service::utils::tokenHashMatches(secret, *currentHash)) {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        if (rows.front()[3].value().value_or("") == "pending") {
            const auto updated = co_await transaction.execute(
                "UPDATE sys_node SET registration_status = 'registered', registered_at = NOW(), "
                "last_heartbeat_at = NOW(), desired_release_id = (SELECT current_release_id FROM "
                "sys_cluster WHERE tenant_id = sys_node.tenant_id AND id = "
                "sys_node.cluster_id), updated_at = NOW() WHERE id = $1 "
                "AND agent_id = $2 AND registration_status = 'pending' AND deleted_at IS NULL",
                rows.front()[0].value().value_or(""), agentId);
            if (updated.affectedRows() != 1) {
                service::common::throwAppError(AGENT_UNAUTHORIZED);
            }
            const std::string tenantIdValue(tenantId);
            const std::string clusterIdValue(clusterId);
            co_await service::cluster_dns::reconcileCluster(transaction, tenantIdValue,
                                                            clusterIdValue);
        } else if (rows.front()[3].value().value_or("") != "registered") {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        AgentPrincipal result{
            .nodeId = std::string(rows.front()[0].value().value_or("")),
            .clusterId = std::string(rows.front()[1].value().value_or("")),
            .tenantId = std::string(rows.front()[2].value().value_or("")),
            .agentId = agentId,
        };
        co_await transaction.commit();
        co_return result;
    }

    ruvia::Task<bool> isCurrent(ruvia::Context& c, const AgentPrincipal& principal) {
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM sys_node WHERE tenant_id = $1 AND id = $2 AND cluster_id = $3 AND "
            "agent_id = $4 AND registration_status = 'registered' AND deleted_at IS NULL LIMIT 1",
            principal.tenantId, principal.nodeId, principal.clusterId, principal.agentId);
        co_return !rows.empty();
    }

    ruvia::Task<DesiredSummary> desiredSummary(ruvia::Context& c, const AgentPrincipal& principal) {
        const auto rows = co_await c.db().query(
            "SELECT node.node_spec_revision, release.id, release.manifest_digest FROM sys_node "
            "node "
            "INNER JOIN sys_cluster_release release ON release.tenant_id = node.tenant_id AND "
            "release.id = node.desired_release_id WHERE node.tenant_id = $1 AND node.id = $2 AND "
            "node.cluster_id = $3 AND node.registration_status = 'registered' AND "
            "node.agent_id = $4 AND node.deleted_at IS NULL "
            "LIMIT 1",
            principal.tenantId, principal.nodeId, principal.clusterId, principal.agentId);
        if (rows.empty()) {
            service::common::throwAppError(REVISION_INVALID);
        }
        co_return DesiredSummary{
            .nodeSpecRevision = rows.front()[0].as<std::int64_t>().value_or(1),
            .releaseId = std::string(rows.front()[1].value().value_or("")),
            .manifestDigest = std::string(rows.front()[2].value().value_or("")),
        };
    }

    ruvia::Task<flexedge::node::v2::DesiredState> desiredState(ruvia::Context& c,
                                                               const AgentPrincipal& principal) {
        const auto& nodeId = principal.nodeId;
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "SELECT node.node_spec_revision, node.name, node.status = 'enabled', "
            "node.config::text, "
            "node.desired_release_id, release.manifest_digest, release.manifest_envelope FROM "
            "sys_node node INNER JOIN sys_cluster_release release ON release.tenant_id = "
            "node.tenant_id AND release.id = node.desired_release_id WHERE node.tenant_id = $1 AND "
            "node.id = $2 AND node.cluster_id = $3 AND node.agent_id = $4 AND "
            "node.registration_status = 'registered' AND "
            "node.deleted_at IS NULL LIMIT 1 FOR UPDATE OF node",
            principal.tenantId, nodeId, principal.clusterId, principal.agentId);
        if (rows.empty()) {
            service::common::throwAppError(REVISION_INVALID);
        }
        flexedge::node::v2::DesiredState result;
        *result.mutable_node_spec() = service::node_dispatch::buildNodeSpec(
            nodeId, rows.front()[0].as<std::int64_t>().value_or(1),
            rows.front()[1].value().value_or(""), rows.front()[2].as<bool>().value_or(false),
            rows.front()[3].value().value_or("{}"));
        service::utils::SensitiveString manifestBytes(
            service::utils::openSecret(rows.front()[6].value().value_or("")));
        if (!flexedge::node::parseArtifact(manifestBytes.view(), *result.mutable_release()) ||
            result.release().digest_sha256() != rows.front()[5].value().value_or("") ||
            flexedge::node::artifactDigest(result.release().content()) !=
                result.release().digest_sha256() ||
            result.release().content().release_id() != rows.front()[4].value().value_or("")) {
            service::common::throwAppError(ARTIFACT_INVALID);
        }
        (void)co_await transaction.execute(
            "UPDATE sys_node SET node_spec_digest = $3, updated_at = NOW() WHERE tenant_id = $1 "
            "AND id = $2 AND node_spec_revision = $4",
            principal.tenantId, nodeId, result.node_spec().digest_sha256(),
            result.node_spec().content().revision());
        co_await transaction.commit();
        co_return result;
    }

    ruvia::Task<flexedge::node::v2::ObjectBatch>
    objects(ruvia::Context& c, const AgentPrincipal& principal, std::string_view releaseId,
            const google::protobuf::RepeatedPtrField<std::string>& requested) {
        std::unordered_set<std::string> missing;
        missing.reserve(static_cast<std::size_t>(requested.size()));
        for (const auto& digest : requested) {
            if (!missing.emplace(digest).second) {
                service::common::throwAppError(ARTIFACT_INVALID);
            }
        }
        const auto rows = co_await c.db().query(
            "SELECT mapping.object_digest, object.kind, object.payload_envelope FROM "
            "sys_cluster_release_object mapping INNER JOIN sys_cluster_release release ON "
            "release.tenant_id = mapping.tenant_id AND release.id = mapping.release_id INNER JOIN "
            "sys_delivery_object object ON object.tenant_id = mapping.tenant_id AND "
            "object.digest_sha256 = mapping.object_digest INNER JOIN sys_node node ON "
            "node.tenant_id "
            "= mapping.tenant_id AND node.desired_release_id = mapping.release_id WHERE "
            "mapping.tenant_id = $1 AND mapping.release_id = $2 AND release.cluster_id = $3 AND "
            "node.id = $4 AND node.agent_id = $5 AND "
            "node.registration_status = 'registered' AND node.deleted_at IS NULL ORDER BY "
            "mapping.position ASC",
            principal.tenantId, releaseId, principal.clusterId, principal.nodeId,
            principal.agentId);
        flexedge::node::v2::ObjectBatch result;
        result.set_release_id(releaseId);
        for (const auto& row : rows) {
            const auto digest = std::string(row[0].value().value_or(""));
            if (!missing.erase(digest)) {
                continue;
            }
            service::utils::SensitiveString plaintext(
                service::utils::openSecret(row[2].value().value_or("")));
            auto* object = result.add_objects();
            object->set_digest_sha256(digest);
            if (!flexedge::node::parseArtifact(plaintext.view(), *object->mutable_content()) ||
                flexedge::node::artifactDigest(object->content()) != digest ||
                (row[1].value().value_or("") == "website" && !object->content().has_website()) ||
                (row[1].value().value_or("") == "certificate" &&
                 !object->content().has_certificate())) {
                service::common::throwAppError(ARTIFACT_INVALID);
            }
        }
        if (!missing.empty()) {
            service::common::throwAppError(ARTIFACT_INVALID);
        }
        co_return result;
    }

    ruvia::Task<void> heartbeat(ruvia::Context& c, const AgentPrincipal& principal,
                                const HeartbeatReport& report) {
        if (report.nodeId != principal.nodeId) {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        service::node_runtime::NodeRuntimeOutput runtime(c);
        runtime.set<"agentVersion">(report.agentVersion);
        runtime.set<"cpuUsage">(report.cpuUsage);
        runtime.set<"memoryUsage">(report.memoryUsage);
        runtime.set<"trafficOutBps">(report.trafficOutBps);
        runtime.set<"connectionCount">(report.connectionCount);
        runtime.set<"load1m">(report.load1m);
        runtime.set<"queuedLogEvents">(report.queuedLogEvents);
        runtime.set<"droppedLogEvents">(report.droppedLogEvents);
        runtime.set<"health">(report.health);
        runtime.set<"lastError">(report.lastError);
        auto& originHealth = runtime.ensure<"originHealth">();
        originHealth.reserve(report.originHealth.size());
        for (const auto& reportItem : report.originHealth) {
            auto& item = originHealth.emplace_back(c);
            item.set<"websiteId">(reportItem.websiteId);
            item.set<"originId">(reportItem.originId);
            item.set<"status">(reportItem.status);
            item.set<"checkedAtUnixMillis">(reportItem.checkedAtUnixMillis);
            item.set<"latencyMillis">(reportItem.latencyMillis);
            if (!reportItem.lastError.empty()) {
                item.set<"lastError">(reportItem.lastError);
            }
        }
        const auto runtimeJson = ruvia::toJson(runtime, {.resource = c.resource()});
        auto transaction = co_await c.db().beginTransaction();
        const auto updated = co_await transaction.query(
            "UPDATE sys_node node SET last_heartbeat_at = NOW(), applied_node_spec_revision = $2, "
            "active_release_id = $3, active_manifest_digest = $4, runtime = $5::jsonb, updated_at "
            "= "
            "NOW() WHERE node.id = $1 AND node.tenant_id = $6 AND node.node_spec_revision >= $2 "
            "AND node.applied_node_spec_revision <= $2 AND node.registration_status = 'registered' "
            "AND node.agent_id = $7 AND node.deleted_at IS NULL AND "
            "EXISTS (SELECT 1 FROM sys_cluster_release release WHERE release.tenant_id = "
            "node.tenant_id AND release.id = $3 AND release.cluster_id = node.cluster_id AND "
            "release.manifest_digest = $4) RETURNING node.id",
            report.nodeId, report.appliedNodeSpecRevision, report.activeReleaseId,
            report.activeManifestDigest, std::string_view(runtimeJson), principal.tenantId,
            principal.agentId);
        if (updated.empty()) {
            service::common::throwAppError(REVISION_INVALID);
        }
        (void)co_await transaction.execute(
            "UPDATE sys_node_release_target SET status = 'applied', failed_phase = NULL, "
            "error_code = NULL, last_error = NULL, retryable = NULL, applied_at = NOW(), "
            "updated_at = NOW() WHERE tenant_id = $1 AND release_id = $2 AND node_id = $3 AND "
            "status IN ('pending', 'failed')",
            principal.tenantId, report.activeReleaseId, report.nodeId);
        co_await transaction.commit();
        co_return;
    }

    ruvia::Task<void> recordApplyResult(ruvia::Context& c, const AgentPrincipal& principal,
                                        const flexedge::node::v2::ApplyResult& result) {
        const auto& nodeId = principal.nodeId;
        if (result.applied()) {
            auto transaction = co_await c.db().beginTransaction();
            const auto updated = co_await transaction.execute(
                "UPDATE sys_node node SET applied_node_spec_revision = $2, active_release_id = $3, "
                "active_manifest_digest = $4, last_apply_phase = NULL, last_apply_error_code = "
                "NULL, last_apply_error = NULL, last_apply_retryable = NULL, updated_at = NOW() "
                "WHERE node.tenant_id = $1 AND "
                "node.id = $5 AND node.agent_id = $6 AND "
                "node.registration_status = 'registered' AND "
                "node.applied_node_spec_revision <= $2 AND node.node_spec_revision >= $2 AND "
                "node.desired_release_id = $3 AND node.node_spec_revision = $2 AND "
                "EXISTS (SELECT 1 FROM sys_cluster_release release WHERE release.tenant_id = "
                "node.tenant_id AND release.id = $3 AND release.cluster_id = node.cluster_id AND "
                "release.manifest_digest = $4)",
                principal.tenantId, result.node_spec_revision(), result.release_id(),
                result.manifest_digest(), nodeId, principal.agentId);
            if (updated.affectedRows() != 1) {
                service::common::throwAppError(REVISION_INVALID);
            }
            (void)co_await transaction.execute(
                "UPDATE sys_node_release_target SET status = 'applied', failed_phase = NULL, "
                "error_code = NULL, last_error = NULL, retryable = NULL, applied_at = NOW(), "
                "updated_at = NOW() WHERE tenant_id = $1 AND release_id = $2 AND node_id = $3 "
                "AND status IN ('pending', 'failed')",
                principal.tenantId, result.release_id(), nodeId);
            co_await transaction.commit();
            co_return;
        }
        const auto phase = flexedge::node::v2::ApplyPhase_Name(result.failed_phase());
        auto transaction = co_await c.db().beginTransaction();
        const auto nodeUpdated = co_await transaction.query(
            "UPDATE sys_node node SET last_apply_phase = CASE WHEN "
            "node.applied_node_spec_revision < $8 THEN $4 ELSE node.last_apply_phase END, "
            "last_apply_error_code = CASE WHEN node.applied_node_spec_revision < $8 THEN $5 "
            "ELSE node.last_apply_error_code END, last_apply_error = CASE WHEN "
            "node.applied_node_spec_revision < $8 THEN $6 ELSE node.last_apply_error END, "
            "last_apply_retryable = CASE WHEN node.applied_node_spec_revision < $8 THEN $7 ELSE "
            "node.last_apply_retryable END, updated_at = CASE WHEN "
            "node.applied_node_spec_revision < $8 THEN NOW() ELSE node.updated_at END WHERE "
            "node.tenant_id = $1 AND node.id = $3 AND node.desired_release_id = $2 AND "
            "node.node_spec_revision = $8 AND node.applied_node_spec_revision <= $8 AND "
            "node.agent_id = $9 AND "
            "node.registration_status = 'registered' "
            "AND EXISTS (SELECT 1 FROM sys_cluster_release release WHERE release.tenant_id = "
            "node.tenant_id AND release.id = $2 AND release.cluster_id = node.cluster_id AND "
            "release.manifest_digest = $10) RETURNING node.applied_node_spec_revision",
            principal.tenantId, result.release_id(), nodeId, phase, result.error_code(),
            result.error().substr(0, 1000), result.retryable(), result.node_spec_revision(),
            principal.agentId, result.manifest_digest());
        if (nodeUpdated.empty()) {
            service::common::throwAppError(REVISION_INVALID);
        }
        if (nodeUpdated.front()[0].as<std::int64_t>().value_or(0) < result.node_spec_revision()) {
            (void)co_await transaction.execute(
                "UPDATE sys_node_release_target target SET status = 'failed', failed_phase = $4, "
                "error_code = $5, last_error = $6, retryable = $7, updated_at = NOW() FROM "
                "sys_cluster_release release WHERE target.tenant_id = $1 AND target.release_id = "
                "$2 "
                "AND target.node_id = $3 AND target.status IN ('pending', 'failed') AND "
                "release.tenant_id = target.tenant_id AND release.id = target.release_id AND "
                "release.cluster_id = $8",
                principal.tenantId, result.release_id(), nodeId, phase, result.error_code(),
                result.error().substr(0, 1000), result.retryable(), principal.clusterId);
        }
        co_await transaction.commit();
        co_return;
    }
};

inline AgentService& agentService() {
    static AgentService service;
    return service;
}

} // namespace service::agent
