#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <string>

#include "service/common/database.h"
#include "service/domains/provider/certificate_provider_types.h"
#include "service/features/certificate/provider_config_mapper.h"

namespace service::provider {

class certificate_provider_read_service final {
public:
    ruvia::task<ruvia::array<certificate_provider_dto>> options(auto& c, const std::string& tenant_id, const std::optional<std::string>& keyword) const {
        ruvia::db_query query(c.pool());
        filter_query(query, tenant_id, keyword);
        project(query);
        const auto rows = co_await c.db().query(query);
        ruvia::array<certificate_provider_dto> items({.resource_ = c.pool()});
        for (const auto& row : rows) {
            fill(c, items.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row);
        }
        co_return items;
    }

    ruvia::task<certificate_provider_page_data_dto> list(auto& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size, std::int64_t skip,
        const std::optional<std::string>& keyword) const {
        const auto filter = [&](ruvia::db_query& query) { filter_query(query, tenant_id, keyword); };
        const auto page_rows = co_await service::common::query_page(c, filter, project, page_size, skip);
        auto result = service::common::init_page_dto<certificate_provider_page_data_dto>(
            c.pool(), page_rows.total_, page, page_size);
        auto& items = result.template ensure<"list">();
        for (const auto& row : page_rows.rows_) {
            fill(c, items.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row);
        }
        co_return result;
    }

private:
    static void filter_query(ruvia::db_query& query, const std::string& tenant_id, const std::optional<std::string>& keyword) {
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
        }
        source(query, tenant_id);
        if (pattern) {
            const auto value = query.value(*pattern);
            query.and_where(query.sql(
                {"provider.provider ILIKE ",
                    " OR (CASE provider.provider WHEN 'letsencrypt' THEN 'Let''s Encrypt' "
                    "WHEN 'zerossl' THEN 'ZeroSSL' ELSE provider.provider END) ILIKE ",
                    " OR provider.config->>'account_email' ILIKE ",
                    " OR provider.config->'access_key'->>'hint' ILIKE ", ""},
                {value, value, value, value}));
        }
    }

    static void source(ruvia::db_query& query, const std::string& tenant_id) {
        query.from("sys_provider", "provider")
            .where(query.sql({"provider.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(query.sql("provider.kind = 'certificate'"))
            .and_where(query.sql("provider.deleted_at IS NULL"));
    }

    static void project(ruvia::db_query& query) {
        query
            .select(service::common::sql_columns(query,
                {"provider.id", "provider.revision", "provider.provider", "provider.config::text",
                    "provider.status",
                    "TO_CHAR(provider.last_verified_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                    "provider.last_error",
                    "TO_CHAR(provider.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                    "TO_CHAR(provider.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')"}))
            .order_by(query.sql("provider.sort"), ruvia::db_order_direction::desc);
    }

    template <typename row_type>
    static void fill(auto& c, certificate_provider_dto& item, const row_type& row) {
        const auto config = service::certificate_issuance::parse_certificate_provider_config(
            row[3].value().value_or("{}"), c.pool());
        item.set<"id">(row[0].value().value_or(""));
        item.set<"revision">(row[1].template as<std::int64_t>().value_or(1));
        item.set<"provider">(row[2].value().value_or(""));
        item.set<"credential_mode">(config.credential_mode_);
        item.set<"status">(row[4].value().value_or(""));
        item.set<"created_at">(row[7].value().value_or(""));
        item.set<"updated_at">(row[8].value().value_or(""));
        if (config.account_email_) {
            item.set<"account_email">(*config.account_email_);
        }
        if (config.hint_) {
            item.set<"access_key_hint">(*config.hint_);
        }
        if (const auto& last_verified_at = row[5].value()) {
            item.set<"last_verified_at">(*last_verified_at);
        }
        if (const auto& last_error = row[6].value()) {
            item.set<"last_error">(*last_error);
        }
    }
};

inline const certificate_provider_read_service& get_certificate_provider_read_service() {
    static const certificate_provider_read_service service;
    return service;
}

}  // namespace service::provider
