#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <optional>
#include <string>
#include <string_view>

namespace service::cluster {

// Locks an enabled live cluster FOR SHARE and returns the zone it publishes
// under; `std::nullopt` when it is missing, disabled or deleted. Lock order:
// the caller has locked its own row; zones and certificates it references are
// locked after the cluster.
inline ruvia::task<std::optional<std::string>> lock_enabled_cluster(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view cluster_id) {
    const auto rows = co_await transaction.query(
        "SELECT dns_zone_id FROM sys_cluster WHERE id = $1 AND tenant_id = $2 AND status = "
        "'enabled' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
        cluster_id, tenant_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return std::string(rows.front()[0].value().value_or(""));
}

// Whether a live cluster publishes under the zone. The caller holds the zone
// FOR UPDATE, so no cluster can start referencing it meanwhile.
inline ruvia::task<bool> clusters_use_dns_zone(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view zone_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_cluster WHERE tenant_id = $1 AND dns_zone_id = $2 AND deleted_at IS NULL LIMIT 1",
        tenant_id, zone_id);
    co_return !rows.empty();
}

}  // namespace service::cluster
