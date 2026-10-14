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
        std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}, ruvia::db_value{website_id}};
        std::string cursor_predicate;
        if (after) {
            params.emplace_back(after->ingested_unix_micros_);
            params.emplace_back(std::string_view{after->id_});
            cursor_predicate =
                " AND (access.created_at, access.id) > (TIMESTAMPTZ 'epoch' + $3::bigint * "
                "INTERVAL '1 microsecond', $4::uuid)";
        }
        params.emplace_back(limit);
        const auto limit_parameter = "$" + std::to_string(params.size());
        const auto rows = co_await c.db().query(
            "SELECT log.id, TO_CHAR(log.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "log.node_id, log.node_name, log.client_ip::text, log.protocol, log.method, log.host, "
            "log.target, log.status_code, log.request_bytes, log.response_bytes, log.duration_ms, "
            "log.user_agent, log.referer, log.request_headers, log.request_body, "
            "log.request_body_truncated, log.tls_fingerprint, log.response_headers, "
            "log.query_string, log.cookies, "
            "log.ingested_unix_micros FROM sys_website website LEFT JOIN LATERAL (SELECT "
            "access.*, COALESCE(node.name, '') AS node_name, "
            "ROUND(EXTRACT(EPOCH FROM access.created_at) * 1000000)::bigint AS "
            "ingested_unix_micros FROM sys_website_access_log access LEFT JOIN sys_node node ON "
            "node.tenant_id = access.tenant_id AND node.id = access.node_id WHERE "
            "access.tenant_id = website.tenant_id AND access.website_id = website.id" +
                cursor_predicate + " ORDER BY access.created_at DESC, access.id DESC LIMIT " +
                limit_parameter +
                ") log ON TRUE WHERE website.tenant_id = $1 AND website.id = $2 AND "
                "website.deleted_at IS NULL ORDER BY log.created_at DESC NULLS LAST, log.id DESC",
            params);
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
                rows.front()[22].template as<std::int64_t>().value_or(0),
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

        std::string where =
            " FROM sys_website_access_log access LEFT JOIN sys_node node ON "
            "node.tenant_id = access.tenant_id AND node.id = access.node_id WHERE "
            "access.tenant_id = $1 AND access.website_id = $2";
        std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}, ruvia::db_value{website_id}};
        std::optional<std::string> keyword_pattern;
        if (keyword) {
            keyword_pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where += " AND (access.host ILIKE " + placeholder + " OR access.target ILIKE " +
                     placeholder + " OR access.method ILIKE " + placeholder +
                     " OR access.client_ip::text ILIKE " + placeholder +
                     " OR COALESCE(node.name, '') ILIKE " + placeholder + ")";
            params.emplace_back(std::string_view(*keyword_pattern));
        }
        if (method) {
            where += " AND access.method = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*method));
        }
        if (status_class) {
            const auto minimum = static_cast<std::int64_t>((*status_class)[0] - '0') * 100;
            where += " AND access.status_code >= $" + std::to_string(params.size() + 1);
            params.emplace_back(minimum);
            where += " AND access.status_code < $" + std::to_string(params.size() + 1);
            params.emplace_back(minimum + 100);
        }

        const auto count_rows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = count_rows.empty() ? std::int64_t{0}
                                              : count_rows.front()[0].template as<std::int64_t>().value_or(0);
        const auto rows = co_await c.db().query(
            "SELECT access.id, TO_CHAR(access.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "access.node_id, COALESCE(node.name, ''), access.client_ip::text, access.protocol, "
            "access.method, access.host, access.target, access.status_code, access.request_bytes, "
            "access.response_bytes, access.duration_ms, access.user_agent, access.referer, "
            "access.request_headers, access.request_body, access.request_body_truncated, "
            "access.tls_fingerprint, access.response_headers, access.query_string, access.cookies" +
                where + " ORDER BY access.created_at DESC, access.id DESC LIMIT " +
                std::to_string(page_size) + " OFFSET " + std::to_string(skip),
            params);
        website_access_log_page_data_dto result({.resource_ = c.pool()});
        auto& items = result.template ensure<"list">();
        items.reserve(rows.size());
        for (const auto& row : rows) {
            fill_website_access_log(items.emplace_back(), row);
        }
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">(page_size > 0 ? (total + page_size - 1) / page_size : 0);
        co_return result;
    }
};

inline const website_access_log_service& get_website_access_log_service() {
    static const website_access_log_service service;
    return service;
}

}  // namespace service::website
