#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <span>
#include <string>
#include <utility>
#include <vector>

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
    std::vector<ruvia::db_value> endpoint_insert_params;
    endpoint_insert_params.reserve(endpoints.size() * 4);
    std::string endpoint_insert_sql =
        "INSERT INTO sys_node_endpoint_claim (tenant_id, node_id, endpoint_id, "
        "ip_address) VALUES ";
    const auto append_param = [&](ruvia::db_value value) {
        endpoint_insert_sql += "$" + std::to_string(endpoint_insert_params.size() + 1);
        endpoint_insert_params.push_back(std::move(value));
    };
    for (const auto& endpoint : endpoints) {
        if (!endpoint_insert_params.empty()) {
            endpoint_insert_sql += ", ";
        }
        endpoint_insert_sql += "(";
        append_param(ruvia::db_value{tenant_id});
        endpoint_insert_sql += ", ";
        append_param(ruvia::db_value{node_id});
        endpoint_insert_sql += ", ";
        append_param(ruvia::db_value{endpoint.id_});
        endpoint_insert_sql += ", ";
        append_param(ruvia::db_value{endpoint.ip_address_});
        endpoint_insert_sql += "::inet)";
    }
    (void)co_await transaction.execute(endpoint_insert_sql, endpoint_insert_params);
    co_return;
}

}  // namespace service::node
