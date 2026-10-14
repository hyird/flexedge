#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <string>
#include <string_view>

namespace service::node_dispatch {

inline ruvia::task<void> exclude_pending_node_targets(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& node_id) {
    (void)co_await transaction.execute(
        "UPDATE sys_node_release_target SET status = 'excluded', updated_at = NOW() "
        "WHERE tenant_id = $1 AND node_id = $2 AND status IN ('pending', 'failed')",
        tenant_id, node_id);
}

inline ruvia::task<void> exclude_pending_node_cluster_targets(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& node_id, const std::string& cluster_id) {
    (void)co_await transaction.execute(
        "UPDATE sys_node_release_target target SET status = 'excluded', updated_at = "
        "NOW() FROM sys_cluster_release release WHERE target.tenant_id = $1 AND "
        "target.node_id = $2 AND target.status IN ('pending', 'failed') AND "
        "release.tenant_id = target.tenant_id AND release.id = target.release_id AND "
        "release.cluster_id = $3",
        tenant_id, node_id, cluster_id);
}

inline ruvia::task<bool> mark_node_target_applied(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view release_id, std::string_view node_id) {
    const auto updated = co_await transaction.execute(
        "UPDATE sys_node_release_target SET status = 'applied', failed_phase = NULL, "
        "error_code = NULL, last_error = NULL, retryable = NULL, applied_at = NOW(), "
        "updated_at = NOW() WHERE tenant_id = $1 AND release_id = $2 AND node_id = $3 "
        "AND status IN ('pending', 'failed')",
        tenant_id, release_id, node_id);
    co_return updated.affected_rows() != 0;
}

struct target_failure {
    std::string phase_;
    std::string error_code_;
    std::string error_;
    bool retryable_{};
};

inline ruvia::task<bool> mark_node_target_failed(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view release_id, std::string_view node_id,
    std::string_view cluster_id, const target_failure& failure) {
    const auto updated = co_await transaction.execute(
        "UPDATE sys_node_release_target target SET status = 'failed', failed_phase = $4, "
        "error_code = $5, last_error = $6, retryable = $7, updated_at = NOW() FROM "
        "sys_cluster_release release WHERE target.tenant_id = $1 AND target.release_id = $2 "
        "AND target.node_id = $3 AND target.status IN ('pending', 'failed') AND "
        "release.tenant_id = target.tenant_id AND release.id = target.release_id AND "
        "release.cluster_id = $8",
        tenant_id, release_id, node_id, failure.phase_, failure.error_code_, failure.error_,
        failure.retryable_, cluster_id);
    co_return updated.affected_rows() != 0;
}
}  // namespace service::node_dispatch
