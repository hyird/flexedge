#pragma once

#include <string>
#include <string_view>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

namespace service::node_dispatch {

inline ruvia::Task<void> excludePendingNodeTargets(ruvia::DbTransaction& transaction,
    const std::string& tenantId, const std::string& nodeId) {
    (void)co_await transaction.execute(
        "UPDATE sys_node_release_target SET status = 'excluded', updated_at = NOW() "
        "WHERE tenant_id = $1 AND node_id = $2 AND status IN ('pending', 'failed')",
        tenantId, nodeId);
}

inline ruvia::Task<void> excludePendingNodeClusterTargets(ruvia::DbTransaction& transaction,
    const std::string& tenantId, const std::string& nodeId, const std::string& clusterId) {
    (void)co_await transaction.execute(
        "UPDATE sys_node_release_target target SET status = 'excluded', updated_at = "
        "NOW() FROM sys_cluster_release release WHERE target.tenant_id = $1 AND "
        "target.node_id = $2 AND target.status IN ('pending', 'failed') AND "
        "release.tenant_id = target.tenant_id AND release.id = target.release_id AND "
        "release.cluster_id = $3",
        tenantId, nodeId, clusterId);
}

inline ruvia::Task<bool> markNodeTargetApplied(ruvia::DbTransaction& transaction,
    std::string_view tenantId, std::string_view releaseId, std::string_view nodeId) {
    const auto updated = co_await transaction.execute(
        "UPDATE sys_node_release_target SET status = 'applied', failed_phase = NULL, "
        "error_code = NULL, last_error = NULL, retryable = NULL, applied_at = NOW(), "
        "updated_at = NOW() WHERE tenant_id = $1 AND release_id = $2 AND node_id = $3 "
        "AND status IN ('pending', 'failed')",
        tenantId, releaseId, nodeId);
    co_return updated.affectedRows() != 0;
}

struct TargetFailure {
    std::string phase;
    std::string errorCode;
    std::string error;
    bool retryable{};
};

inline ruvia::Task<void> markNodeTargetFailed(ruvia::DbTransaction& transaction,
    std::string_view tenantId, std::string_view releaseId, std::string_view nodeId,
    std::string_view clusterId, const TargetFailure& failure) {
    (void)co_await transaction.execute(
        "UPDATE sys_node_release_target target SET status = 'failed', failed_phase = $4, "
        "error_code = $5, last_error = $6, retryable = $7, updated_at = NOW() FROM "
        "sys_cluster_release release WHERE target.tenant_id = $1 AND target.release_id = $2 "
        "AND target.node_id = $3 AND target.status IN ('pending', 'failed') AND "
        "release.tenant_id = target.tenant_id AND release.id = target.release_id AND "
        "release.cluster_id = $8",
        tenantId, releaseId, nodeId, failure.phase, failure.errorCode, failure.error,
        failure.retryable, clusterId);
}
} // namespace service::node_dispatch
