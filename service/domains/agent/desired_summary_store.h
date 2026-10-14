#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db.h>

#include <concepts>
#include <optional>

#include "service/domains/agent/agent_types.h"

namespace service::agent {

// Accept either a request database handle or the caller's existing transaction.
template <typename database_type>
    requires(std::same_as<database_type, ruvia::db_handle> || std::same_as<database_type, ruvia::db_transaction>)
inline ruvia::task<std::optional<desired_summary>> find_desired_summary(
    database_type& db, const agent_principal& principal) {
    const auto rows = co_await db.query(
        "SELECT node.node_spec_revision, release.id, release.manifest_digest FROM sys_node "
        "node "
        "INNER JOIN sys_cluster_release release ON release.tenant_id = node.tenant_id AND "
        "release.id = node.desired_release_id AND release.cluster_id = node.cluster_id "
        "WHERE node.tenant_id = $1 AND node.id = $2 AND "
        "node.cluster_id = $3 AND node.registration_status = 'registered' AND "
        "node.agent_id = $4 AND node.deleted_at IS NULL "
        "LIMIT 1",
        principal.tenant_id_, principal.node_id_, principal.cluster_id_, principal.agent_id_);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return desired_summary{
        .node_spec_revision_ = rows.front()[0].template as<std::int64_t>().value_or(1),
        .release_id_ = std::string(rows.front()[1].value().value_or("")),
        .manifest_digest_ = std::string(rows.front()[2].value().value_or("")),
    };
}

}  // namespace service::agent
