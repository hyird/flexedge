#pragma once

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/domain_name.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/website_config/model.h"

namespace service::website_dns {

template <typename Db>
inline ruvia::Task<std::set<std::string>> managedZoneIds(Db& db, const std::string& tenantId,
                                                         std::string_view configJson) {
    const auto config = service::website_config::parseStored(configJson);
    if (!config) {
        throw std::runtime_error("网站聚合配置损坏");
    }

    const auto rows = co_await db.query(
        "SELECT id, domain FROM sys_dns_zone WHERE tenant_id = $1 AND deleted_at IS NULL ORDER "
        "BY length(domain) DESC, sort ASC",
        tenantId);
    std::set<std::string> result;
    for (const auto& domain : config->domains) {
        if (domain.dnsMode != "managed") {
            continue;
        }
        const auto owner = std::ranges::find_if(rows, [&](const auto& row) {
            return service::common::domainBelongsToZone(domain.hostname,
                                                        row[1].value().value_or(""));
        });
        if (owner != rows.end()) {
            result.emplace((*owner)[0].value().value_or(""));
        }
    }
    co_return result;
}

inline ruvia::Task<void> reconcileConfigChange(ruvia::DbTransaction& transaction,
                                               const std::string& tenantId,
                                               std::optional<std::string_view> previousConfig,
                                               std::optional<std::string_view> nextConfig) {
    std::set<std::string> zoneIds;
    if (previousConfig) {
        auto previous = co_await managedZoneIds(transaction, tenantId, *previousConfig);
        zoneIds.insert(previous.begin(), previous.end());
    }
    if (nextConfig) {
        auto next = co_await managedZoneIds(transaction, tenantId, *nextConfig);
        zoneIds.insert(next.begin(), next.end());
    }
    for (const auto& zoneId : zoneIds) {
        (void)co_await service::dns_sync::markZoneDirty(transaction, tenantId, zoneId);
    }
    co_return;
}

inline ruvia::Task<void> reconcileClusterConsumers(ruvia::DbTransaction& transaction,
                                                   const std::string& tenantId,
                                                   const std::string& clusterId) {
    const auto rows = co_await transaction.query(
        "SELECT DISTINCT claim.dns_zone_id FROM sys_website_domain_claim claim INNER JOIN "
        "sys_website website ON website.tenant_id = claim.tenant_id AND website.id = "
        "claim.website_id WHERE website.tenant_id = $1 AND website.cluster_id = $2 AND "
        "website.status = 'enabled' AND website.deleted_at IS NULL AND claim.dns_mode = "
        "'managed' AND claim.dns_zone_id IS NOT NULL ORDER BY claim.dns_zone_id",
        tenantId, clusterId);
    for (const auto& row : rows) {
        (void)co_await service::dns_sync::markZoneDirty(transaction, tenantId,
                                                        std::string(row[0].value().value_or("")));
    }
    co_return;
}

} // namespace service::website_dns
