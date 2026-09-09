#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>
#include "service/domains/agent/agent.types.h"

namespace service::agent {

struct DesiredStateRecord final {
    std::int64_t revision;
    std::string name;
    bool enabled;
    std::string configJson, releaseId, manifestDigest, manifestEnvelope;
};

inline ruvia::Task<std::optional<DesiredStateRecord>> lockDesiredState(
    ruvia::DbTransaction& transaction, const AgentPrincipal& principal) {
    const auto rows = co_await transaction.query(
        "SELECT node.node_spec_revision, node.name, node.status = 'enabled', node.config::text, "
        "node.desired_release_id, release.manifest_digest, release.manifest_envelope FROM "
        "sys_node node INNER JOIN sys_cluster_release release ON release.tenant_id = "
        "node.tenant_id AND release.id = node.desired_release_id AND release.cluster_id = node.cluster_id "
        "WHERE node.tenant_id = $1 AND node.id = $2 AND node.cluster_id = $3 AND node.agent_id = $4 AND "
        "node.registration_status = 'registered' AND node.deleted_at IS NULL LIMIT 1 FOR UPDATE OF node",
        principal.tenantId, principal.nodeId, principal.clusterId, principal.agentId);
    if (rows.empty()) co_return std::nullopt;
    const auto& row = rows.front();
    co_return DesiredStateRecord{
        .revision = row[0].as<std::int64_t>().value_or(1),
        .name = std::string(row[1].value().value_or("")),
        .enabled = row[2].as<bool>().value_or(false),
        .configJson = std::string(row[3].value().value_or("{}")),
        .releaseId = std::string(row[4].value().value_or("")),
        .manifestDigest = std::string(row[5].value().value_or("")),
        .manifestEnvelope = std::string(row[6].value().value_or(""))};
}

inline ruvia::Task<bool> recordNodeSpecDigest(ruvia::DbTransaction& transaction,
    std::string_view tenantId, std::string_view nodeId, std::int64_t revision,
    std::string_view digest) {
    const auto updated = co_await transaction.execute(
        "UPDATE sys_node SET node_spec_digest = $3, updated_at = NOW() WHERE tenant_id = $1 "
        "AND id = $2 AND node_spec_revision = $4",
        tenantId, nodeId, digest, revision);
    co_return updated.affectedRows() == 1;
}

} // namespace service::agent