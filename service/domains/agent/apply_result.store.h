#pragma once
#include <cstdint>
#include <string_view>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>
#include "service/domains/agent/agent.types.h"
namespace service::agent {
inline ruvia::Task<bool> recordAppliedDeployment(ruvia::DbTransaction& transaction,
    const AgentPrincipal& principal, std::int64_t revision, std::string_view releaseId,
    std::string_view manifestDigest) {
            const auto updated = co_await transaction.execute(
                "UPDATE sys_node node SET applied_node_spec_revision = $2, active_release_id = $3, "
                "active_manifest_digest = $4, last_apply_phase = NULL, last_apply_error_code = "
                "NULL, last_apply_error = NULL, last_apply_retryable = NULL, updated_at = NOW() "
                "WHERE node.tenant_id = $1 AND "
                "node.id = $5 AND node.agent_id = $6 AND "
                "node.registration_status = 'registered' AND node.deleted_at IS NULL AND "
                "node.applied_node_spec_revision <= $2 AND node.node_spec_revision >= $2 AND "
                "node.desired_release_id = $3 AND node.node_spec_revision = $2 AND "
                "EXISTS (SELECT 1 FROM sys_cluster_release release WHERE release.tenant_id = "
                "node.tenant_id AND release.id = $3 AND release.cluster_id = node.cluster_id AND "
                "release.manifest_digest = $4)",
                principal.tenantId, revision, releaseId,
                manifestDigest, principal.nodeId, principal.agentId);
    co_return updated.affectedRows() == 1;
}
enum class ApplyFailureOutcome { rejected, alreadyApplied, recorded };

inline ruvia::Task<ApplyFailureOutcome> recordFailedDeployment(ruvia::DbTransaction& transaction,
    const AgentPrincipal& principal, const ApplyFailureReport& failure) {
        const auto nodeUpdated = co_await transaction.query(
            "UPDATE sys_node node SET last_apply_phase = CASE WHEN "
            "(node.applied_node_spec_revision < $8 OR node.active_release_id IS DISTINCT FROM $2 "
            "OR node.active_manifest_digest IS DISTINCT FROM $10) THEN $4 ELSE "
            "node.last_apply_phase END, "
            "last_apply_error_code = CASE WHEN (node.applied_node_spec_revision < $8 OR "
            "node.active_release_id IS DISTINCT FROM $2 OR node.active_manifest_digest IS DISTINCT "
            "FROM $10) THEN $5 "
            "ELSE node.last_apply_error_code END, last_apply_error = CASE WHEN "
            "(node.applied_node_spec_revision < $8 OR node.active_release_id IS DISTINCT FROM $2 "
            "OR node.active_manifest_digest IS DISTINCT FROM $10) THEN $6 ELSE "
            "node.last_apply_error END, "
            "last_apply_retryable = CASE WHEN (node.applied_node_spec_revision < $8 OR "
            "node.active_release_id IS DISTINCT FROM $2 OR node.active_manifest_digest IS DISTINCT "
            "FROM $10) THEN $7 ELSE "
            "node.last_apply_retryable END, updated_at = CASE WHEN "
            "(node.applied_node_spec_revision < $8 OR node.active_release_id IS DISTINCT FROM $2 "
            "OR node.active_manifest_digest IS DISTINCT FROM $10) THEN NOW() ELSE node.updated_at "
            "END WHERE "
            "node.tenant_id = $1 AND node.id = $3 AND node.desired_release_id = $2 AND "
            "node.node_spec_revision = $8 AND node.applied_node_spec_revision <= $8 AND "
            "node.agent_id = $9 AND "
            "node.registration_status = 'registered' AND node.deleted_at IS NULL "
            "AND EXISTS (SELECT 1 FROM sys_cluster_release release WHERE release.tenant_id = "
            "node.tenant_id AND release.id = $2 AND release.cluster_id = node.cluster_id AND "
            "release.manifest_digest = $10) RETURNING (node.applied_node_spec_revision < $8 OR "
            "node.active_release_id IS DISTINCT FROM $2 OR node.active_manifest_digest IS DISTINCT "
            "FROM $10)",
            principal.tenantId, failure.releaseId, principal.nodeId, failure.phase, failure.errorCode,
            failure.error, failure.retryable, failure.revision,
            principal.agentId, failure.manifestDigest);
    if (nodeUpdated.empty()) co_return ApplyFailureOutcome::rejected;
    co_return nodeUpdated.front()[0].as<bool>().value()
        ? ApplyFailureOutcome::recorded : ApplyFailureOutcome::alreadyApplied;
}
} // namespace service::agent
