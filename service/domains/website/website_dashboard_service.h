#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/common/http.h"
#include "service/domains/website/website_error.h"
#include "service/domains/website/website_types.h"
#include "service/features/geoip/runtime_database.h"

namespace service::website {

class website_dashboard_service final {
public:
    ruvia::task<website_dashboard_dto> dashboard(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        const auto website = co_await c.db().query(
            "SELECT 1 FROM sys_website WHERE tenant_id = $1 AND id = $2 AND deleted_at IS NULL "
            "LIMIT 1",
            tenant_id, id);
        if (website.empty()) {
            service::common::throw_app_error(website_error::not_found);
        }

        const auto summary_rows = co_await c.db().query(
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
            tenant_id, id);

        website_dashboard_dto result({.resource_ = c.pool()});
        auto& summary = result.template ensure<"summary">();
        const auto& summary_row = summary_rows.front();
        summary.template set<"previous_month_peak_bps">(summary_row[0].template as<std::int64_t>().value_or(0));
        summary.template set<"current_month_peak_bps">(summary_row[1].template as<std::int64_t>().value_or(0));
        summary.template set<"today_peak_bps">(summary_row[2].template as<std::int64_t>().value_or(0));
        summary.template set<"current_bandwidth_bps">(summary_row[3].template as<std::int64_t>().value_or(0));
        summary.template set<"today_unique_ips">(summary_row[4].template as<std::int64_t>().value_or(0));
        summary.template set<"today_response_bytes">(summary_row[5].template as<std::int64_t>().value_or(0));

        const auto hourly_rows = co_await c.db().query(
            "WITH buckets AS (SELECT generate_series(date_trunc('hour', NOW()) - INTERVAL "
            "'23 hours', date_trunc('hour', NOW()), INTERVAL '1 hour') AS bucket) SELECT "
            "TO_CHAR(buckets.bucket, 'YYYY-MM-DD\"T\"HH24:MI:SSOF'), COUNT(access.id)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint, COALESCE(SUM(access.response_bytes) "
            "* 8 / 3600, 0)::bigint FROM buckets LEFT JOIN sys_website_access_log access ON "
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "buckets.bucket AND access.occurred_at < buckets.bucket + INTERVAL '1 hour' GROUP "
            "BY buckets.bucket ORDER BY buckets.bucket",
            tenant_id, id);
        auto& hourly = result.template ensure<"hourly">();
        append_buckets(hourly, hourly_rows);

        const auto daily_rows = co_await c.db().query(
            "WITH buckets AS (SELECT generate_series(date_trunc('day', NOW()) - INTERVAL "
            "'14 days', date_trunc('day', NOW()), INTERVAL '1 day') AS bucket) SELECT "
            "TO_CHAR(buckets.bucket, 'YYYY-MM-DD\"T\"HH24:MI:SSOF'), COUNT(access.id)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint, COALESCE(SUM(access.response_bytes) "
            "* 8 / 86400, 0)::bigint FROM buckets LEFT JOIN sys_website_access_log access ON "
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "buckets.bucket AND access.occurred_at < buckets.bucket + INTERVAL '1 day' GROUP "
            "BY buckets.bucket ORDER BY buckets.bucket",
            tenant_id, id);
        auto& daily = result.template ensure<"daily">();
        append_buckets(daily, daily_rows);

        // `countries` is required: without an installed GeoIP database it stays an empty array.
        auto& countries = result.template ensure<"countries">();
        const auto& geo_database = service::geoip::get_xdb_database();
        if (geo_database.available()) {
            const auto country_ip_rows = co_await c.db().query(
                "SELECT access.client_ip::text, COUNT(*)::bigint, "
                "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log "
                "access WHERE access.tenant_id = $1 AND access.website_id = $2 AND "
                "access.occurred_at >= NOW() - INTERVAL '24 hours' AND access.client_ip IS NOT "
                "NULL GROUP BY access.client_ip",
                tenant_id, id);
            append_countries(countries, country_ip_rows, geo_database);
        }
        co_await append_ranking(
            c, result.template ensure<"status_codes">(),
            "SELECT access.status_code::text, COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), "
            "0)::bigint FROM sys_website_access_log access WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= NOW() - INTERVAL '24 hours' GROUP "
            "BY access.status_code ORDER BY COUNT(*) DESC, access.status_code ASC LIMIT 10",
            tenant_id, id);
        co_await append_ranking(
            c, result.template ensure<"methods">(),
            "SELECT access.method, COUNT(*)::bigint, COALESCE(SUM(access.response_bytes), "
            "0)::bigint FROM sys_website_access_log access WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= NOW() - INTERVAL '24 hours' GROUP "
            "BY access.method ORDER BY COUNT(*) DESC, access.method ASC LIMIT 10",
            tenant_id, id);
        co_await append_ranking(
            c, result.template ensure<"hosts">(),
            "SELECT access.host, COUNT(*)::bigint, COALESCE(SUM(access.response_bytes), "
            "0)::bigint FROM sys_website_access_log access WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= NOW() - INTERVAL '24 hours' GROUP "
            "BY access.host ORDER BY COUNT(*) DESC, access.host ASC LIMIT 10",
            tenant_id, id);
        co_await append_ranking(
            c, result.template ensure<"referers">(),
            "SELECT COALESCE(NULLIF(access.referer, ''), '/'), COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log access "
            "WHERE access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "NOW() - INTERVAL '24 hours' GROUP BY 1 ORDER BY COUNT(*) DESC, 1 ASC LIMIT 10",
            tenant_id, id);
        co_await append_ranking(
            c, result.template ensure<"paths">(),
            "SELECT split_part(access.target, '?', 1), COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), "
            "0)::bigint FROM sys_website_access_log access WHERE access.tenant_id = $1 AND "
            "access.website_id = $2 AND access.occurred_at >= NOW() - INTERVAL '24 hours' GROUP "
            "BY 1 ORDER BY COUNT(*) DESC, 1 ASC LIMIT 10",
            tenant_id, id);
        co_await append_client_ip_ranking(
            c, result.template ensure<"client_ips_by_bytes">(), geo_database,
            "SELECT COALESCE(access.client_ip::text, '未知 IP'), COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log access "
            "WHERE access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "NOW() - INTERVAL '24 hours' GROUP BY 1 ORDER BY 3 DESC, 2 DESC, 1 ASC LIMIT 10",
            tenant_id, id);
        co_await append_client_ip_ranking(
            c, result.template ensure<"client_ips_by_requests">(), geo_database,
            "SELECT COALESCE(access.client_ip::text, '未知 IP'), COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log access "
            "WHERE access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "NOW() - INTERVAL '24 hours' GROUP BY 1 ORDER BY 2 DESC, 3 DESC, 1 ASC LIMIT 10",
            tenant_id, id);
        co_return result;
    }

private:
    template <typename output_type, typename rows_type>
    static void append_buckets(output_type& output, const rows_type& rows) {
        for (const auto& row : rows) {
            auto& item = output.emplace_back();
            item.template set<"timestamp">(row[0].value().value_or(""));
            item.template set<"request_count">(row[1].template as<std::int64_t>().value_or(0));
            item.template set<"response_bytes">(row[2].template as<std::int64_t>().value_or(0));
            item.template set<"bandwidth_bps">(row[3].template as<std::int64_t>().value_or(0));
        }
    }

    template <typename output_type, typename rows_type>
    static void append_rankings(output_type& output, const rows_type& rows) {
        for (const auto& row : rows) {
            auto& item = output.emplace_back();
            item.template set<"label">(row[0].value().value_or(""));
            item.template set<"request_count">(row[1].template as<std::int64_t>().value_or(0));
            item.template set<"response_bytes">(row[2].template as<std::int64_t>().value_or(0));
        }
    }

    template <typename output_type>
    static ruvia::task<void> append_ranking(auto& c, output_type& output, std::string_view sql,
        std::string_view tenant_id, std::string_view website_id) {
        const auto rows = co_await c.db().query(sql, tenant_id, website_id);
        append_rankings(output, rows);
        co_return;
    }

    template <typename output_type>
    static ruvia::task<void> append_client_ip_ranking(auto& c, output_type& output,
        const service::geoip::xdb_database& geo_database,
        std::string_view sql, std::string_view tenant_id,
        std::string_view website_id) {
        const auto rows = co_await c.db().query(sql, tenant_id, website_id);
        for (const auto& row : rows) {
            std::string label{row[0].value().value_or("")};
            const auto location = geo_database.lookup(label);
            if (location) {
                label += " · ";
                label += location->display_;
            } else if (label != "未知 IP") {
                label += " · 未知地区";
            }
            auto& item = output.emplace_back();
            item.template set<"label">(label);
            item.template set<"request_count">(row[1].template as<std::int64_t>().value_or(0));
            item.template set<"response_bytes">(row[2].template as<std::int64_t>().value_or(0));
            item.template set<"asn">(location ? location->asn_ : "");
            item.template set<"as_name">(location ? location->as_name_ : "");
        }
        co_return;
    }

    template <typename output_type, typename rows_type>
    static void append_countries(output_type& output, const rows_type& rows,
        const service::geoip::xdb_database& geo_database) {
        struct country_total final {
            std::int64_t request_count_{};
            std::int64_t response_bytes_{};
        };
        std::unordered_map<std::string, country_total> countries;
        for (const auto& row : rows) {
            const auto ip = row[0].value();
            if (!ip) {
                continue;
            }
            const auto country = geo_database.country(*ip);
            if (!country) {
                continue;
            }
            auto& total = countries[*country];
            total.request_count_ += row[1].template as<std::int64_t>().value_or(0);
            total.response_bytes_ += row[2].template as<std::int64_t>().value_or(0);
        }
        std::vector<std::pair<std::string, country_total>> ordered_countries;
        ordered_countries.reserve(countries.size());
        for (const auto& country : countries) {
            ordered_countries.emplace_back(country.first, country.second);
        }
        std::ranges::sort(ordered_countries, [](const auto& left, const auto& right) {
            if (left.second.request_count_ != right.second.request_count_) {
                return left.second.request_count_ > right.second.request_count_;
            }
            if (left.second.response_bytes_ != right.second.response_bytes_) {
                return left.second.response_bytes_ > right.second.response_bytes_;
            }
            return left.first < right.first;
        });
        const auto count = std::min<std::size_t>(ordered_countries.size(), 10);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& country = ordered_countries[index];
            auto& item = output.emplace_back();
            item.template set<"label">(country.first);
            item.template set<"request_count">(country.second.request_count_);
            item.template set<"response_bytes">(country.second.response_bytes_);
        }
    }
};

inline const website_dashboard_service& get_website_dashboard_service() {
    static const website_dashboard_service service;
    return service;
}

}  // namespace service::website
