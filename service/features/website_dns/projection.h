#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <set>
#include <string>
#include <string_view>

#include "service/features/background/queued_work.h"
#include "service/features/dns_sync/queue.h"

namespace service::website_dns {

// The zones the website's persisted managed domain claims belong to.
template <typename db_type>
inline ruvia::task<std::set<std::string>> managed_zone_ids(db_type& db, std::string_view tenant_id,
    std::string_view website_id) {
    const auto rows = co_await db.query(
        "SELECT DISTINCT dns_zone_id FROM sys_website_domain_claim WHERE tenant_id = $1 AND website_id = $2 AND "
        "dns_mode = 'managed' AND dns_zone_id IS NOT NULL",
        tenant_id, website_id);
    std::set<std::string> result;
    for (const auto& row : rows) {
        result.emplace(row[0].value().value_or(""));
    }
    co_return result;
}

// The website's domain claims were written: the zones of its previous and
// current managed claims re-project their CNAME records.
inline ruvia::task<void> reconcile_claim_change(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view website_id, const std::set<std::string>& previous_zone_ids,
    service::background::queued_work& work) {
    auto zone_ids = co_await managed_zone_ids(transaction, tenant_id, website_id);
    zone_ids.insert(previous_zone_ids.begin(), previous_zone_ids.end());
    for (const auto& zone_id : zone_ids) {
        co_await service::dns_sync::mark_zone_dirty(transaction, tenant_id, zone_id, work);
    }
}

// The cluster's access hostname or state changed: every zone its enabled
// websites' managed claims belong to re-projects its CNAME records.
inline ruvia::task<void> reconcile_cluster_consumers(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view cluster_id, service::background::queued_work& work) {
    const auto rows = co_await transaction.query(
        "SELECT DISTINCT claim.dns_zone_id FROM sys_website_domain_claim claim INNER JOIN "
        "sys_website website ON website.tenant_id = claim.tenant_id AND website.id = "
        "claim.website_id WHERE website.tenant_id = $1 AND website.cluster_id = $2 AND "
        "website.status = 'enabled' AND website.deleted_at IS NULL AND claim.dns_mode = "
        "'managed' AND claim.dns_zone_id IS NOT NULL ORDER BY claim.dns_zone_id",
        tenant_id, cluster_id);
    for (const auto& row : rows) {
        co_await service::dns_sync::mark_zone_dirty(transaction, tenant_id, row[0].value().value_or(""), work);
    }
}

}  // namespace service::website_dns
