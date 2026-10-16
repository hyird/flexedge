#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_transaction.h>

#include <span>
#include <string>

#include "service/features/node_config/model.h"

namespace service::node {

inline ruvia::task<void> clear_endpoint_claims(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& node_id) {
    (void)co_await transaction.execute(
        "DELETE FROM sys_node_endpoint_claim WHERE tenant_id = $1 AND node_id = $2", tenant_id,
        node_id);
}

inline ruvia::task<void> replace_endpoint_claims(ruvia::db_transaction& transaction, const std::string& tenant_id,
    const std::string& node_id,
    std::span<const service::node_config::node_endpoint_data> endpoints) {
    co_await clear_endpoint_claims(transaction, tenant_id, node_id);
    if (endpoints.empty()) {
        co_return;
    }
    ruvia::db_query insert;
    insert.insert_into("sys_node_endpoint_claim", {"tenant_id", "node_id", "endpoint_id", "ip_address"});
    for (const auto& endpoint : endpoints) {
        insert.values({insert.value(tenant_id), insert.value(node_id), insert.value(endpoint.id_),
            insert.sql({"", "::inet"}, {insert.value(endpoint.ip_address_)})});
    }
    (void)co_await transaction.execute(insert);
    co_return;
}

}  // namespace service::node
