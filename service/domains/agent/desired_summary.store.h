#pragma once

#include <concepts>
#include <optional>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>
#include "service/domains/agent/agent.types.h"

namespace service::agent {

// Accept either a request database handle or the caller's existing transaction.
template <typename Database>
    requires (std::same_as<Database, ruvia::DbHandle> || std::same_as<Database, ruvia::DbTransaction>)
inline ruvia::Task<std::optional<DesiredSummary>> findDesiredSummary(
    Database& db, const AgentPrincipal& principal) {
        const auto rows = co_await db.query(
            "SELECT node.node_spec_revision, release.id, release.manifest_digest FROM sys_node "
            "node "
            "INNER JOIN sys_cluster_release release ON release.tenant_id = node.tenant_id AND "
            "release.id = node.desired_release_id AND release.cluster_id = node.cluster_id "
            "WHERE node.tenant_id = $1 AND node.id = $2 AND "
            "node.cluster_id = $3 AND node.registration_status = 'registered' AND "
            "node.agent_id = $4 AND node.deleted_at IS NULL "
            "LIMIT 1",
            principal.tenantId, principal.nodeId, principal.clusterId, principal.agentId);
        if (rows.empty()) co_return std::nullopt;
        co_return DesiredSummary{
            .nodeSpecRevision = rows.front()[0].template as<std::int64_t>().value_or(1),
            .releaseId = std::string(rows.front()[1].value().value_or("")),
            .manifestDigest = std::string(rows.front()[2].value().value_or("")),
        };
}

} // namespace service::agent
