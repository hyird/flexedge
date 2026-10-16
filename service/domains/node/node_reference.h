#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <string_view>

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

}  // namespace service::node
