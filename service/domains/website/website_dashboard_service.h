#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/common/http.h"
#include "service/domains/website/website_error.h"
#include "service/domains/website/website_types.h"
#include "service/features/geoip/runtime_database.h"

namespace service::website {

// Calendar windows (今日, 本月, the 15 daily buckets) follow the console day in
// `console_time_zone`; rolling windows (last minute, 24 hours) do not depend on it.
class website_dashboard_service final {
public:
    static constexpr std::string_view console_time_zone{"Asia/Shanghai"};

    // `as_of` is the reference time as PostgreSQL `timestamptz` input; the default
    // `now` is the transaction start time, the same instant as `NOW()`.
    ruvia::task<website_dashboard_dto> dashboard(auto& c, const std::string& tenant_id,
        const std::string& id, std::string_view as_of = "now") const {
        const auto website = co_await c.db().query(
            "SELECT 1 FROM sys_website WHERE tenant_id = $1 AND id = $2 AND deleted_at IS NULL "
            "LIMIT 1",
            tenant_id, id);
        if (website.empty()) {
            service::common::throw_app_error(website_error::not_found);
        }

        const auto summary_rows = co_await c.db().query(
            "WITH bounds AS (SELECT date_trunc('month', $3::timestamptz AT TIME ZONE $4::text) "
            "AT TIME ZONE $4::text AS current_month_start, date_trunc('day', $3::timestamptz AT "
            "TIME ZONE $4::text) AT TIME ZONE $4::text AS today_start), "
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
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "$3::timestamptz - INTERVAL '1 minute'), 0)::bigint, COALESCE((SELECT COUNT(DISTINCT "
            "access.client_ip) FROM sys_website_access_log access, bounds WHERE "
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "bounds.today_start AND access.client_ip IS NOT NULL), 0)::bigint, "
            "COALESCE((SELECT SUM(access.response_bytes) FROM sys_website_access_log access, "
            "bounds WHERE access.tenant_id = $1 AND access.website_id = $2 AND "
            "access.occurred_at >= bounds.today_start), 0)::bigint FROM bounds",
            tenant_id, id, as_of, console_time_zone);

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
            "WITH buckets AS (SELECT generate_series(date_trunc('hour', $3::timestamptz) - "
            "INTERVAL '23 hours', date_trunc('hour', $3::timestamptz), INTERVAL '1 hour') AS "
            "bucket) SELECT TO_CHAR(buckets.bucket, 'YYYY-MM-DD\"T\"HH24:MI:SSOF'), "
            "COUNT(access.id)::bigint, COALESCE(SUM(access.response_bytes), 0)::bigint, "
            "COALESCE(SUM(access.response_bytes) * 8 / 3600, 0)::bigint FROM buckets LEFT JOIN "
            "sys_website_access_log access ON access.tenant_id = $1 AND access.website_id = $2 "
            "AND access.occurred_at >= buckets.bucket AND access.occurred_at < buckets.bucket + "
            "INTERVAL '1 hour' GROUP BY buckets.bucket ORDER BY buckets.bucket",
            tenant_id, id, as_of);
        auto& hourly = result.template ensure<"hourly">();
        append_buckets(hourly, hourly_rows);

        // The last of the 15 console days is today: the same start as 今日 above.
        const auto daily_rows = co_await c.db().query(
            "WITH bounds AS (SELECT date_trunc('day', $3::timestamptz AT TIME ZONE $4::text) "
            "AS today_local), buckets AS (SELECT day_local AT TIME ZONE $4::text AS bucket, "
            "(day_local + INTERVAL '1 day') AT TIME ZONE $4::text AS bucket_end FROM bounds, "
            "generate_series(bounds.today_local - INTERVAL '14 days', bounds.today_local, "
            "INTERVAL '1 day') AS day_local) SELECT TO_CHAR(buckets.bucket, "
            "'YYYY-MM-DD\"T\"HH24:MI:SSOF'), COUNT(access.id)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint, COALESCE(SUM(access.response_bytes) "
            "* 8 / 86400, 0)::bigint FROM buckets LEFT JOIN sys_website_access_log access ON "
            "access.tenant_id = $1 AND access.website_id = $2 AND access.occurred_at >= "
            "buckets.bucket AND access.occurred_at < buckets.bucket_end GROUP BY buckets.bucket, "
            "buckets.bucket_end ORDER BY buckets.bucket",
            tenant_id, id, as_of, console_time_zone);
        auto& daily = result.template ensure<"daily">();
        append_buckets(daily, daily_rows);

        // All IP-based rankings (countries, ASNs, client IPs by bytes/requests) use
        // every IP in the window in a single pass before applying their own top 10.
        // Missing IPs or ASN metadata still contribute to the unknown ASN total.
        const auto& geo_database = service::geoip::get_xdb_database();
        const auto geographic_ip_rows = co_await c.db().query(
            "SELECT access.client_ip::text, COUNT(*)::bigint, "
            "COALESCE(SUM(access.response_bytes), 0)::bigint FROM sys_website_access_log "
            "access WHERE access.tenant_id = $1 AND access.website_id = $2 AND "
            "access.occurred_at >= $3::timestamptz - INTERVAL '24 hours' "
            "GROUP BY access.client_ip",
            tenant_id, id, as_of);
        append_geographic_rankings(result, geographic_ip_rows, geo_database);
        co_await append_dimension_rankings(c, result, tenant_id, id, as_of);
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
    static ruvia::task<void> append_ranking(auto& c, output_type& output,
        std::string_view label_sql, std::string_view group_order_sql,
        std::string_view tenant_id, std::string_view website_id, std::string_view as_of) {
        const std::string sql = "SELECT " + std::string(label_sql) +
                                ", COUNT(*)::bigint, COALESCE(SUM(access.response_bytes), "
                                "0)::bigint FROM sys_website_access_log access WHERE "
                                "access.tenant_id = $1 AND access.website_id = $2 AND "
                                "access.occurred_at >= $3::timestamptz - INTERVAL '24 hours' "
                                "GROUP BY " +
                                std::string(group_order_sql) + " ORDER BY COUNT(*) DESC, " +
                                std::string(group_order_sql) + " ASC LIMIT 10";
        const auto rows = co_await c.db().query(sql, tenant_id, website_id, as_of);
        append_rankings(output, rows);
        co_return;
    }

    static ruvia::task<void> append_dimension_rankings(auto& c, website_dashboard_dto& result,
        std::string_view tenant_id, std::string_view website_id, std::string_view as_of) {
        co_await append_ranking(c, result.template ensure<"status_codes">(),
            "access.status_code::text", "access.status_code", tenant_id, website_id, as_of);
        co_await append_ranking(c, result.template ensure<"methods">(), "access.method",
            "access.method", tenant_id, website_id, as_of);
        co_await append_ranking(c, result.template ensure<"hosts">(), "access.host",
            "access.host", tenant_id, website_id, as_of);
        co_await append_ranking(c, result.template ensure<"referers">(),
            "COALESCE(NULLIF(access.referer, ''), '/')", "1", tenant_id, website_id, as_of);
        co_await append_ranking(c, result.template ensure<"paths">(),
            "split_part(access.target, '?', 1)", "1", tenant_id, website_id, as_of);
    }

    struct ranking_total final {
        std::int64_t request_count_{};
        std::int64_t response_bytes_{};
    };

    template <typename output_type>
    static void append_ordered_totals(output_type& output,
        std::vector<std::pair<std::string, ranking_total>> totals) {
        std::ranges::sort(totals, [](const auto& left, const auto& right) {
            if (left.second.request_count_ != right.second.request_count_) {
                return left.second.request_count_ > right.second.request_count_;
            }
            if (left.second.response_bytes_ != right.second.response_bytes_) {
                return left.second.response_bytes_ > right.second.response_bytes_;
            }
            return left.first < right.first;
        });
        const auto count = std::min<std::size_t>(totals.size(), 10);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& total = totals[index];
            auto& item = output.emplace_back();
            item.template set<"label">(total.first);
            item.template set<"request_count">(total.second.request_count_);
            item.template set<"response_bytes">(total.second.response_bytes_);
        }
    }

    template <typename rows_type>
    static void append_geographic_rankings(website_dashboard_dto& result, const rows_type& rows,
        const service::geoip::xdb_database& geo_database) {
        auto& countries_output = result.template ensure<"countries">();
        auto& asns_output = result.template ensure<"asns">();
        auto& ips_by_bytes_output = result.template ensure<"client_ips_by_bytes">();
        auto& ips_by_requests_output = result.template ensure<"client_ips_by_requests">();

        struct asn_total final {
            ranking_total traffic_;
            std::string as_name_;
        };
        struct client_ip_entry final {
            std::string ip_text_;
            std::string label_;
            std::int64_t request_count_{};
            std::int64_t response_bytes_{};
            std::string asn_;
            std::string as_name_;
        };

        std::unordered_map<std::string, ranking_total> countries;
        std::unordered_map<std::string, asn_total> asns;
        std::vector<client_ip_entry> client_ips;
        client_ips.reserve(rows.size());

        for (const auto& row : rows) {
            const auto ip = row[0].value();
            const auto location = ip ? geo_database.lookup(*ip) : std::nullopt;
            const auto request_count = row[1].template as<std::int64_t>().value_or(0);
            const auto response_bytes = row[2].template as<std::int64_t>().value_or(0);
            if (location && !location->country_.empty()) {
                auto& country = countries[location->country_];
                country.request_count_ += request_count;
                country.response_bytes_ += response_bytes;
            }

            const bool has_asn = location && !location->asn_.empty();
            auto& asn = asns[has_asn ? location->asn_ : "未知 ASN"];
            asn.traffic_.request_count_ += request_count;
            asn.traffic_.response_bytes_ += response_bytes;
            // Group by ASN, even if records contain different organization names.
            // Choose the first nonempty name lexically so row order cannot change labels.
            if (has_asn && !location->as_name_.empty() &&
                (asn.as_name_.empty() || location->as_name_ < asn.as_name_)) {
                asn.as_name_ = location->as_name_;
            }

            std::string ip_text{ip.value_or("未知 IP")};
            std::string label = ip ? ip_text + " · " + (location ? location->display_ : "未知地区")
                                   : ip_text;
            client_ips.push_back({
                .ip_text_ = std::move(ip_text),
                .label_ = std::move(label),
                .request_count_ = request_count,
                .response_bytes_ = response_bytes,
                .asn_ = location ? location->asn_ : "",
                .as_name_ = location ? location->as_name_ : "",
            });
        }

        std::vector<std::pair<std::string, ranking_total>> country_totals;
        country_totals.reserve(countries.size());
        for (const auto& [country, total] : countries) {
            country_totals.emplace_back(country, total);
        }
        append_ordered_totals(countries_output, std::move(country_totals));

        std::vector<std::pair<std::string, ranking_total>> asn_totals;
        asn_totals.reserve(asns.size());
        for (const auto& [asn, total] : asns) {
            asn_totals.emplace_back(
                total.as_name_.empty() ? asn : asn + " " + total.as_name_, total.traffic_);
        }
        append_ordered_totals(asns_output, std::move(asn_totals));

        const auto append_top_client_ips = [](auto& output,
                                               const std::vector<client_ip_entry>& entries) {
            const auto count = std::min<std::size_t>(entries.size(), 10);
            for (std::size_t index = 0; index < count; ++index) {
                const auto& entry = entries[index];
                auto& item = output.emplace_back();
                item.template set<"label">(entry.label_);
                item.template set<"request_count">(entry.request_count_);
                item.template set<"response_bytes">(entry.response_bytes_);
                item.template set<"asn">(entry.asn_);
                item.template set<"as_name">(entry.as_name_);
            }
        };

        std::ranges::sort(client_ips, [](const auto& left, const auto& right) {
            if (left.response_bytes_ != right.response_bytes_) {
                return left.response_bytes_ > right.response_bytes_;
            }
            if (left.request_count_ != right.request_count_) {
                return left.request_count_ > right.request_count_;
            }
            return left.ip_text_ < right.ip_text_;
        });
        append_top_client_ips(ips_by_bytes_output, client_ips);

        std::ranges::sort(client_ips, [](const auto& left, const auto& right) {
            if (left.request_count_ != right.request_count_) {
                return left.request_count_ > right.request_count_;
            }
            if (left.response_bytes_ != right.response_bytes_) {
                return left.response_bytes_ > right.response_bytes_;
            }
            return left.ip_text_ < right.ip_text_;
        });
        append_top_client_ips(ips_by_requests_output, client_ips);
    }
};

inline const website_dashboard_service& get_website_dashboard_service() {
    static const website_dashboard_service service;
    return service;
}

}  // namespace service::website
