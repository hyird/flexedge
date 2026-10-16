#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/db/db_query.h>

#include <cstdint>
#include <optional>
#include <string>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/cluster/cluster_error.h"
#include "service/domains/cluster/cluster_types.h"
#include "service/domains/provider/provider_reference.h"
#include "service/features/node_presence/node_connection.h"

namespace service::cluster {

class cluster_read_service final {
public:
    ruvia::task<ruvia::array<cluster_dto>> options(auto& c, const std::string& tenant_id, const std::optional<std::string>& keyword) const {
        ruvia::db_query query(c.pool());
        filter_query(query, tenant_id, keyword, std::nullopt, std::nullopt);
        project(query);
        const auto rows = co_await c.db().query(query);
        ruvia::array<cluster_dto> items({.resource_ = c.pool()});
        for (const auto& row : rows) {
            fill_cluster(items.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row);
        }
        co_return items;
    }

    ruvia::task<cluster_page_data_dto> list(auto& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size,
        std::int64_t skip,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& dns_zone_id,
        const std::optional<std::string>& status) const {
        const auto filter = [&](ruvia::db_query& query) { filter_query(query, tenant_id, keyword, dns_zone_id, status); };
        const auto page_rows = co_await service::common::query_page(c, filter, project, page_size, skip);
        const auto total = page_rows.total_;

        cluster_page_data_dto result({.resource_ = c.pool()});
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">(page_size > 0 ? (total + page_size - 1) / page_size : 0);
        auto& items = result.template ensure<"list">();
        for (const auto& row : page_rows.rows_) {
            fill_cluster(items.emplace_back(), row);
        }
        co_return result;
    }

    // One cluster with exactly the list item's projection.
    ruvia::task<cluster_dto> get(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        ruvia::db_query query(c.pool());
        source(query, tenant_id);
        query.and_where(query.sql({"cluster.id = ", ""}, {query.value(id)}));
        project(query);
        query.limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::throw_app_error(cluster_error::not_found);
        }
        cluster_dto result({.resource_ = c.pool()});
        fill_cluster(result, rows.front());
        co_return result;
    }

private:
    static void filter_query(ruvia::db_query& query, const std::string& tenant_id, const std::optional<std::string>& keyword, const std::optional<std::string>& dns_zone_id, const std::optional<std::string>& status) {
        std::optional<std::string> keyword_pattern;
        if (keyword) {
            keyword_pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
        }
        source(query, tenant_id);
        if (keyword_pattern) {
            const auto pattern = query.value(*keyword_pattern);
            query.and_where(query.sql({"cluster.name ILIKE ",
                                          " OR (cluster.hostname_prefix || '.' || zone.domain) ILIKE ",
                                          ""},
                {pattern, pattern}));
        }
        if (dns_zone_id) {
            query.and_where(query.sql({"cluster.dns_zone_id = ", ""}, {query.value(*dns_zone_id)}));
        }
        if (status) {
            query.and_where(query.sql({"cluster.status = ", ""}, {query.value(*status)}));
        }
    }

    static void source(ruvia::db_query& query, const std::string& tenant_id) {
        query.from("sys_cluster", "cluster")
            .join(ruvia::db_join_type::inner, "sys_dns_zone",
                query.sql("zone.tenant_id = cluster.tenant_id AND zone.id = cluster.dns_zone_id"), "zone")
            .join(ruvia::db_join_type::inner, "sys_provider",
                query.sql("provider.tenant_id = zone.tenant_id AND provider.id = zone.provider_id"),
                "provider")
            .where(query.sql({"cluster.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(query.sql("cluster.deleted_at IS NULL AND zone.deleted_at IS NULL"))
            .and_where(service::provider::live_dns_provider_predicate(query, "provider"));
    }

    static void project(ruvia::db_query& query) {
        auto columns = service::common::sql_columns(query,
            {"cluster.id", "cluster.name", "cluster.dns_zone_id", "zone.domain", "provider.provider",
                "cluster.hostname_prefix", "cluster.hostname_prefix || '.' || zone.domain",
                "SELECT COUNT(*) FROM sys_node node WHERE node.tenant_id = cluster.tenant_id AND "
                "node.cluster_id = cluster.id AND node.deleted_at IS NULL"});
        columns.push_back(query.sql({"SELECT COUNT(*) FROM sys_node node WHERE node.tenant_id = "
                                     "cluster.tenant_id AND node.cluster_id = cluster.id AND "
                                     "node.deleted_at IS NULL AND ",
                                        ""},
            {service::node::node_online_predicate(query, "node")}));
        for (const auto column : {"cluster.status", "cluster.revision",
                 "TO_CHAR(cluster.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                 "TO_CHAR(cluster.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')"}) {
            columns.push_back(query.sql(column));
        }
        query.select(columns).order_by(query.sql("cluster.sort"), ruvia::db_order_direction::desc);
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
