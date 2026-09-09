#pragma once

#include "service/features/node_dispatch/notifications.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent.error.h"
#include "service/domains/agent/agent_runtime.mapper.h"
#include "service/domains/agent/agent.types.h"
#include "service/domains/agent/heartbeat.store.h"
#include "service/domains/agent/desired_state.store.h"
#include "service/domains/agent/release_manifest.h"
#include "service/domains/agent/apply_result.store.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_dispatch/target.store.h"
#include "service/features/live_resource/node_runtime.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::agent {

class AgentCommandService final {
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
            // The target set is created from registered nodes. Publish only after this node has
            // transitioned out of pending so the initial release includes it as a target.
            co_await service::node_dispatch::publishClusterRelease(transaction, tenantId,
                                                                   clusterId);
            const std::string tenantIdValue(tenantId);
            const std::string clusterIdValue(clusterId);
            co_await service::cluster_dns::reconcileCluster(transaction, tenantIdValue,
                                                            clusterIdValue);
        } else if (rows.front()[3].value().value_or("") == "registered") {
            co_await service::node_dispatch::ensureClusterRelease(transaction, tenantId, clusterId);
        } else {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        AgentPrincipal result{
            .nodeId = std::string(rows.front()[0].value().value_or("")),
            .clusterId = std::string(rows.front()[1].value().value_or("")),
            .tenantId = std::string(rows.front()[2].value().value_or("")),
            .agentId = agentId,
        };
        co_await transaction.commit();
        service::node_dispatch::notifications::published(result.tenantId);
        service::live_resource::hub().publish(result.tenantId, service::live_resource::Resource::nodes, result.nodeId);
        service::live_resource::hub().publish(result.tenantId, service::live_resource::Resource::clusters, result.clusterId);
        service::live_resource::hub().publish(result.tenantId, service::live_resource::Resource::tasks);
        service::live_resource::nodeDeadlines().touch(result.tenantId, result.nodeId);
        co_return result;
    }

    ruvia::Task<flexedge::node::v2::DesiredState> desiredState(ruvia::Context& c,
                                                               const AgentPrincipal& principal) {
        const auto& nodeId = principal.nodeId;
        auto transaction = co_await c.db().beginTransaction();
        const auto state = co_await lockDesiredState(transaction, principal);
        if (!state) service::common::throwAppError(REVISION_INVALID);
        flexedge::node::v2::DesiredState result;
        *result.mutable_node_spec() = service::node_dispatch::buildNodeSpec(
            nodeId, state->revision, state->name, state->enabled, state->configJson);
        service::utils::SensitiveString manifestBytes(service::utils::openSecret(state->manifestEnvelope));
        auto manifest = parseReleaseManifest(manifestBytes.view(), state->manifestDigest,
            state->releaseId, principal.clusterId);
        if (!manifest) service::common::throwAppError(ARTIFACT_INVALID);
        *result.mutable_release() = std::move(*manifest);
        if (!(co_await recordNodeSpecDigest(transaction, principal.tenantId, nodeId,
            state->revision, result.node_spec().digest_sha256()))) {
            service::common::throwAppError(REVISION_INVALID);
        }
        co_await transaction.commit();
        co_return result;
    }

    ruvia::Task<void> heartbeat(ruvia::Context& c, const AgentPrincipal& principal,
                                const HeartbeatReport& report) {
        if (report.nodeId != principal.nodeId) {
            service::common::throwAppError(AGENT_UNAUTHORIZED);
        }
        const auto runtimeJson = heartbeatRuntimeJson(c, report);
        auto transaction = co_await c.db().beginTransaction();
        const auto update = co_await updateHeartbeat(transaction, principal, report, runtimeJson);
        if (!update) {
            service::common::throwAppError(REVISION_INVALID);
        }
        const auto targetChanged = co_await service::node_dispatch::markNodeTargetApplied(transaction, principal.tenantId, report.activeReleaseId, report.nodeId);
        co_await transaction.commit();
        service::live_resource::nodeDeadlines().touch(principal.tenantId, principal.nodeId);
        service::live_resource::hub().publishRuntime(principal.tenantId, principal.nodeId,
            service::live_resource::runtimePatch(c, report, *update));
        service::live_resource::publishOriginRuntime(c, principal.tenantId, report, *update);
        if (update->becameOnline) {
            service::live_resource::hub().publish(principal.tenantId, service::live_resource::Resource::nodes, principal.nodeId);
            service::live_resource::hub().publish(principal.tenantId, service::live_resource::Resource::overview);
            service::live_resource::hub().publish(principal.tenantId, service::live_resource::Resource::clusters, principal.clusterId);
        }
        if (targetChanged) {
            service::live_resource::hub().publish(principal.tenantId, service::live_resource::Resource::tasks);
            service::live_resource::hub().publish(principal.tenantId, service::live_resource::Resource::websites);
            service::live_resource::hub().publish(principal.tenantId, service::live_resource::Resource::clusters, principal.clusterId);
        }
        co_return;
    }

    ruvia::Task<void> recordApplyResult(ruvia::Context& c, const AgentPrincipal& principal,
                                        const flexedge::node::v2::ApplyResult& result) {
        const auto& nodeId = principal.nodeId;
        if (result.applied()) {
            auto transaction = co_await c.db().beginTransaction();
            if (!(co_await recordAppliedDeployment(transaction, principal, result.node_spec_revision(),
                result.release_id(), result.manifest_digest()))) {
                service::common::throwAppError(REVISION_INVALID);
            }
            co_await service::node_dispatch::markNodeTargetApplied(transaction, principal.tenantId, result.release_id(), nodeId);
            co_await transaction.commit();
            publishApplyChange(principal);
            co_return;
        }
        const auto phase = flexedge::node::v2::ApplyPhase_Name(result.failed_phase());
        auto transaction = co_await c.db().beginTransaction();
        const ApplyFailureReport failure{.revision = result.node_spec_revision(),
            .releaseId = result.release_id(), .manifestDigest = result.manifest_digest(),
            .phase = phase, .errorCode = result.error_code(), .error = result.error().substr(0, 1000),
            .retryable = result.retryable()};
        const auto outcome = co_await recordFailedDeployment(transaction, principal, failure);
        if (outcome == ApplyFailureOutcome::rejected) {
            service::common::throwAppError(REVISION_INVALID);
        }
        if (outcome == ApplyFailureOutcome::recorded) {
            co_await service::node_dispatch::markNodeTargetFailed(transaction,
                principal.tenantId, failure.releaseId, nodeId, principal.clusterId,
                {.phase = failure.phase, .errorCode = failure.errorCode,
                 .error = failure.error, .retryable = failure.retryable});
        }
        co_await transaction.commit();
        publishApplyChange(principal);
        co_return;
    }
  private:
    static void publishApplyChange(const AgentPrincipal& principal) {
        using service::live_resource::Resource;
        auto& hub = service::live_resource::hub();
        hub.publish(principal.tenantId, Resource::nodes, principal.nodeId);
        hub.publish(principal.tenantId, Resource::clusters, principal.clusterId);
        hub.publish(principal.tenantId, Resource::websites);
        hub.publish(principal.tenantId, Resource::tasks);
    }
};

inline AgentCommandService& agentCommandService() {
    static AgentCommandService service;
    return service;
}

} // namespace service::agent
