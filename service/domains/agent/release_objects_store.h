#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db.h>

#include <concepts>
#include <string>
#include <string_view>
#include <vector>

#include "service/domains/agent/agent_types.h"

namespace service::agent {

struct release_object_record final {
    std::string digest_, kind_, payload_envelope_;
};

template <typename database_type>
    requires(std::same_as<database_type, ruvia::db_handle> || std::same_as<database_type, ruvia::db_transaction>)
inline ruvia::task<std::vector<release_object_record>> load_release_objects(
    database_type& db, const agent_principal& principal, std::string_view release_id) {
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
        principal.tenant_id_, release_id, principal.cluster_id_, principal.node_id_,
        principal.agent_id_);
    std::vector<release_object_record> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        result.push_back({std::string(row[0].value().value_or("")),
            std::string(row[1].value().value_or("")), std::string(row[2].value().value_or(""))});
    }
    co_return result;
}

}  // namespace service::agent
