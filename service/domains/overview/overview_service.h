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
                service::certificate::certificate_abnormal_sql() + ")",
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
        } else {
            const auto& row = summary_rows.front();
            resources.template set<"website_count">(count(row, 0));
            resources.template set<"domain_count">(count(row, 1));
            resources.template set<"certificate_count">(count(row, 2));
            resources.template set<"cluster_count">(count(row, 3));
            issues.template set<"dns_zone_issue_count">(count(row, 4));
            issues.template set<"certificate_expiring_count">(count(row, 5));
            issues.template set<"certificate_failed_count">(count(row, 6));
        }
        resources.template set<"node_count">(node_rows.empty() ? 0 : count(node_rows.front(), 0));
        resources.template set<"online_node_count">(node_rows.empty() ? 0
                                                                      : count(node_rows.front(), 1));
        result.template set<"resources">(std::move(resources));
        result.template set<"issues">(std::move(issues));
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
