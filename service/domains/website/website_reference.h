#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <string_view>

namespace service::website {

// Whether a website claims a name under the zone. The caller holds the zone
// FOR UPDATE, so no website can start claiming under it meanwhile.
inline ruvia::task<bool> websites_claim_dns_zone(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view zone_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_website_domain_claim WHERE tenant_id = $1 AND dns_zone_id = $2 LIMIT 1",
        tenant_id, zone_id);
    co_return !rows.empty();
}

// Whether a website serves the certificate. The caller holds the certificate
// FOR UPDATE, so no website can start binding it meanwhile.
inline ruvia::task<bool> websites_bind_certificate(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view certificate_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_website_certificate_binding WHERE tenant_id = $1 AND certificate_id = $2 LIMIT 1",
        tenant_id, certificate_id);
    co_return !rows.empty();
}

// How the websites of a cluster still depend on it.
struct cluster_website_usage {
    bool live_;
    bool cleanup_active_;
};

// Live websites of the cluster, and deleted ones whose delete task has not
// finished. The caller holds the cluster FOR UPDATE, so no website can move
// onto it meanwhile.
inline ruvia::task<cluster_website_usage> websites_use_cluster(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view cluster_id) {
    const auto rows = co_await transaction.query(
        "SELECT EXISTS(SELECT 1 FROM sys_website WHERE tenant_id = $1 AND cluster_id = $2 AND "
        "deleted_at IS NULL), EXISTS(SELECT 1 FROM sys_website website INNER JOIN sys_sync_task task ON "
        "task.tenant_id = website.tenant_id AND task.resource_type = 'website' AND task.resource_id = "
        "website.id AND task.operation = 'delete' AND NOT task.is_done WHERE website.tenant_id = $1 AND "
        "website.cluster_id = $2 AND website.deleted_at IS NOT NULL)",
        tenant_id, cluster_id);
    co_return cluster_website_usage{rows.front()[0].as<bool>().value_or(false),
        rows.front()[1].as<bool>().value_or(false)};
}

}  // namespace service::website
