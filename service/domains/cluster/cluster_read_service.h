#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/http.h"
#include "service/domains/cluster/cluster_error.h"
#include "service/domains/cluster/cluster_types.h"

namespace service::cluster {

class cluster_read_service final {
public:
    ruvia::task<cluster_page_data_dto> list(auto& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size,
        std::int64_t skip,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& dns_zone_id,
        const std::optional<std::string>& status) const {
        std::string where = cluster_source();
        std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}};
        std::optional<std::string> keyword_pattern;
        if (keyword) {
            keyword_pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where += " AND (cluster.name ILIKE " + placeholder +
                     " OR (cluster.hostname_prefix || '.' || zone.domain) ILIKE " + placeholder +
                     ")";
            params.emplace_back(std::string_view(*keyword_pattern));
        }
        if (dns_zone_id) {
            where += " AND cluster.dns_zone_id = $" + std::to_string(params.size() + 1);
            params.emplace_back(*dns_zone_id);
        }
        if (status) {
            where += " AND cluster.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }

        const auto count_rows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = count_rows.empty() ? std::int64_t{0}
                                              : count_rows.front()[0].template as<std::int64_t>().value_or(0);
        const auto rows = co_await c.db().query(
            cluster_columns() + where + " ORDER BY cluster.sort DESC LIMIT " +
                std::to_string(page_size) + " OFFSET " + std::to_string(skip),
            params);

        cluster_page_data_dto result({.resource_ = c.pool()});
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">(page_size > 0 ? (total + page_size - 1) / page_size : 0);
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            fill_cluster(items.emplace_back(), row);
        }
        co_return result;
    }

    // One cluster with exactly the list item's projection.
    ruvia::task<cluster_dto> get(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        const auto rows = co_await c.db().query(
            cluster_columns() + cluster_source() + " AND cluster.id = $2 LIMIT 1", tenant_id, id);
        if (rows.empty()) {
            service::common::throw_app_error(cluster_error::not_found);
        }
        cluster_dto result({.resource_ = c.pool()});
        fill_cluster(result, rows.front());
        co_return result;
    }

private:
    // The list query and its projection predate structured queries and are
    // not rewritten in this change; the detail read reuses them verbatim.
    static std::string cluster_source() {
        return " FROM sys_cluster cluster INNER JOIN sys_dns_zone zone ON zone.tenant_id = "
               "cluster.tenant_id AND zone.id = cluster.dns_zone_id INNER JOIN sys_provider "
               "provider ON provider.tenant_id = zone.tenant_id AND provider.id = "
               "zone.provider_id AND provider.kind = 'dns' WHERE cluster.tenant_id = $1 AND "
               "cluster.deleted_at IS NULL AND zone.deleted_at IS NULL AND provider.deleted_at IS "
               "NULL";
    }

    static std::string cluster_columns() {
        return "SELECT cluster.id, cluster.name, cluster.dns_zone_id, zone.domain, provider.provider, "
               "cluster.hostname_prefix, cluster.hostname_prefix || '.' || zone.domain, "
               "(SELECT COUNT(*) FROM sys_node node WHERE node.tenant_id = cluster.tenant_id "
               "AND node.cluster_id = cluster.id AND node.deleted_at IS NULL), (SELECT COUNT(*) FROM "
               "sys_node node WHERE node.tenant_id = cluster.tenant_id AND node.cluster_id = "
               "cluster.id AND node.deleted_at IS NULL AND node.registration_status = 'registered' "
               "AND "
               "node.status = 'enabled' AND node.last_heartbeat_at >= NOW() - INTERVAL '90 "
               "seconds'), cluster.status, cluster.revision, "
               "TO_CHAR(cluster.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "TO_CHAR(cluster.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')";
    }

    template <typename row_type>
    static void fill_cluster(cluster_dto& item, const row_type& row) {
        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"name">(row[1].value().value_or(""));
        item.template set<"dns_zone_id">(row[2].value().value_or(""));
        item.template set<"dns_zone_domain">(row[3].value().value_or(""));
        item.template set<"dns_provider_name">(row[4].value().value_or(""));
        item.template set<"hostname_prefix">(row[5].value().value_or(""));
        item.template set<"access_domain">(row[6].value().value_or(""));
        item.template set<"node_count">(row[7].template as<std::int64_t>().value_or(0));
        item.template set<"online_node_count">(row[8].template as<std::int64_t>().value_or(0));
        item.template set<"status">(row[9].value().value_or("disabled"));
        item.template set<"revision">(row[10].template as<std::int64_t>().value_or(1));
        item.template set<"created_at">(row[11].value().value_or(""));
        item.template set<"updated_at">(row[12].value().value_or(""));
    }
};

inline const cluster_read_service& get_cluster_read_service() {
    static const cluster_read_service service;
    return service;
}

}  // namespace service::cluster
