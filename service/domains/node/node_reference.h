#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <string>
#include <string_view>
#include <vector>

namespace service::node {

// Whether a live node belongs to the cluster. The caller holds the cluster
// FOR UPDATE, so no node can join it meanwhile.
inline ruvia::task<bool> nodes_use_cluster(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view cluster_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_node WHERE tenant_id = $1 AND cluster_id = $2 AND deleted_at IS NULL LIMIT 1",
        tenant_id, cluster_id);
    co_return !rows.empty();
}

inline ruvia::task<std::vector<std::string>> cache_operation_nodes(
    ruvia::db_transaction& tx, const std::string& tenant, const std::string& cluster = {}) {
    const auto rows = co_await tx.query("SELECT id FROM sys_node WHERE tenant_id=$1 AND ($2='' OR cluster_id=NULLIF($2,'')::uuid) AND deleted_at IS NULL AND registration_status='registered' AND status='enabled'", tenant, cluster);
    std::vector<std::string> result;
    for (const auto& row : rows) {
        result.emplace_back(row[0].value().value());
    }
    co_return result;
}
}  // namespace service::node
