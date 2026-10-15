#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "service/features/operation/work_origin.h"

namespace service::node_dispatch {

namespace detail {

// The subtasks of excluded targets are cancelled; the excluding task is
// named when the write scheduled work of its own (cancelling alone starts
// no task).
template <typename rows_type>
ruvia::task<void> cancel_excluded(ruvia::db_transaction& transaction, std::string_view tenant_id,
    const rows_type& rows, service::operation::work_origin& origin) {
    for (const auto& row : rows) {
        const auto subtask = row[0].value().value_or("");
        if (subtask.empty()) {
            continue;
        }
        const auto settled = co_await service::operation::settle_subtask(transaction, tenant_id, subtask,
            service::operation::state_cancelled, {}, origin.operation_id().value_or(""));
        origin.touch(settled.value_or(""));
    }
}

}  // namespace detail

inline ruvia::task<void> exclude_pending_node_targets(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& node_id, service::operation::work_origin& origin) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_node_release_target SET status = 'excluded', updated_at = NOW() "
        "WHERE tenant_id = $1 AND node_id = $2 AND status IN ('pending', 'failed') RETURNING "
        "COALESCE(subtask_id::text, '')",
        tenant_id, node_id);
    co_await detail::cancel_excluded(transaction, tenant_id, rows, origin);
}

inline ruvia::task<void> exclude_pending_node_cluster_targets(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& node_id, const std::string& cluster_id,
    service::operation::work_origin& origin) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_node_release_target target SET status = 'excluded', updated_at = "
        "NOW() FROM sys_cluster_release release WHERE target.tenant_id = $1 AND "
        "target.node_id = $2 AND target.status IN ('pending', 'failed') AND "
        "release.tenant_id = target.tenant_id AND release.id = target.release_id AND "
        "release.cluster_id = $3 RETURNING COALESCE(target.subtask_id::text, '')",
        tenant_id, node_id, cluster_id);
    co_await detail::cancel_excluded(transaction, tenant_id, rows, origin);
}

// A node configuration change (node spec revision) is delivered to the node
// as its own subtask; earlier open configuration subtasks of the node merge
// into it. Only registered nodes receive configuration.
inline ruvia::task<void> track_node_config(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view node_id, std::int64_t spec_revision, service::operation::work_origin& origin) {
    const auto registered = co_await transaction.query(
        "SELECT 1 FROM sys_node WHERE tenant_id = $1 AND id = $2 AND registration_status = 'registered' AND "
        "deleted_at IS NULL",
        tenant_id, node_id);
    if (registered.empty()) {
        co_return;
    }
    const auto subtask = co_await origin.require_subtask(transaction, tenant_id,
        service::operation::subtask_kind::node_config, "node", node_id, spec_revision);
    const auto previous = co_await transaction.query(
        "SELECT id::text FROM sys_operation_subtask WHERE tenant_id = $1 AND resource_id = $2::uuid AND kind = "
        "'node_config' AND state = 'open' AND version < $3",
        tenant_id, node_id, spec_revision);
    const auto closed_by = origin.operation_id().value_or("");
    for (const auto& row : previous) {
        const auto settled = co_await service::operation::settle_subtask(transaction, tenant_id,
            row[0].value().value_or(""), service::operation::state_merged, subtask.value_or(""), closed_by);
        origin.touch(settled.value_or(""));
    }
}

// A deleted node or one with reset credentials never applies pending
// configuration: its open configuration subtasks are cancelled.
inline ruvia::task<void> cancel_node_config(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view node_id, service::operation::work_origin& origin) {
    const auto open = co_await transaction.query(
        "SELECT id::text FROM sys_operation_subtask WHERE tenant_id = $1 AND resource_id = $2::uuid AND kind = "
        "'node_config' AND state = 'open'",
        tenant_id, node_id);
    co_await detail::cancel_excluded(transaction, tenant_id, open, origin);
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
