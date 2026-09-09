#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <vector>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>
#include "service/domains/agent/agent.types.h"

namespace service::agent {

struct ReleaseObjectRecord final {
    std::string digest, kind, payloadEnvelope;
};

template <typename Database>
    requires (std::same_as<Database, ruvia::DbHandle> || std::same_as<Database, ruvia::DbTransaction>)
inline ruvia::Task<std::vector<ReleaseObjectRecord>> loadReleaseObjects(
    Database& db, const AgentPrincipal& principal, std::string_view releaseId) {
        const auto rows = co_await db.query(
            "SELECT mapping.object_digest, object.kind, object.payload_envelope FROM "
            "sys_cluster_release_object mapping INNER JOIN sys_cluster_release release ON "
            "release.tenant_id = mapping.tenant_id AND release.id = mapping.release_id INNER JOIN "
            "sys_delivery_object object ON object.tenant_id = mapping.tenant_id AND "
            "object.digest_sha256 = mapping.object_digest INNER JOIN sys_node node ON "
            "node.tenant_id "
            "= mapping.tenant_id AND node.desired_release_id = mapping.release_id WHERE "
            "mapping.tenant_id = $1 AND mapping.release_id = $2 AND release.cluster_id = $3 AND "
            "node.cluster_id = release.cluster_id AND node.id = $4 AND node.agent_id = $5 AND "
            "node.registration_status = 'registered' AND node.deleted_at IS NULL ORDER BY "
            "mapping.position ASC",
            principal.tenantId, releaseId, principal.clusterId, principal.nodeId,
            principal.agentId);
    std::vector<ReleaseObjectRecord> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back({std::string(row[0].value().value_or("")),
            std::string(row[1].value().value_or("")), std::string(row[2].value().value_or(""))});
    }
    co_return result;
}

} // namespace service::agent