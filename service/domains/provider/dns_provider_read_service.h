#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/provider/dns_provider_error.h"
#include "service/domains/provider/dns_provider_types.h"
#include "service/domains/provider/provider_reference.h"
#include "service/features/dns/provider_config_mapper.h"

namespace service::provider {

class dns_provider_read_service final {
public:
    ruvia::task<ruvia::array<dns_provider_dto>> options(auto& c, const std::string& tenant_id, const std::optional<std::string>& keyword, const std::optional<std::string>& status) const {
        ruvia::db_query query(c.pool());
        filter_query(query, tenant_id, keyword, status);
        project(query);
        const auto rows = co_await c.db().query(query);
        ruvia::array<dns_provider_dto> items({.resource_ = c.pool()});
        for (const auto& row : rows) {
            fill(items.emplace_back(ruvia::model_options{.resource_ = c.pool()}), parse_row(c, row));
        }
        co_return items;
    }

    ruvia::task<dns_provider_page_data_dto> list(auto& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size,
        std::int64_t skip,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& status) const {
        const auto filter = [&](ruvia::db_query& query) { filter_query(query, tenant_id, keyword, status); };
        const auto page_rows = co_await service::common::query_page(c, filter, project, page_size, skip);
        const auto total = page_rows.total_;

        dns_provider_page_data_dto result({.resource_ = c.pool()});
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">(page_size > 0 ? (total + page_size - 1) / page_size : 0);
        auto& items = result.template ensure<"list">();
        for (const auto& row : page_rows.rows_) {
            fill(items.emplace_back(ruvia::model_options{.resource_ = c.pool()}), parse_row(c, row));
        }
        co_return result;
    }

    ruvia::task<dns_provider_dto> get(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        ruvia::db_query query(c.pool());
        source(query, tenant_id);
        query.and_where(query.sql({"provider.id = ", ""}, {query.value(id)}));
        project(query);
        query.limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::throw_app_error(dns_provider_error::not_found);
        }
        dns_provider_dto result({.resource_ = c.pool()});
        fill(result, parse_row(c, rows.front()));
        co_return result;
    }

private:
    static void filter_query(ruvia::db_query& query, const std::string& tenant_id, const std::optional<std::string>& keyword, const std::optional<std::string>& status) {
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
        }
        source(query, tenant_id);
        if (status) {
            query.and_where(query.sql({"provider.status = ", ""}, {query.value(*status)}));
        }
        if (pattern) {
            const auto value = query.value(*pattern);
            query.and_where(query.sql(
                {"provider.name ILIKE ", " OR provider.account_id ILIKE ", ""}, {value, value}));
        }
    }

    struct stored_provider final {
        std::string id_;
        std::string provider_;
        std::int64_t revision_{};
        std::string name_;
        std::string account_id_;
        std::string hint_;
        std::string status_;
        std::optional<std::string> last_verified_at_;
        std::optional<std::string> last_error_;
        std::string created_at_;
        std::string updated_at_;
        std::int64_t zone_count_{};
    };

    static void source(ruvia::db_query& query, const std::string& tenant_id) {
        query.from("sys_provider", "provider")
            .where(query.sql({"provider.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(live_dns_provider_predicate(query, "provider"));
    }

    static void project(ruvia::db_query& query) {
        query
            .select(service::common::sql_columns(query,
                {"provider.id", "provider.provider", "provider.revision", "provider.name",
                    "provider.account_id", "provider.config::text", "provider.status",
                    "TO_CHAR(provider.last_verified_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                    "provider.last_error", "TO_CHAR(provider.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                    "TO_CHAR(provider.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                    "SELECT COUNT(*) FROM sys_dns_zone zone WHERE zone.tenant_id = provider.tenant_id "
                    "AND zone.provider_id = provider.id AND zone.deleted_at IS NULL"}))
            .order_by(query.sql("provider.sort"), ruvia::db_order_direction::desc);
    }

    template <typename row_type>
    static stored_provider parse_row(auto& c, const row_type& row) {
        const auto config =
            service::dns::parse_dns_provider_config(row[5].value().value_or("{}"), c.pool());
        return {
            .id_ = std::string(row[0].value().value_or("")),
            .provider_ = std::string(row[1].value().value_or("")),
            .revision_ = row[2].template as<std::int64_t>().value_or(0),
            .name_ = std::string(row[3].value().value_or("")),
            .account_id_ = std::string(row[4].value().value_or("")),
            .hint_ = config.credential_hint_,
            .status_ = std::string(row[6].value().value_or("")),
            .last_verified_at_ = optional_string(row[7]),
            .last_error_ = optional_string(row[8]),
            .created_at_ = std::string(row[9].value().value_or("")),
            .updated_at_ = std::string(row[10].value().value_or("")),
            .zone_count_ = row[11].template as<std::int64_t>().value_or(0),
        };
    }

    template <typename value_type>
    static std::optional<std::string> optional_string(const value_type& value) {
        if (const auto result = value.value()) {
            return std::string(*result);
        }
        return std::nullopt;
    }

    static void fill(dns_provider_dto& item, const stored_provider& provider) {
        item.template set<"id">(provider.id_);
        item.template set<"revision">(provider.revision_);
        item.template set<"name">(provider.name_);
        item.template set<"account_id">(provider.account_id_);
        item.template set<"provider">(provider.provider_);
        item.template set<"token_hint">(provider.hint_);
        item.template set<"status">(provider.status_);
        item.template set<"created_at">(provider.created_at_);
        item.template set<"updated_at">(provider.updated_at_);
        item.template set<"zone_count">(provider.zone_count_);
        if (provider.last_verified_at_) {
            item.template set<"last_verified_at">(*provider.last_verified_at_);
        }
        if (provider.last_error_) {
            item.template set<"last_error">(*provider.last_error_);
        }
    }
};

inline const dns_provider_read_service& get_dns_provider_read_service() {
    static const dns_provider_read_service service;
    return service;
}

}  // namespace service::provider
