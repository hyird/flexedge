#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/domains/website/website.error.h"
#include "service/domains/website/website.types.h"
#include "service/features/geoip/xdb_database.h"

namespace service::website {

class WebsiteDashboardService final {
  public:
    ruvia::Task<WebsiteDashboardDto> dashboard(ruvia::Context& c, const std::string& tenantId,
                                               const std::string& id) const {
        const auto website = co_await c.db().query(
            "SELECT 1 FROM sys_website WHERE tenant_id = $1 AND id = $2 AND deleted_at IS NULL "
            "LIMIT 1",
            tenantId, id);
        if (website.empty()) {
            service::common::throwAppError(WebsiteError::NOT_FOUND);
        }

        const auto summaryRows = co_await c.db().query(
            "WITH bounds AS (SELECT date_trunc('month', NOW() AT TIME ZONE 'Asia/Shanghai') "
            "AT TIME ZONE 'Asia/Shanghai' AS current_month_start, date_trunc('day', NOW() AT "
            "TIME ZONE 'Asia/Shanghai') AT TIME ZONE 'Asia/Shanghai' AS today_start), "
            "minute_buckets AS (SELECT date_trunc('minute', access.occurred_at) AS bucket, "
            "(SUM(access.response_bytes) * 8 / 60)::bigint AS bandwidth_bps FROM "
            "sys_website_access_log access, bounds WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= bounds.current_month_start - "
            "INTERVAL '1 month' GROUP BY date_trunc('minute', access.occurred_at)) SELECT "
            "COALESCE((SELECT MAX(bandwidth_bps) FROM minute_buckets, bounds WHERE bucket >= "
            "bounds.current_month_start - INTERVAL '1 month' AND bucket < "
            "bounds.current_month_start), 0)::bigint, COALESCE((SELECT MAX(bandwidth_bps) "
            "FROM minute_buckets, bounds WHERE bucket >= bounds.current_month_start), "
            "0)::bigint, COALESCE((SELECT MAX(bandwidth_bps) FROM minute_buckets, bounds "
            "WHERE bucket >= bounds.today_start), 0)::bigint, COALESCE((SELECT "
            "SUM(access.response_bytes) * 8 / 60 FROM sys_website_access_log access WHERE "
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= NOW() "
            "- INTERVAL '1 minute'), 0)::bigint, COALESCE((SELECT COUNT(DISTINCT "
            "access.client_ip) FROM sys_website_access_log access, bounds WHERE "
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "bounds.today_start AND access.client_ip IS NOT NULL), 0)::bigint, "
            "COALESCE((SELECT SUM(access.response_bytes) FROM sys_website_access_log access, "
            "bounds WHERE access.tenant_id = $1 AND access.website_id = $2 AND "
            "access.occurred_at >= bounds.today_start), 0)::bigint FROM bounds",
            tenantId, id);

        WebsiteDashboardDto result(c);
        auto& summary = result.ensure<"summary">();
        const auto& summaryRow = summaryRows.front();
        summary.set<"previousMonthPeakBps">(summaryRow[0].as<std::int64_t>().value_or(0));
        summary.set<"currentMonthPeakBps">(summaryRow[1].as<std::int64_t>().value_or(0));
        summary.set<"todayPeakBps">(summaryRow[2].as<std::int64_t>().value_or(0));
        summary.set<"currentBandwidthBps">(summaryRow[3].as<std::int64_t>().value_or(0));
        summary.set<"todayUniqueIps">(summaryRow[4].as<std::int64_t>().value_or(0));
        summary.set<"todayResponseBytes">(summaryRow[5].as<std::int64_t>().value_or(0));

        const auto hourlyRows = co_await c.db().query(
            "WITH buckets AS (SELECT generate_series(date_trunc('hour', NOW()) - INTERVAL "
            "'23 hours', date_trunc('hour', NOW()), INTERVAL '1 hour') AS bucket) SELECT "
            "TO_CHAR(buckets.bucket, 'YYYY-MM-DD\"T\"HH24:MI:SSOF'), COUNT(access.id)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint, COALESCE(SUM(access.response_bytes) "
            "* 8 / 3600, 0)::bigint FROM buckets LEFT JOIN sys_website_access_log access ON "
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "buckets.bucket AND access.occurred_at < buckets.bucket + INTERVAL '1 hour' GROUP "
            "BY buckets.bucket ORDER BY buckets.bucket",
            tenantId, id);
        auto& hourly = result.ensure<"hourly">();
        appendBuckets(c, hourly, hourlyRows);

        const auto dailyRows = co_await c.db().query(
            "WITH buckets AS (SELECT generate_series(date_trunc('day', NOW()) - INTERVAL "
            "'14 days', date_trunc('day', NOW()), INTERVAL '1 day') AS bucket) SELECT "
            "TO_CHAR(buckets.bucket, 'YYYY-MM-DD\"T\"HH24:MI:SSOF'), COUNT(access.id)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint, COALESCE(SUM(access.response_bytes) "
            "* 8 / 86400, 0)::bigint FROM buckets LEFT JOIN sys_website_access_log access ON "
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "buckets.bucket AND access.occurred_at < buckets.bucket + INTERVAL '1 day' GROUP "
            "BY buckets.bucket ORDER BY buckets.bucket",
            tenantId, id);
        auto& daily = result.ensure<"daily">();
        appendBuckets(c, daily, dailyRows);

        const auto& geoDatabase = service::geoip::xdbDatabase();
        if (geoDatabase.available()) {
            const auto countryIpRows = co_await c.db().query(
                "SELECT access.client_ip::text, COUNT(*)::bigint, "
                "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log "
                "access WHERE access.tenant_id = $1 AND access.website_id = $2 AND "
                "access.occurred_at >= NOW() - INTERVAL '24 hours' AND access.client_ip IS NOT "
                "NULL GROUP BY access.client_ip",
                tenantId, id);
            appendCountries(c, result.ensure<"countries">(), countryIpRows, geoDatabase);
        }
        co_await appendRanking(
            c, result.ensure<"statusCodes">(),
            "SELECT access.status_code::text, COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), "
            "0)::bigint FROM sys_website_access_log access WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= NOW() - INTERVAL '24 hours' GROUP "
            "BY access.status_code ORDER BY COUNT(*) DESC, access.status_code ASC LIMIT 10",
            tenantId, id);
        co_await appendRanking(
            c, result.ensure<"methods">(),
            "SELECT access.method, COUNT(*)::bigint, COALESCE(SUM(access.response_bytes), "
            "0)::bigint FROM sys_website_access_log access WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= NOW() - INTERVAL '24 hours' GROUP "
            "BY access.method ORDER BY COUNT(*) DESC, access.method ASC LIMIT 10",
            tenantId, id);
        co_await appendRanking(
            c, result.ensure<"hosts">(),
            "SELECT access.host, COUNT(*)::bigint, COALESCE(SUM(access.response_bytes), "
            "0)::bigint FROM sys_website_access_log access WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= NOW() - INTERVAL '24 hours' GROUP "
            "BY access.host ORDER BY COUNT(*) DESC, access.host ASC LIMIT 10",
            tenantId, id);
        co_await appendRanking(
            c, result.ensure<"referers">(),
            "SELECT COALESCE(NULLIF(access.referer, ''), '直接访问'), COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log access "
            "WHERE access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "NOW() - INTERVAL '24 hours' GROUP BY 1 ORDER BY COUNT(*) DESC, 1 ASC LIMIT 10",
            tenantId, id);
        co_await appendRanking(
            c, result.ensure<"paths">(),
            "SELECT split_part(access.target, '?', 1), COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), "
            "0)::bigint FROM sys_website_access_log access WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= NOW() - INTERVAL '24 hours' GROUP "
            "BY 1 ORDER BY COUNT(*) DESC, 1 ASC LIMIT 10",
            tenantId, id);
        co_await appendClientIpRanking(
            c, result.ensure<"clientIpsByBytes">(), geoDatabase,
            "SELECT COALESCE(access.client_ip::text, '未知 IP'), COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log access "
            "WHERE access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "NOW() - INTERVAL '24 hours' GROUP BY 1 ORDER BY 3 DESC, 2 DESC, 1 ASC LIMIT 10",
            tenantId, id);
        co_await appendClientIpRanking(
            c, result.ensure<"clientIpsByRequests">(), geoDatabase,
            "SELECT COALESCE(access.client_ip::text, '未知 IP'), COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log access "
            "WHERE access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "NOW() - INTERVAL '24 hours' GROUP BY 1 ORDER BY 2 DESC, 3 DESC, 1 ASC LIMIT 10",
            tenantId, id);
        co_return result;
    }

  private:
    template <typename Output, typename Rows>
    static void appendBuckets(ruvia::Context& c, Output& output, const Rows& rows) {
        for (const auto& row : rows) {
            auto& item = output.emplace_back(c);
            item.template set<"timestamp">(row[0].value().value_or(""));
            item.template set<"requestCount">(row[1].template as<std::int64_t>().value_or(0));
            item.template set<"responseBytes">(row[2].template as<std::int64_t>().value_or(0));
            item.template set<"bandwidthBps">(row[3].template as<std::int64_t>().value_or(0));
        }
    }

    template <typename Output, typename Rows>
    static void appendRankings(ruvia::Context& c, Output& output, const Rows& rows) {
        for (const auto& row : rows) {
            auto& item = output.emplace_back(c);
            item.template set<"label">(row[0].value().value_or(""));
            item.template set<"requestCount">(row[1].template as<std::int64_t>().value_or(0));
            item.template set<"responseBytes">(row[2].template as<std::int64_t>().value_or(0));
        }
    }

    template <typename Output>
    static ruvia::Task<void> appendRanking(ruvia::Context& c, Output& output, std::string_view sql,
                                           std::string_view tenantId, std::string_view websiteId) {
        const auto rows = co_await c.db().query(sql, tenantId, websiteId);
        appendRankings(c, output, rows);
        co_return;
    }

    template <typename Output>
    static ruvia::Task<void> appendClientIpRanking(ruvia::Context& c, Output& output,
                                                   const service::geoip::XdbDatabase& geoDatabase,
                                                   std::string_view sql, std::string_view tenantId,
                                                   std::string_view websiteId) {
        const auto rows = co_await c.db().query(sql, tenantId, websiteId);
        for (const auto& row : rows) {
            std::string label{row[0].value().value_or("")};
            if (const auto location = geoDatabase.lookup(label)) {
                label += " · ";
                label += location->display;
            } else if (label != "未知 IP") {
                label += " · 未知地区";
            }
            auto& item = output.emplace_back(c);
            item.template set<"label">(label);
            item.template set<"requestCount">(row[1].template as<std::int64_t>().value_or(0));
            item.template set<"responseBytes">(row[2].template as<std::int64_t>().value_or(0));
        }
        co_return;
    }

    template <typename Output, typename Rows>
    static void appendCountries(ruvia::Context& c, Output& output, const Rows& rows,
                                const service::geoip::XdbDatabase& geoDatabase) {
        struct CountryTotal final {
            std::int64_t requestCount{};
            std::int64_t responseBytes{};
        };
        std::unordered_map<std::string, CountryTotal> countries;
        for (const auto& row : rows) {
            const auto ip = row[0].value();
            if (!ip) {
                continue;
            }
            const auto country = geoDatabase.country(*ip);
            if (!country) {
                continue;
            }
            auto& total = countries[*country];
            total.requestCount += row[1].template as<std::int64_t>().value_or(0);
            total.responseBytes += row[2].template as<std::int64_t>().value_or(0);
        }
        std::vector<std::pair<std::string, CountryTotal>> orderedCountries;
        orderedCountries.reserve(countries.size());
        for (const auto& country : countries) {
            orderedCountries.emplace_back(country.first, country.second);
        }
        std::ranges::sort(orderedCountries, [](const auto& left, const auto& right) {
            if (left.second.requestCount != right.second.requestCount) {
                return left.second.requestCount > right.second.requestCount;
            }
            if (left.second.responseBytes != right.second.responseBytes) {
                return left.second.responseBytes > right.second.responseBytes;
            }
            return left.first < right.first;
        });
        const auto count = std::min<std::size_t>(orderedCountries.size(), 10);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& country = orderedCountries[index];
            auto& item = output.emplace_back(c);
            item.template set<"label">(country.first);
            item.template set<"requestCount">(country.second.requestCount);
            item.template set<"responseBytes">(country.second.responseBytes);
        }
    }
};

inline const WebsiteDashboardService& websiteDashboardService() {
    static const WebsiteDashboardService service;
    return service;
}

} // namespace service::website
