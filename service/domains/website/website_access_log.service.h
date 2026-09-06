#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/domains/website/website_access_log.mapper.h"
#include "service/domains/website/website.error.h"
#include "service/domains/website/website.types.h"
#include "service/features/log_ingest/tail.h"

namespace service::website {

class WebsiteAccessLogService final {
  public:
    ruvia::Task<WebsiteAccessLogTailDataDto>
    tail(ruvia::Context& c, const std::string& tenantId, const std::string& websiteId,
         std::int64_t limit, const std::optional<service::log_ingest::TailCursor>& after) const {
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}, ruvia::DbValue{websiteId}};
        std::string cursorPredicate;
        if (after) {
            params.emplace_back(after->ingestedUnixMicros);
            params.emplace_back(std::string_view{after->id});
            cursorPredicate =
                " AND (access.created_at, access.id) > (TIMESTAMPTZ 'epoch' + $3::bigint * "
                "INTERVAL '1 microsecond', $4::uuid)";
        }
        params.emplace_back(limit);
        const auto limitParameter = "$" + std::to_string(params.size());
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
                cursorPredicate + " ORDER BY access.created_at DESC, access.id DESC LIMIT " +
                limitParameter +
                ") log ON TRUE WHERE website.tenant_id = $1 AND website.id = $2 AND "
                "website.deleted_at IS NULL ORDER BY log.created_at DESC NULLS LAST, log.id DESC",
            params);
        if (rows.empty()) {
            service::common::throwAppError(WebsiteError::NOT_FOUND);
        }

        WebsiteAccessLogTailDataDto result(c);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            if (!row[0].value()) {
                continue;
            }
            fillWebsiteAccessLog(items.emplace_back(c), row);
        }
        if (!items.empty()) {
            result.set<"cursor">(service::log_ingest::encodeTailCursor(
                rows.front()[22].as<std::int64_t>().value_or(0),
                rows.front()[0].value().value_or("")));
        }
        co_return result;
    }

    ruvia::Task<WebsiteAccessLogPageDataDto>
    history(ruvia::Context& c, const std::string& tenantId, const std::string& websiteId,
            std::int64_t page, std::int64_t pageSize, std::int64_t skip,
            const std::optional<std::string>& keyword, const std::optional<std::string>& method,
            const std::optional<std::string>& statusClass) const {
        const auto website = co_await c.db().query(
            "SELECT 1 FROM sys_website WHERE tenant_id = $1 AND id = $2 AND deleted_at IS NULL "
            "LIMIT 1",
            tenantId, websiteId);
        if (website.empty()) {
            service::common::throwAppError(WebsiteError::NOT_FOUND);
        }

        std::string where = " FROM sys_website_access_log access LEFT JOIN sys_node node ON "
                            "node.tenant_id = access.tenant_id AND node.id = access.node_id WHERE "
                            "access.tenant_id = $1 AND access.website_id = $2";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}, ruvia::DbValue{websiteId}};
        std::optional<std::string> keywordPattern;
        if (keyword) {
            keywordPattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where += " AND (access.host ILIKE " + placeholder + " OR access.target ILIKE " +
                     placeholder + " OR access.method ILIKE " + placeholder +
                     " OR access.client_ip::text ILIKE " + placeholder +
                     " OR COALESCE(node.name, '') ILIKE " + placeholder + ")";
            params.emplace_back(std::string_view(*keywordPattern));
        }
        if (method) {
            where += " AND access.method = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*method));
        }
        if (statusClass) {
            const auto minimum = static_cast<std::int64_t>((*statusClass)[0] - '0') * 100;
            where += " AND access.status_code >= $" + std::to_string(params.size() + 1);
            params.emplace_back(minimum);
            where += " AND access.status_code < $" + std::to_string(params.size() + 1);
            params.emplace_back(minimum + 100);
        }

        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows = co_await c.db().query(
            "SELECT access.id, TO_CHAR(access.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "access.node_id, COALESCE(node.name, ''), access.client_ip::text, access.protocol, "
            "access.method, access.host, access.target, access.status_code, access.request_bytes, "
            "access.response_bytes, access.duration_ms, access.user_agent, access.referer, "
            "access.request_headers, access.request_body, access.request_body_truncated, "
            "access.tls_fingerprint, access.response_headers, access.query_string, access.cookies" +
                where + " ORDER BY access.created_at DESC, access.id DESC LIMIT " +
                std::to_string(pageSize) + " OFFSET " + std::to_string(skip),
            params);
        WebsiteAccessLogPageDataDto result(c);
        auto& items = result.ensure<"list">();
        items.reserve(rows.size());
        for (const auto& row : rows) {
            fillWebsiteAccessLog(items.emplace_back(c), row);
        }
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        co_return result;
    }
};

inline const WebsiteAccessLogService& websiteAccessLogService() {
    static const WebsiteAccessLogService service;
    return service;
}

} // namespace service::website
