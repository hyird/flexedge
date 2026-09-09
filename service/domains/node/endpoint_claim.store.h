#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>
#include "service/features/node_config/model.h"

namespace service::node {

inline ruvia::Task<void> clearEndpointClaims(ruvia::DbTransaction& transaction,
                                           const std::string& tenantId, const std::string& nodeId) {
    (void)co_await transaction.execute(
        "DELETE FROM sys_node_endpoint_claim WHERE tenant_id = $1 AND node_id = $2", tenantId,
        nodeId);
}

inline ruvia::Task<void> replaceEndpointClaims(ruvia::DbTransaction& transaction, const std::string& tenantId,
                      const std::string& nodeId,
                      std::span<const service::node_config::NodeEndpointData> endpoints) {
    co_await clearEndpointClaims(transaction, tenantId, nodeId);
    if (endpoints.empty()) {
        co_return;
    }
    std::vector<ruvia::DbValue> endpointInsertParams;
    endpointInsertParams.reserve(endpoints.size() * 4);
    std::string endpointInsertSql =
        "INSERT INTO sys_node_endpoint_claim (tenant_id, node_id, endpoint_id, "
        "ip_address) VALUES ";
    const auto appendParam = [&](ruvia::DbValue value) {
        endpointInsertSql += "$" + std::to_string(endpointInsertParams.size() + 1);
        endpointInsertParams.push_back(std::move(value));
    };
    for (const auto& endpoint : endpoints) {
        if (!endpointInsertParams.empty()) {
            endpointInsertSql += ", ";
        }
        endpointInsertSql += "(";
        appendParam(ruvia::DbValue{tenantId});
        endpointInsertSql += ", ";
        appendParam(ruvia::DbValue{nodeId});
        endpointInsertSql += ", ";
        appendParam(ruvia::DbValue{endpoint.id});
        endpointInsertSql += ", ";
        appendParam(ruvia::DbValue{endpoint.ipAddress});
        endpointInsertSql += "::inet)";
    }
    (void)co_await transaction.execute(endpointInsertSql, endpointInsertParams);
    co_return;
}

} // namespace service::node
