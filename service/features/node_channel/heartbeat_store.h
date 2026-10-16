#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "service/features/node_presence/node_connection.h"
#include "service/features/node_channel/types.h"

namespace service::node_channel {
// The node spec revision the node had applied before this report, read
// under the node row lock its update takes anyway; nothing when the node
// is gone or belongs to another agent.
inline ruvia::task<std::optional<std::int64_t>> lock_applied_node_spec_revision(
    ruvia::db_transaction& transaction, const agent_principal& principal) {
    const auto rows = co_await transaction.query(
        "SELECT applied_node_spec_revision FROM sys_node WHERE tenant_id = $1 AND id = $2 AND agent_id = $3 "
        "AND deleted_at IS NULL FOR UPDATE",
        principal.tenant_id_, principal.node_id_, principal.agent_id_);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return rows.front()[0].as<std::int64_t>().value_or(0);
}
struct heartbeat_update final {
    std::string at_;
    std::string status_;
    bool became_online_{};
    std::string node_name_;
    std::string previous_runtime_;
    std::int64_t node_revision_{};
};
inline ruvia::task<std::optional<heartbeat_update>> update_heartbeat(ruvia::db_transaction& transaction,
    const agent_principal& principal, const heartbeat_report& report, std::string_view runtime_json,
    std::int64_t received_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count()) {
    // `online` is the heartbeat window alone: a disabled node that keeps reporting must not
    // count as coming online on every heartbeat.
    const auto updated = co_await transaction.query(
        "WITH previous AS MATERIALIZED (SELECT node.id, node.runtime, "
        "node.last_heartbeat_at >= NOW() - make_interval(secs => $9) "
        "AS online FROM sys_node node WHERE node.id = $1 AND node.tenant_id = $6 FOR UPDATE) "
        "UPDATE sys_node node SET last_heartbeat_at = to_timestamp($8::double precision/1000.0), applied_node_spec_revision = $2, "
        "active_release_id = $3, active_manifest_digest = $4, runtime = $5::jsonb, updated_at "
        "= "
        "NOW() FROM previous WHERE node.id = previous.id AND node.id = $1 AND node.tenant_id = $6 AND node.node_spec_revision >= $2 "
        "AND node.applied_node_spec_revision <= $2 AND node.registration_status = 'registered' "
        "AND (node.last_heartbeat_at IS NULL OR node.last_heartbeat_at < to_timestamp($8::double precision/1000.0)) "
        "AND node.agent_id = $7 AND node.deleted_at IS NULL AND "
        "EXISTS (SELECT 1 FROM sys_cluster_release release WHERE release.tenant_id = "
        "node.tenant_id AND release.id = $3 AND release.cluster_id = node.cluster_id AND "
        "release.manifest_digest = $4) RETURNING TO_CHAR(node.last_heartbeat_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
        "node.status, NOT COALESCE(previous.online, FALSE), node.name, previous.runtime::text, node.revision",
        principal.node_id_, report.applied_node_spec_revision_, report.active_release_id_,
        report.active_manifest_digest_, runtime_json, principal.tenant_id_, principal.agent_id_, received_unix_ms,
        service::node::online_window_seconds);
    if (updated.empty()) {
        co_return std::nullopt;
    }
    co_return heartbeat_update{std::string(updated.front()[0].value().value_or("")),
        std::string(updated.front()[1].value().value_or("disabled")),
        updated.front()[2].as<bool>().value_or(false),
        std::string(updated.front()[3].value().value_or("")),
        std::string(updated.front()[4].value().value_or("{}")),
        updated.front()[5].as<std::int64_t>().value_or(0)};
}
}  // namespace service::node_channel
