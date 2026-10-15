#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <string>

#include "service/features/dns_sync/queue.h"

namespace service::cluster_dns {

inline ruvia::task<void> reconcile_cluster(ruvia::db_transaction& transaction,
    const std::string& tenant_id,
    const std::string& cluster_id, service::operation::work_origin& origin) {
    const auto rows = co_await transaction.query(
        "SELECT dns_zone_id FROM sys_cluster WHERE tenant_id = $1 AND id = $2 LIMIT 1", tenant_id,
        cluster_id);
    if (!rows.empty()) {
        (void)co_await service::dns_sync::mark_zone_dirty(
            transaction, tenant_id, std::string(rows.front()[0].value().value_or("")), origin);
    }
    co_return;
}

inline ruvia::task<void> reconcile_zone_transition(ruvia::db_transaction& transaction,
    const std::string& tenant_id,
    const std::string& previous_zone_id,
    const std::string& next_zone_id, service::operation::work_origin& origin) {
    (void)co_await service::dns_sync::mark_zone_dirty(transaction, tenant_id, previous_zone_id, origin);
    if (next_zone_id != previous_zone_id) {
        (void)co_await service::dns_sync::mark_zone_dirty(transaction, tenant_id, next_zone_id, origin);
    }
    co_return;
}

}  // namespace service::cluster_dns
