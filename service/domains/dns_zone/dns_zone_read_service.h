#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "service/common/database.h"
#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/domains/dns_zone/dns_zone_error.h"
#include "service/domains/dns_zone/dns_zone_filter.h"
#include "service/domains/dns_zone/dns_zone_types.h"
#include "service/domains/provider/provider_reference.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/queue.h"

namespace service::dns_zone {

class dns_zone_read_service final {
public:
    ruvia::task<dns_zone_option_list_data_dto> options(auto& c, const std::string& tenant_id,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& owner_of,
        const std::optional<bool>& available,
        bool all = false) const {
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
        }
        std::optional<std::string> owner;
        if (owner_of) {
            owner = service::common::normalize_domain_name(*owner_of);
            if (owner->starts_with("*.")) {
                owner->erase(0, 2);
            }
        }
        ruvia::db_query query(c.pool());
        source(query, tenant_id);
        query.select(service::common::sql_columns(query,
            {"zone.id", "zone.domain", "provider.provider", "provider.name", "zone.sync_status",
                "zone.sync_status = 'synced' AND provider.status = 'verified'"}));
        if (pattern) {
            const auto value = query.value(*pattern);
            query.and_where(
                query.sql({"zone.domain ILIKE ", " OR provider.name ILIKE ", ""}, {value, value}));
        }
        if (owner) {
            const auto value = query.value(*owner);
            query.and_where(
                query.sql({"", " = zone.domain OR ", " LIKE '%.' || zone.domain"}, {value, value}));
        }
        if (available) {
            query.and_where(
                query.sql({"(zone.sync_status = 'synced' AND provider.status = 'verified') = ", ""},
                    {query.value(*available)}));
        }
        if (owner_of) {
            query.order_by(query.sql("length(zone.domain)"), ruvia::db_order_direction::desc);
            query.add_order_by(query.sql("zone.sort"), ruvia::db_order_direction::desc);
        } else {
            query.order_by(query.sql("zone.sort"), ruvia::db_order_direction::desc);
        }
        if (!all) {
            query.limit(20);
        }
        const auto rows = co_await c.db().query(query);
        dns_zone_option_list_data_dto result({.resource_ = c.pool()});
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            auto& item = items.emplace_back();
            item.template set<"id">(row[0].value().value_or(""));
            item.template set<"domain">(row[1].value().value_or(""));
            item.template set<"dns_provider">(row[2].value().value_or(""));
            item.template set<"dns_provider_name">(row[3].value().value_or(""));
            item.template set<"sync_status">(row[4].value().value_or("pending"));
            item.template set<"available">(row[5].template as<bool>().value_or(false));
        }
        co_return result;
    }

    ruvia::task<available_dns_zone_list_data_dto>
    available(auto& c, const std::string& tenant_id, const std::string& provider_id) const {
        auto db = c.db();
        const auto zones = co_await service::provider::verified_dns_provider_zones(db, tenant_id,
            provider_id, c.pool());
        if (!zones) {
            service::common::throw_app_error(dns_zone_error::provider_unavailable);
        }
        const auto local_rows = co_await c.db().query(
            "SELECT domain FROM sys_dns_zone WHERE tenant_id = $1 AND deleted_at IS NULL",
            tenant_id);
        std::unordered_set<std::string> managed;
        managed.reserve(local_rows.size());
        for (const auto& row : local_rows) {
            managed.emplace(row[0].value().value_or(""));
        }

        available_dns_zone_list_data_dto result({.resource_ = c.pool()});
        auto& items = result.template ensure<"list">();
        for (const auto& zone : *zones) {
            if (managed.contains(zone.domain_)) {
                continue;
            }
            auto& item = items.emplace_back();
            item.template set<"domain">(zone.domain_);
            item.template set<"status">(zone.status_);
        }
        co_return result;
    }

    ruvia::task<dns_zone_page_data_dto> list(auto& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size,
        std::int64_t skip,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& provider_id,
        const std::optional<std::string>& sync_status) const {
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
        }
        const auto filter = [&](ruvia::db_query& query) {
            source(query, tenant_id);
            if (provider_id) {
                query.and_where(query.sql({"provider.id = ", ""}, {query.value(*provider_id)}));
            }
            if (pattern) {
                query.and_where(query.sql({"zone.domain ILIKE ", ""}, {query.value(*pattern)}));
            }
            if (sync_status) {
                query.and_where(*sync_status == dns_zone_issue_filter
                                    ? dns_zone_issue_predicate(query, "zone")
                                    : query.sql({"zone.sync_status = ", ""}, {query.value(*sync_status)}));
            }
        };
        const auto page_rows = co_await service::common::query_page(c, filter, project, page_size, skip);
        const auto total = page_rows.total_;
        const auto& rows = page_rows.rows_;
        dns_zone_page_data_dto result({.resource_ = c.pool()});
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">(page_size > 0 ? (total + page_size - 1) / page_size : 0);
        auto& items = result.template ensure<"list">();
        auto db = c.db();
        std::vector<std::string> zone_ids;
        zone_ids.reserve(rows.size());
        for (const auto& row : rows) {
            zone_ids.emplace_back(row[0].value().value_or(""));
        }
        const auto projected_by_zone =
            co_await service::dns_sync::load_projected_records_by_zone(db, tenant_id, zone_ids);
        for (const auto& row : rows) {
            const auto& projected = projected_by_zone.at(std::string(row[0].value().value_or("")));
            fill_dns_zone(c, items.emplace_back(), row, projected);
        }
        co_return result;
    }

    ruvia::task<dns_zone_dto> get(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        ruvia::db_query query(c.pool());
        source(query, tenant_id);
        query.and_where(query.sql({"zone.id = ", ""}, {query.value(id)}));
        project(query);
        query.limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::throw_app_error(dns_zone_error::not_found);
        }
        auto db = c.db();
        const auto projected = co_await service::dns_sync::load_projected_records(db, tenant_id, id);
        dns_zone_dto result({.resource_ = c.pool()});
        fill_dns_zone(c, result, rows.front(), projected);
        co_return result;
    }

private:
    static void source(ruvia::db_query& query, const std::string& tenant_id) {
        query.from("sys_dns_zone", "zone")
            .join(ruvia::db_join_type::inner, "sys_provider",
                query.sql("provider.tenant_id = zone.tenant_id AND provider.id = zone.provider_id"),
                "provider")
            .where(query.sql({"zone.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(query.sql("zone.deleted_at IS NULL"))
            .and_where(service::provider::live_dns_provider_predicate(query, "provider"));
    }

    static void project(ruvia::db_query& query) {
        query
            .select(service::common::sql_columns(query,
                {"zone.id", "provider.id", "provider.provider", "provider.name", "zone.domain",
                    "zone.sync_status", "zone.revision", "zone.desired_revision", "zone.synced_revision",
                    "zone.config::text", "zone.runtime::text",
                    "TO_CHAR(zone.last_synced_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')", "zone.last_error",
                    "TO_CHAR(zone.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                    "TO_CHAR(zone.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                    "SELECT COUNT(DISTINCT claim.website_id) FROM sys_website_domain_claim claim "
                    "INNER JOIN sys_website website ON website.tenant_id = claim.tenant_id AND "
                    "website.id = claim.website_id WHERE claim.tenant_id = zone.tenant_id AND "
                    "claim.dns_zone_id = zone.id AND website.status = 'enabled' AND "
                    "website.deleted_at IS NULL"}))
            .order_by(query.sql("zone.sort"), ruvia::db_order_direction::desc);
    }

    template <typename dto_type, typename row_type>
    static void fill_dns_zone(auto& c, dto_type& item, const row_type& row,
        const std::vector<service::dns_sync::snapshot_record>& projected) {
        const auto config = service::dns_sync::parse_stored(row[9].value().value_or("{}"),
            {.resource_ = c.pool()});
        const auto runtime = service::dns_sync::parse_stored_runtime(row[10].value().value_or("{}"),
            {.resource_ = c.pool()});
        if (!config || !runtime) {
            throw_corrupt_config();
        }

        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"dns_provider_id">(row[1].value().value_or(""));
        item.template set<"dns_provider">(row[2].value().value_or(""));
        item.template set<"dns_provider_name">(row[3].value().value_or(""));
        item.template set<"domain">(row[4].value().value_or(""));
        item.template set<"sync_status">(row[5].value().value_or("pending"));
        item.template set<"revision">(row[6].template as<std::int64_t>().value_or(1));
        item.template set<"desired_revision">(row[7].template as<std::int64_t>().value_or(1));
        item.template set<"synced_revision">(row[8].template as<std::int64_t>().value_or(0));
        item.template set<"website_count">(row[15].template as<std::int64_t>().value_or(0));
        item.template set<"config">(to_config(c, *config));
        item.template set<"runtime">(to_public_runtime(c, *runtime, projected));
        item.template set<"created_at">(row[13].value().value_or(""));
        item.template set<"updated_at">(row[14].value().value_or(""));
        if (const auto& last_synced_at = row[11].value()) {
            item.template set<"last_synced_at">(*last_synced_at);
        }
        if (const auto& last_error = row[12].value()) {
            item.template set<"last_error">(*last_error);
        }
    }

    static service::dns_sync::zone_config_output
    to_config(auto& c, const service::dns_sync::zone_config_data& input) {
        return service::dns_sync::to_output(input, {.resource_ = c.pool()});
    }

    static void fill_runtime_lines(dns_zone_runtime_dto& output,
        const service::dns_sync::zone_runtime_data& input) {
        auto& lines = output.template ensure<"lines">();
        for (const auto& line : input.lines_) {
            if (!line.code_ || !line.name_) {
                continue;
            }
            auto& item = lines.emplace_back();
            item.template set<"code">(*line.code_);
            item.template set<"name">(*line.name_);
            item.template set<"display_name">(line.display_name_ ? *line.display_name_ : *line.name_);
            item.template set<"status">(line.status_ ? *line.status_ : std::string_view{"enabled"});
        }
    }

    static void
    fill_projected_records(dns_zone_runtime_dto& output,
        const std::vector<service::dns_sync::snapshot_record>& projected) {
        auto& projected_records = output.template ensure<"projected_records">();
        for (const auto& record : projected) {
            auto& item = projected_records.emplace_back();
            item.template set<"id">(record.id_);
            item.template set<"type">(record.type_);
            item.template set<"name">(record.name_);
            item.template set<"content">(record.content_);
            item.template set<"ttl">(record.ttl_);
            item.template set<"proxied">(record.proxied_);
            item.template set<"line_code">(record.line_code_);
            if (record.priority_) {
                item.template set<"priority">(*record.priority_);
            }
        }
    }

    static void fill_record_states(dns_zone_runtime_dto& output,
        const service::dns_sync::zone_runtime_data& input) {
        auto& states = output.template ensure<"record_states">();
        for (const auto& state : input.record_states_) {
            if (!state.id_) {
                continue;
            }
            auto& item = states.emplace_back();
            item.template set<"id">(*state.id_);
            item.template set<"sync_status">(state.sync_status_ ? *state.sync_status_
                                                                : std::string_view{"pending"});
            item.template set<"synced_revision">(state.synced_revision_ ? *state.synced_revision_ : 0);
            if (state.last_error_) {
                item.template set<"last_error">(*state.last_error_);
            }
        }
    }

    static dns_zone_runtime_dto
    to_public_runtime(auto& c, const service::dns_sync::zone_runtime_data& input,
        const std::vector<service::dns_sync::snapshot_record>& projected) {
        dns_zone_runtime_dto output({.resource_ = c.pool()});
        fill_runtime_lines(output, input);
        fill_projected_records(output, projected);
        fill_record_states(output, input);
        return output;
    }

    [[noreturn]] static void throw_corrupt_config() {
        service::common::throw_app_error(service::common::server_error_code, "聚合配置损坏", 500);
    }
};

inline const dns_zone_read_service& get_dns_zone_read_service() {
    static const dns_zone_read_service service;
    return service;
}

}  // namespace service::dns_zone
