#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db_query.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "service/domains/certificate/certificate_filter.h"
#include "service/domains/dns_zone/dns_zone_filter.h"
#include "service/domains/node/node_connection.h"
#include "service/domains/overview/overview_types.h"

namespace service::overview {

class overview_service final {
public:
    ruvia::task<overview_data_dto> get(auto& c, const std::string& tenant_id) {
        const auto summary_rows = co_await c.db().query(
            "SELECT "
            "(SELECT COUNT(*) FROM sys_website website WHERE website.tenant_id = $1 AND "
            "website.deleted_at IS NULL), "
            "(SELECT COUNT(*) FROM sys_website_domain_claim claim INNER JOIN sys_website "
            "website ON website.tenant_id = claim.tenant_id AND website.id = claim.website_id "
            "WHERE claim.tenant_id = $1 AND website.deleted_at IS NULL), "
            "(SELECT COUNT(*) FROM sys_certificate certificate WHERE certificate.tenant_id = $1 "
            "AND certificate.deleted_at IS NULL), "
            "(SELECT COUNT(*) FROM sys_cluster cluster WHERE cluster.tenant_id = $1 AND "
            "cluster.deleted_at IS NULL), "
            "(SELECT COUNT(*) FROM sys_dns_zone zone WHERE zone.tenant_id = $1 AND "
            "zone.deleted_at IS NULL AND " +
                service::dns_zone::dns_zone_issue_sql() +
                "), "
                "(SELECT COUNT(*) FROM sys_certificate cert WHERE cert.tenant_id = $1 "
                "AND cert.deleted_at IS NULL AND " +
                service::certificate::certificate_expiring_sql() +
                "), "
                "(SELECT COUNT(*) FROM sys_certificate cert WHERE cert.tenant_id = $1 "
                "AND cert.deleted_at IS NULL AND " +
                service::certificate::certificate_abnormal_sql() +
                "), "
                "(SELECT COUNT(*) FROM sys_sync_task marker WHERE marker.tenant_id = $1 AND "
                "NOT marker.is_done), "
                "(SELECT COUNT(*) FROM sys_sync_task marker WHERE marker.tenant_id = $1 AND "
                "NOT marker.is_done AND marker.count_fails > 0)",
            tenant_id);

        // Same online rule as node lists and node_deadlines, which publish the
        // overview on every registration, enable/disable and online/offline change.
        ruvia::db_query nodes(c.pool());
        const auto node_count = nodes.aggregate("count", {nodes.star()});
        nodes
            .select({node_count,
                nodes.filter(nodes.aggregate("count", {nodes.star()}),
                    service::node::node_online_predicate(nodes, "node"))})
            .from("sys_node", "node")
            .where(nodes.binary(nodes.column("tenant_id", "node"), ruvia::db_binary_operator::equal,
                nodes.value(tenant_id)))
            .and_where(nodes.unary(ruvia::db_unary_operator::is_null,
                nodes.column("deleted_at", "node")));
        const auto node_rows = co_await c.db().query(nodes);
        overview_data_dto result({.resource_ = c.pool()});
        overview_resource_counts_dto resources({.resource_ = c.pool()});
        overview_issue_counts_dto issues({.resource_ = c.pool()});
        if (summary_rows.empty()) {
            resources.template set<"website_count">(0);
            resources.template set<"domain_count">(0);
            resources.template set<"certificate_count">(0);
            resources.template set<"cluster_count">(0);
            issues.template set<"dns_zone_issue_count">(0);
            issues.template set<"certificate_expiring_count">(0);
            issues.template set<"certificate_failed_count">(0);
            issues.template set<"active_marker_count">(0);
            issues.template set<"retry_marker_count">(0);
        } else {
            const auto& row = summary_rows.front();
            resources.template set<"website_count">(count(row, 0));
            resources.template set<"domain_count">(count(row, 1));
            resources.template set<"certificate_count">(count(row, 2));
            resources.template set<"cluster_count">(count(row, 3));
            issues.template set<"dns_zone_issue_count">(count(row, 4));
            issues.template set<"certificate_expiring_count">(count(row, 5));
            issues.template set<"certificate_failed_count">(count(row, 6));
            issues.template set<"active_marker_count">(count(row, 7));
            issues.template set<"retry_marker_count">(count(row, 8));
        }
        resources.template set<"node_count">(node_rows.empty() ? 0 : count(node_rows.front(), 0));
        resources.template set<"online_node_count">(node_rows.empty() ? 0
                                                                      : count(node_rows.front(), 1));
        result.template set<"resources">(std::move(resources));
        result.template set<"issues">(std::move(issues));

        const auto marker_rows = co_await c.db().query(
            "SELECT marker.id, marker.resource_type, marker.resource_id, CASE "
            "marker.resource_type WHEN 'provider' THEN COALESCE((SELECT CASE provider.kind WHEN "
            "'dns' THEN COALESCE(provider.name, provider.provider) ELSE provider.provider END "
            "FROM sys_provider provider WHERE provider.tenant_id = marker.tenant_id AND "
            "provider.id = marker.resource_id), '供应商') WHEN 'dns_zone' THEN COALESCE((SELECT "
            "zone.domain FROM sys_dns_zone zone WHERE zone.tenant_id = marker.tenant_id AND "
            "zone.id = marker.resource_id), 'DNS 托管域名') WHEN 'certificate' THEN "
            "COALESCE((SELECT "
            "certificate.domain FROM sys_certificate certificate WHERE certificate.tenant_id = "
            "marker.tenant_id AND certificate.id = marker.resource_id), '证书') WHEN 'website' "
            "THEN "
            "COALESCE((SELECT MIN(claim.domain_key) FROM sys_website_domain_claim claim WHERE "
            "claim.tenant_id = marker.tenant_id AND claim.website_id = marker.resource_id), "
            "'网站') "
            "ELSE marker.resource_type END, marker.operation, CASE WHEN marker.lease_until IS NOT "
            "NULL THEN 'running' WHEN marker.is_done AND marker.is_ok THEN 'completed' WHEN "
            "marker.count_fails > 0 THEN 'retry' ELSE 'pending' "
            "END, "
            "CASE WHEN marker.count_fails > 0 AND marker.error <> '' THEN marker.error ELSE NULL "
            "END, "
            "TO_CHAR(marker.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_sync_task "
            "marker "
            "WHERE marker.tenant_id = $1 ORDER BY marker.updated_at DESC, marker.id DESC LIMIT 8",
            tenant_id);
        auto& recent_markers = result.template ensure<"recent_markers">();
        for (const auto& row : marker_rows) {
            auto& marker = recent_markers.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            marker.template set<"id">(row[0].value().value_or(""));
            marker.template set<"resource_type">(row[1].value().value_or(""));
            marker.template set<"resource_id">(row[2].value().value_or(""));
            marker.template set<"resource_name">(row[3].value().value_or(""));
            marker.template set<"operation">(row[4].value().value_or(""));
            marker.template set<"status">(row[5].value().value_or("pending"));
            marker.template set<"updated_at">(row[7].value().value_or(""));
            if (const auto& last_error = row[6].value()) {
                marker.template set<"last_error">(*last_error);
            }
        }
        co_return result;
    }

private:
    template <typename row_type>
    static std::int64_t count(const row_type& row, std::size_t index) {
        return row[index].template as<std::int64_t>().value_or(0);
    }
};

inline overview_service& get_overview_service() {
    static overview_service instance;
    return instance;
}

}  // namespace service::overview
