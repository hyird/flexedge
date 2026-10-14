#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/model_json.h>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/common/domain_name.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/website_config/mapper.h"

namespace service::website_dns {

template <typename db_type>
inline ruvia::task<std::set<std::string>> managed_zone_ids(db_type& db, const std::string& tenant_id,
    std::string_view config_json) {
    const auto config = service::website_config::parse_stored(config_json);
    if (!config) {
        throw std::runtime_error("网站聚合配置损坏");
    }

    const auto rows = co_await db.query(
        "SELECT id, domain FROM sys_dns_zone WHERE tenant_id = $1 AND deleted_at IS NULL ORDER "
        "BY length(domain) DESC, sort ASC",
        tenant_id);
    std::set<std::string> result;
    for (const auto& domain : config->domains_) {
        if (domain.dns_mode_ != "managed") {
            continue;
        }
        const auto owner = std::ranges::find_if(rows, [&](const auto& row) {
            return service::common::domain_belongs_to_zone(domain.hostname_,
                row[1].value().value_or(""));
        });
        if (owner != rows.end()) {
            result.emplace((*owner)[0].value().value_or(""));
        }
    }
    co_return result;
}

inline ruvia::task<void> reconcile_config_change(ruvia::db_transaction& transaction,
    const std::string& tenant_id,
    std::optional<std::string_view> previous_config,
    std::optional<std::string_view> next_config) {
    std::set<std::string> zone_ids;
    if (previous_config) {
        auto previous = co_await managed_zone_ids(transaction, tenant_id, *previous_config);
        zone_ids.insert(previous.begin(), previous.end());
    }
    if (next_config) {
        auto next = co_await managed_zone_ids(transaction, tenant_id, *next_config);
        zone_ids.insert(next.begin(), next.end());
    }
    for (const auto& zone_id : zone_ids) {
        (void)co_await service::dns_sync::mark_zone_dirty(transaction, tenant_id, zone_id);
    }
    co_return;
}

inline ruvia::task<void> reconcile_cluster_consumers(ruvia::db_transaction& transaction,
    const std::string& tenant_id,
    const std::string& cluster_id) {
    const auto rows = co_await transaction.query(
        "SELECT DISTINCT claim.dns_zone_id FROM sys_website_domain_claim claim INNER JOIN "
        "sys_website website ON website.tenant_id = claim.tenant_id AND website.id = "
        "claim.website_id WHERE website.tenant_id = $1 AND website.cluster_id = $2 AND "
        "website.status = 'enabled' AND website.deleted_at IS NULL AND claim.dns_mode = "
        "'managed' AND claim.dns_zone_id IS NOT NULL ORDER BY claim.dns_zone_id",
        tenant_id, cluster_id);
    for (const auto& row : rows) {
        (void)co_await service::dns_sync::mark_zone_dirty(transaction, tenant_id,
            std::string(row[0].value().value_or("")));
    }
    co_return;
}

}  // namespace service::website_dns
