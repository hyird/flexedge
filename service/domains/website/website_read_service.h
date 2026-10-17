#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/website/website_error.h"
#include "service/domains/website/website_runtime_mapper.h"
#include "service/domains/website/website_types.h"
#include "service/features/node_runtime/mapper.h"
#include "service/features/website_config/mapper.h"
#include "service/features/website_dns/mapper.h"

namespace service::website {

class website_read_service final {
public:
    ruvia::task<website_page_data_dto> list(auto& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size,
        std::int64_t skip,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& cluster_id,
        const std::optional<std::string>& status,
        const std::optional<std::string>& certificate_id) const {
        std::optional<std::string> keyword_pattern;
        if (keyword) {
            keyword_pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
        }
        const auto filter = [&](ruvia::db_query& query) {
            source(query, tenant_id);
            if (cluster_id) {
                query.and_where(query.sql({"website.cluster_id = ", ""}, {query.value(*cluster_id)}));
            }
            if (status) {
                query.and_where(query.sql({"website.status = ", ""}, {query.value(*status)}));
            }
            if (certificate_id) {
                query.and_where(query.sql(
                    {"EXISTS (SELECT 1 FROM sys_website_certificate_binding binding WHERE "
                     "binding.tenant_id = website.tenant_id AND binding.website_id = website.id AND "
                     "binding.certificate_id = ",
                        ")"},
                    {query.value(*certificate_id)}));
            }
            if (keyword_pattern) {
                const auto pattern = query.value(*keyword_pattern);
                query.and_where(query.sql(
                    {"cluster.name ILIKE ",
                        " OR EXISTS (SELECT 1 FROM sys_website_domain_claim claim WHERE "
                        "claim.tenant_id = website.tenant_id AND claim.website_id = website.id AND "
                        "claim.domain_key ILIKE ",
                        ")"},
                    {pattern, pattern}));
            }
        };
        const auto page_rows = co_await service::common::query_page(c, filter, project, page_size, skip);
        const auto& rows = page_rows.rows_;
        std::vector<std::string> website_ids;
        website_ids.reserve(rows.size());
        for (const auto& row : rows) {
            website_ids.emplace_back(row[0].value().value_or(""));
        }
        const auto certificates = co_await load_bound_certificates_by_website(c, tenant_id, website_ids);

        auto result = service::common::init_page_dto<website_page_data_dto>(
            c.pool(), page_rows.total_, page, page_size);
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            const auto config = parse_config(c, row[5].value().value_or("{}"));
            const auto available = certificates.find(std::string(row[0].value().value_or("")));
            fill_website(c, items.emplace_back(), row, config,
                available == certificates.end() ? empty_certificates() : available->second);
        }
        co_return result;
    }

    ruvia::task<website_dto> detail(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        ruvia::db_query query(c.pool());
        source(query, tenant_id);
        query.and_where(query.sql({"website.id = ", ""}, {query.value(id)}));
        project(query);
        query.limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::throw_app_error(website_error::not_found);
        }
        const auto config = parse_config(c, rows.front()[5].value().value_or("{}"));
        const auto certificates = co_await load_bound_certificates_by_website(c, tenant_id, {id});
        ruvia::array<website_origin_source_dto> sources({.resource_ = c.pool()});
        const auto origin_states =
            co_await load_origin_runtime(c, tenant_id, rows.front()[1].value().value_or(""), id, sources);
        website_dto result({.resource_ = c.pool()});
        const auto available = certificates.find(id);
        fill_website(c, result, rows.front(), config,
            available == certificates.end() ? empty_certificates() : available->second,
            origin_states);
        result.template ensure<"runtime">().template set<"origin_sources">(std::move(sources));
        co_return result;
    }

private:
    using bound_certificate = detail::bound_certificate;
    using bound_certificates_by_website =
        std::unordered_map<std::string, std::vector<bound_certificate>>;
    using origin_runtime_state = detail::origin_runtime_state;

    static void source(ruvia::db_query& query, const std::string& tenant_id) {
        query.from("sys_website", "website")
            .join(ruvia::db_join_type::inner, "sys_cluster",
                query.sql("cluster.tenant_id = website.tenant_id AND cluster.id = website.cluster_id"),
                "cluster")
            .join(ruvia::db_join_type::inner, "sys_dns_zone",
                query.sql("zone.tenant_id = cluster.tenant_id AND zone.id = cluster.dns_zone_id"),
                "zone")
            .where(query.sql({"website.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(query.sql("website.deleted_at IS NULL AND cluster.deleted_at IS NULL AND "
                                 "zone.deleted_at IS NULL"));
    }

    static void project(ruvia::db_query& query) {
        query
            .select(service::common::sql_columns(query,
                {"website.id", "website.cluster_id", "cluster.name",
                    "cluster.hostname_prefix || '.' || zone.domain", "website.revision",
                    "website.config::text", "website.runtime::text",
                    "SELECT COUNT(*) FROM sys_node node WHERE node.tenant_id = website.tenant_id AND "
                    "node.cluster_id = website.cluster_id AND node.deleted_at IS NULL AND "
                    "node.status = 'enabled' AND node.registration_status = 'registered'",
                    "SELECT COUNT(*) FROM sys_node node WHERE node.tenant_id = website.tenant_id AND "
                    "node.cluster_id = website.cluster_id AND node.deleted_at IS NULL AND node.status "
                    "= 'enabled' AND node.registration_status = 'registered' AND "
                    "node.desired_release_id IS NOT NULL AND node.active_release_id = "
                    "node.desired_release_id",
                    "TO_CHAR(website.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                    "TO_CHAR(website.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')", "website.status"}))
            .order_by(query.sql("website.sort"), ruvia::db_order_direction::desc);
    }

    static service::website_config::website_config_data parse_config(auto& c,
        std::string_view json) {
        auto config = service::website_config::parse_stored(json, {.resource_ = c.pool()});
        if (!config) {
            throw_corrupt_config();
        }
        return std::move(*config);
    }

    static const std::vector<bound_certificate>& empty_certificates() {
        static const std::vector<bound_certificate> value;
        return value;
    }

    static ruvia::task<bound_certificates_by_website>
    load_bound_certificates_by_website(auto& c, const std::string& tenant_id,
        const std::vector<std::string>& website_ids) {
        bound_certificates_by_website result;
        if (website_ids.empty()) {
            co_return result;
        }
        ruvia::db_query query(c.pool());
        std::vector<ruvia::db_expression> ids;
        ids.reserve(website_ids.size());
        for (const auto& website_id : website_ids) {
            ids.push_back(query.value(website_id));
        }
        query
            .select(service::common::sql_columns(query,
                {"binding.website_id", "certificate.id", "certificate.subject_alt_names[1]",
                    "certificate.subject_alt_names[2]",
                    "COALESCE(certificate.issued_revision > 0 AND certificate.expires_at > NOW(), FALSE)"}))
            .from("sys_website_certificate_binding", "binding")
            .join(ruvia::db_join_type::inner, "sys_certificate",
                query.sql("certificate.tenant_id = binding.tenant_id AND certificate.id = "
                          "binding.certificate_id"),
                "certificate")
            .where(query.sql({"binding.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(query.binary(query.sql("binding.website_id"), ruvia::db_binary_operator::in,
                query.list(ids)))
            .and_where(query.sql("certificate.deleted_at IS NULL"))
            .order_by(query.sql("binding.website_id"))
            .add_order_by(query.sql("binding.position"));
        const auto rows = co_await c.db().query(query);
        for (const auto& row : rows) {
            std::vector<std::string> domains;
            if (const auto value = row[2].value()) {
                domains.emplace_back(*value);
            }
            if (const auto value = row[3].value()) {
                domains.emplace_back(*value);
            }
            result[std::string(row[0].value().value_or(""))].push_back(
                {.id_ = std::string(row[1].value().value_or("")),
                    .domains_ = std::move(domains),
                    .usable_ = row[4].template as<bool>().value_or(false)});
        }
        co_return result;
    }

    static ruvia::task<std::vector<origin_runtime_state>>
    load_origin_runtime(auto& c, const std::string& tenant_id, std::string_view cluster_id,
        std::string_view website_id, ruvia::array<website_origin_source_dto>& sources) {
        const auto rows = co_await c.db().query(
            "SELECT id, name, runtime::text, revision, "
            "TO_CHAR(last_heartbeat_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') "
            "FROM sys_node WHERE tenant_id = $1 AND cluster_id = "
            "$2 AND status = 'enabled' AND registration_status = 'registered' AND "
            "deleted_at IS NULL ORDER BY sort ASC",
            tenant_id, cluster_id);
        std::vector<origin_runtime_state> result;
        for (const auto& row : rows) {
            auto& source = sources.emplace_back();
            source.template set<"node_id">(row[0].value().value_or(""));
            source.template set<"node_revision">(row[3].template as<std::int64_t>().value_or(0));
            source.template set<"reported_at">(row[4].value().value_or(""));
            const auto runtime = service::node_runtime::parse_stored(row[2].value().value_or("{}"),
                {.resource_ = c.pool()});
            if (!runtime) {
                continue;
            }
            for (const auto& health : runtime->origin_health_) {
                if (health.website_id_ == website_id) {
                    result.push_back({.node_id_ = std::string(row[0].value().value_or("")),
                        .node_name_ = std::string(row[1].value().value_or("")),
                        .health_ = health});
                }
            }
        }
        co_return result;
    }

    template <typename row_type>
    static void fill_website(auto& c, website_dto& item, const row_type& row,
        const service::website_config::website_config_data& config,
        const std::vector<bound_certificate>& certificates,
        const std::vector<origin_runtime_state>& origin_states = {}) {
        const auto runtime = service::website_dns::parse_stored(row[6].value().value_or("{}"),
            {.resource_ = c.pool()});
        if (!runtime) {
            throw_corrupt_config();
        }
        const auto target_count = row[7].template as<std::int64_t>().value_or(0);
        const auto synced_count = row[8].template as<std::int64_t>().value_or(0);
        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"cluster_id">(row[1].value().value_or(""));
        item.template set<"cluster_name">(row[2].value().value_or(""));
        item.template set<"access_domain">(row[3].value().value_or(""));
        item.template set<"status">(row[11].value().value_or(""));
        item.template set<"revision">(row[4].template as<std::int64_t>().value_or(1));
        item.template set<"config">(service::website_config::to_output(config, {.resource_ = c.pool()}));
        auto& certificate_dtos = item.template ensure<"certificates">();
        for (const auto& certificate : certificates) {
            auto& output = certificate_dtos.emplace_back();
            output.template set<"id">(certificate.id_);
            output.template set<"usable">(certificate.usable_);
            auto& domains = output.template ensure<"domains">();
            for (const auto& domain : certificate.domains_) {
                domains.emplace_back(domain, ruvia::model_options{.resource_ = c.pool()});
            }
        }
        item.template set<"runtime">(detail::to_runtime(c, config, *runtime, certificates, target_count,
            synced_count, origin_states));
        item.template set<"created_at">(row[9].value().value_or(""));
        item.template set<"updated_at">(row[10].value().value_or(""));
    }

    [[noreturn]] static void throw_corrupt_config() {
        service::common::throw_app_error(service::common::server_error_code, "聚合配置损坏", 500);
    }
};

inline const website_read_service& get_website_read_service() {
    static const website_read_service service;
    return service;
}

}  // namespace service::website
