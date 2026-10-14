#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <string_view>

#include "service/domains/agent/agent_types.h"
namespace service::agent {
inline ruvia::task<bool> record_applied_deployment(ruvia::db_transaction& transaction,
    const agent_principal& principal, std::int64_t revision, std::string_view release_id,
    std::string_view manifest_digest) {
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
        principal.tenant_id_, revision, release_id,
        manifest_digest, principal.node_id_, principal.agent_id_);
    co_return updated.affected_rows() == 1;
}
enum class apply_failure_outcome { rejected,
    already_applied,
    recorded };

inline ruvia::task<apply_failure_outcome> record_failed_deployment(ruvia::db_transaction& transaction,
    const agent_principal& principal, const apply_failure_report& failure) {
    const auto node_updated = co_await transaction.query(
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
        principal.tenant_id_, failure.release_id_, principal.node_id_, failure.phase_, failure.error_code_,
        failure.error_, failure.retryable_, failure.revision_,
        principal.agent_id_, failure.manifest_digest_);
    if (node_updated.empty()) {
        co_return apply_failure_outcome::rejected;
    }
    co_return node_updated.front()[0].as<bool>().value()
        ? apply_failure_outcome::recorded
        : apply_failure_outcome::already_applied;
}
}  // namespace service::agent
