#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/website/website_access_log_mapper.h"
#include "service/domains/website/website_error.h"
#include "service/domains/website/website_types.h"
#include "service/features/log_ingest/tail_cursor.h"

namespace service::website {

class website_access_log_service final {
public:
    ruvia::task<website_access_log_tail_data_dto>
    tail(auto& c, const std::string& tenant_id, const std::string& website_id,
        std::int64_t limit, const std::optional<service::log_ingest::tail_cursor>& after) const {
        ruvia::db_query entries(c.pool());
        entries
            .select({entries.star("entry"),
                entries.alias(entries.sql("COALESCE(node.name, '')"), "node_name"),
                entries.alias(
                    entries.sql("ROUND(EXTRACT(EPOCH FROM entry.created_at) * 1000000)::bigint"),
                    "ingested_unix_micros")})
            .from("sys_website_access_log", "entry")
            .join(ruvia::db_join_type::left, "sys_node",
                entries.sql("node.tenant_id = entry.tenant_id AND node.id = entry.node_id"), "node")
            .where(entries.sql("entry.tenant_id = website.tenant_id AND entry.website_id = website.id"));
        service::log_ingest::apply_tail_cursor_window(entries, "entry", limit, after);
        ruvia::db_query query(c.pool());
        query
            .select(service::common::sql_columns(query,
                {"log.id", "TO_CHAR(log.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')", "log.node_id",
                    "log.node_name", "log.client_ip::text", "log.protocol", "log.method", "log.host",
                    "log.target", "log.status_code", "log.request_bytes", "log.response_bytes",
                    "log.duration_ms", "log.user_agent", "log.referer", "log.request_headers",
                    "log.request_body", "log.request_body_truncated", "log.tls_fingerprint",
                    "log.response_headers", "log.query_string", "log.cookies",
                    "log.cache_status", "log.cache_reason", "log.ingested_unix_micros"}))
            .from("sys_website", "website")
            .join(ruvia::db_join_type::left, entries, query.sql("TRUE"), "log", {.lateral_ = true})
            .where(query.sql({"website.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(query.sql({"website.id = ", ""}, {query.value(website_id)}))
            .and_where(query.sql("website.deleted_at IS NULL"))
            .order_by(query.sql("log.created_at"), ruvia::db_order_direction::desc,
                ruvia::db_nulls_order::last)
            .add_order_by(query.sql("log.id"), ruvia::db_order_direction::desc);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::throw_app_error(website_error::not_found);
        }

        website_access_log_tail_data_dto result({.resource_ = c.pool()});
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            if (!row[0].value()) {
                continue;
            }
            fill_website_access_log(items.emplace_back(), row);
        }
        if (!items.empty()) {
            result.template set<"cursor">(service::log_ingest::encode_tail_cursor(
                rows.front()[24].template as<std::int64_t>().value_or(0),
                rows.front()[0].value().value_or("")));
        }
        co_return result;
    }

    ruvia::task<website_access_log_page_data_dto>
    history(auto& c, const std::string& tenant_id, const std::string& website_id,
        std::int64_t page, std::int64_t page_size, std::int64_t skip,
        const std::optional<std::string>& keyword, const std::optional<std::string>& method,
        const std::optional<std::string>& status_class) const {
        const auto website = co_await c.db().query(
            "SELECT 1 FROM sys_website WHERE tenant_id = $1 AND id = $2 AND deleted_at IS NULL "
            "LIMIT 1",
            tenant_id, website_id);
        if (website.empty()) {
            service::common::throw_app_error(website_error::not_found);
        }

        std::optional<std::string> keyword_pattern;
        if (keyword) {
            keyword_pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
        }
        const auto filter = [&](ruvia::db_query& query) {
            query.from("sys_website_access_log", "access")
                .join(ruvia::db_join_type::left, "sys_node",
                    query.sql("node.tenant_id = access.tenant_id AND node.id = access.node_id"), "node")
                .where(query.sql({"access.tenant_id = ", ""}, {query.value(tenant_id)}))
                .and_where(query.sql({"access.website_id = ", ""}, {query.value(website_id)}));
            if (keyword_pattern) {
                const auto pattern = query.value(*keyword_pattern);
                query.and_where(query.sql({"access.host ILIKE ", " OR access.target ILIKE ",
                                              " OR access.method ILIKE ", " OR access.client_ip::text ILIKE ",
                                              " OR COALESCE(node.name, '') ILIKE ", ""},
                    {pattern, pattern, pattern, pattern, pattern}));
            }
            if (method) {
                query.and_where(query.sql({"access.method = ", ""}, {query.value(*method)}));
            }
            if (status_class) {
                const auto minimum = static_cast<std::int64_t>((*status_class)[0] - '0') * 100;
                query.and_where(query.sql({"access.status_code >= ", ""}, {query.value(minimum)}))
                    .and_where(query.sql({"access.status_code < ", ""}, {query.value(minimum + 100)}));
            }
        };
        const auto project = [](ruvia::db_query& query) {
            query
                .select(service::common::sql_columns(query,
                    {"access.id", "TO_CHAR(access.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                        "access.node_id", "COALESCE(node.name, '')", "access.client_ip::text",
                        "access.protocol", "access.method", "access.host", "access.target",
                        "access.status_code", "access.request_bytes", "access.response_bytes",
                        "access.duration_ms", "access.user_agent", "access.referer",
                        "access.request_headers", "access.request_body", "access.request_body_truncated",
                        "access.tls_fingerprint", "access.response_headers", "access.query_string",
                        "access.cookies", "access.cache_status", "access.cache_reason"}))
                .order_by(query.sql("access.created_at"), ruvia::db_order_direction::desc)
                .add_order_by(query.sql("access.id"), ruvia::db_order_direction::desc);
        };
        const auto page_rows = co_await service::common::query_page(c, filter, project, page_size, skip);
        const auto& rows = page_rows.rows_;
        auto result = service::common::init_page_dto<website_access_log_page_data_dto>(
            c.pool(), page_rows.total_, page, page_size);
        auto& items = result.template ensure<"list">();
        items.reserve(rows.size());
        for (const auto& row : rows) {
            fill_website_access_log(items.emplace_back(), row);
        }
        co_return result;
    }
};

inline const website_access_log_service& get_website_access_log_service() {
    static const website_access_log_service service;
    return service;
}

}  // namespace service::website
