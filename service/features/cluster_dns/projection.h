#pragma once

#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/features/dns_sync/queue.h"

namespace service::cluster_dns {

inline ruvia::Task<void> reconcileCluster(ruvia::DbTransaction& transaction,
                                          const std::string& tenantId,
                                          const std::string& clusterId) {
    const auto rows = co_await transaction.query(
        "SELECT dns_zone_id FROM sys_cluster WHERE tenant_id = $1 AND id = $2 LIMIT 1", tenantId,
        clusterId);
    if (!rows.empty()) {
        (void)co_await service::dns_sync::markZoneDirty(
            transaction, tenantId, std::string(rows.front()[0].value().value_or("")));
    }
    co_return;
}

inline ruvia::Task<void> reconcileZoneTransition(ruvia::DbTransaction& transaction,
                                                 const std::string& tenantId,
                                                 const std::string& previousZoneId,
                                                 const std::string& nextZoneId) {
    (void)co_await service::dns_sync::markZoneDirty(transaction, tenantId, previousZoneId);
    if (nextZoneId != previousZoneId) {
        (void)co_await service::dns_sync::markZoneDirty(transaction, tenantId, nextZoneId);
    }
    co_return;
}

} // namespace service::cluster_dns
