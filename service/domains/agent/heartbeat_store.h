#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <optional>
#include <string>
#include <string_view>

#include "service/domains/agent/agent_types.h"

namespace service::agent {
struct heartbeat_update final {
    std::string at_;
    std::string status_;
    bool became_online_{};
    std::string node_name_;
    std::string previous_runtime_;
    std::int64_t node_revision_{};
};
inline ruvia::task<std::optional<heartbeat_update>> update_heartbeat(ruvia::db_transaction& transaction,
    const agent_principal& principal, const heartbeat_report& report, std::string_view runtime_json) {
    const auto updated = co_await transaction.query(
        "WITH previous AS MATERIALIZED (SELECT id, runtime, last_heartbeat_at >= NOW() - INTERVAL '90 seconds' AS online "
        "FROM sys_node WHERE id = $1 AND tenant_id = $6 FOR UPDATE) "
        "UPDATE sys_node node SET last_heartbeat_at = NOW(), applied_node_spec_revision = $2, "
        "active_release_id = $3, active_manifest_digest = $4, runtime = $5::jsonb, updated_at "
        "= "
        "NOW() FROM previous WHERE node.id = previous.id AND node.id = $1 AND node.tenant_id = $6 AND node.node_spec_revision >= $2 "
        "AND node.applied_node_spec_revision <= $2 AND node.registration_status = 'registered' "
        "AND node.agent_id = $7 AND node.deleted_at IS NULL AND "
        "EXISTS (SELECT 1 FROM sys_cluster_release release WHERE release.tenant_id = "
        "node.tenant_id AND release.id = $3 AND release.cluster_id = node.cluster_id AND "
        "release.manifest_digest = $4) RETURNING TO_CHAR(node.last_heartbeat_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
        "node.status, NOT COALESCE(previous.online, FALSE), node.name, previous.runtime::text, node.revision",
        principal.node_id_, report.applied_node_spec_revision_, report.active_release_id_,
        report.active_manifest_digest_, runtime_json, principal.tenant_id_, principal.agent_id_);
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
}  // namespace service::agent
