#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "service/domains/overview/overview_types.h"

namespace service::overview {

// Tenant-wide access-log traffic for the last 24 hourly buckets. All parts use
// one window: from the start of the hour 23 hours ago up to the end of the
// current hour, so the summary always equals the sum of the hourly points.
// Every scan filters on the primary key prefix (tenant_id, occurred_at), and
// the window bounds restrict TimescaleDB to the chunks covering 24 hours.
// Only logged requests count: websites can disable access logs or log some
// status classes only, which the coverage block makes visible.
class overview_traffic_service final {
public:
    static constexpr std::int64_t window_hours = 24;
    static constexpr std::size_t top_websites = 8;
    static constexpr std::array<std::string_view, 5> status_classes{"1xx", "2xx", "3xx", "4xx",
        "5xx"};

    ruvia::task<overview_traffic_dto> get(auto& c, const std::string& tenant_id) const {
        const auto hourly_rows = co_await c.db().query(hourly_query(c.pool(), tenant_id));
        const auto totals_rows = co_await c.db().query(totals_query(c.pool(), tenant_id));
        const auto top_rows = co_await c.db().query(top_websites_query(c.pool(), tenant_id));

        overview_traffic_dto result({.resource_ = c.pool()});
        result.template set<"window_hours">(window_hours);
        std::int64_t requests{}, bytes{}, peak{};
        std::array<std::int64_t, status_classes.size()> classes{};
        auto& hourly = result.template ensure<"hourly">();
        for (const auto& row : hourly_rows) {
            auto& point = hourly.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            const auto point_requests = number(row, 1);
            const auto point_bytes = number(row, 2);
            point.template set<"timestamp">(row[0].value().value_or(""));
            point.template set<"request_count">(point_requests);
            point.template set<"response_bytes">(point_bytes);
            point.template set<"bandwidth_bps">(number(row, 3));
            requests += point_requests;
            bytes += point_bytes;
            peak = std::max(peak, number(row, 4));
            for (std::size_t index = 0; index < classes.size(); ++index) {
                classes[index] += number(row, 5 + index);
            }
        }

        const auto totals = totals_rows.empty() ? nullptr : &totals_rows.front();
        auto& summary = result.template ensure<"summary">();
        summary.template set<"request_count">(requests);
        summary.template set<"response_bytes">(bytes);
        summary.template set<"peak_bandwidth_bps">(peak);
        summary.template set<"status5xx_count">(classes.back());
        summary.template set<"unique_client_ips">(totals ? number(*totals, 0) : 0);

        auto& status_classes_value = result.template ensure<"status_classes">();
        for (std::size_t index = 0; index < classes.size(); ++index) {
            auto& item = status_classes_value.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            item.template set<"label">(status_classes[index]);
            item.template set<"request_count">(classes[index]);
        }

        auto& top_websites = result.template ensure<"top_websites">();
        for (const auto& row : top_rows) {
            auto& item = top_websites.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            item.template set<"website_id">(row[0].value().value_or(""));
            item.template set<"name">(row[1].value().value_or(""));
            item.template set<"request_count">(number(row, 2));
            item.template set<"response_bytes">(number(row, 3));
        }

        auto& coverage = result.template ensure<"coverage">();
        coverage.template set<"logging_website_count">(totals ? number(*totals, 2) : 0);
        coverage.template set<"website_count">(totals ? number(*totals, 1) : 0);
        co_return result;
    }

    // Exposed for EXPLAIN in tests and operations; compiled per read.
    static ruvia::db_query hourly_query(std::pmr::memory_resource* resource,
        const std::string& tenant_id) {
        ruvia::db_query minutes(resource);
        const auto status_code = minutes.column("status_code", "access");
        auto status_count = [&](std::int64_t lower, std::string_view alias) {
            return minutes.alias(
                minutes.filter(minutes.aggregate("count", {minutes.star()}),
                    minutes.binary(
                        minutes.binary(status_code, ruvia::db_binary_operator::greater_equal,
                            minutes.value(lower)),
                        ruvia::db_binary_operator::and_value,
                        minutes.binary(status_code, ruvia::db_binary_operator::less,
                            minutes.value(lower + 100)))),
                alias);
        };
        const auto minute = minutes.call(
            "date_trunc", {minutes.value("minute"), minutes.column("occurred_at", "access")});
        minutes
            .select({minutes.alias(minute, "bucket"),
                minutes.alias(minutes.aggregate("count", {minutes.star()}), "requests"),
                minutes.alias(minutes.aggregate("sum", {minutes.column("response_bytes", "access")}),
                    "bytes"),
                status_count(100, "status_1xx"), status_count(200, "status_2xx"),
                status_count(300, "status_3xx"), status_count(400, "status_4xx"),
                status_count(500, "status_5xx")})
            .from("sys_website_access_log", "access");
        restrict_to_window(minutes, tenant_id);
        minutes.group_by({minute});

        ruvia::db_query query(resource);
        const auto bucket = query.column("bucket", "hours");
        const auto hour = query.cast(query.value("1 hour"), ruvia::db_data_type::interval);
        const auto sum = [&](std::string_view column) {
            return query.coalesce(
                {query.aggregate("sum", {query.column(column, "minutes")}), query.value(0)});
        };
        const auto as_big_int = [&](ruvia::db_expression value) {
            return query.cast(value, ruvia::db_data_type::big_int);
        };
        const auto bytes = sum("bytes");
        query.with("minutes", minutes)
            .select({query.call("to_char", {bucket, query.value(timestamp_format)}),
                as_big_int(sum("requests")), as_big_int(bytes),
                as_big_int(query.binary(query.binary(bytes, ruvia::db_binary_operator::multiply,
                                            query.value(8)),
                    ruvia::db_binary_operator::divide, query.value(3600))),
                // Peak of per-minute bandwidth inside the hour.
                as_big_int(query.binary(
                    query.binary(query.coalesce({query.aggregate(
                                                     "max", {query.column("bytes", "minutes")}),
                                     query.value(0)}),
                        ruvia::db_binary_operator::multiply, query.value(8)),
                    ruvia::db_binary_operator::divide, query.value(60))),
                as_big_int(sum("status_1xx")), as_big_int(sum("status_2xx")),
                as_big_int(sum("status_3xx")), as_big_int(sum("status_4xx")),
                as_big_int(sum("status_5xx"))})
            .from_function(query.call("generate_series",
                               {window_start(query), current_hour(query), hour}),
                "hours", {.columns_ = {{.name_ = "bucket", .type_ = {}}}})
            .join(ruvia::db_join_type::left, "minutes",
                query.binary(
                    query.binary(query.column("bucket", "minutes"),
                        ruvia::db_binary_operator::greater_equal, bucket),
                    ruvia::db_binary_operator::and_value,
                    query.binary(query.column("bucket", "minutes"), ruvia::db_binary_operator::less,
                        query.binary(bucket, ruvia::db_binary_operator::add, hour))),
                "minutes")
            .group_by({bucket})
            .order_by(bucket);
        return query;
    }

    // Unique client IPs in the window and the website coverage counts.
    static ruvia::db_query totals_query(std::pmr::memory_resource* resource,
        const std::string& tenant_id) {
        ruvia::db_query clients(resource);
        clients.select(clients.aggregate("count", {clients.column("client_ip", "access")}, true))
            .from("sys_website_access_log", "access");
        restrict_to_window(clients, tenant_id);

        ruvia::db_query websites(resource);
        const auto logging_enabled = websites.binary(
            websites.column("config", "website"), ruvia::db_binary_operator::json_contains,
            websites.cast(websites.value(R"({"access_log_enabled":true})"),
                ruvia::db_data_type::jsonb));
        websites
            .select({websites.aggregate("count", {websites.star()}),
                websites.filter(websites.aggregate("count", {websites.star()}),
                    logging_enabled)})
            .from("sys_website", "website")
            .where(websites.binary(websites.column("tenant_id", "website"),
                ruvia::db_binary_operator::equal, websites.value(tenant_id)))
            .and_where(websites.unary(ruvia::db_unary_operator::is_null,
                websites.column("deleted_at", "website")));

        ruvia::db_query query(resource);
        query.with("clients", clients).with("websites", websites);
        query.select({query.star("clients"), query.star("websites")})
            .from("clients")
            .join(ruvia::db_join_type::cross, "websites");
        return query;
    }

    // Largest websites by response bytes; deleted websites are excluded.
    static ruvia::db_query top_websites_query(std::pmr::memory_resource* resource,
        const std::string& tenant_id) {
        ruvia::db_query totals(resource);
        const auto website_id = totals.column("website_id", "access");
        totals
            .select({website_id, totals.alias(totals.aggregate("count", {totals.star()}), "requests"),
                totals.alias(totals.aggregate("sum", {totals.column("response_bytes", "access")}),
                    "bytes")})
            .from("sys_website_access_log", "access");
        restrict_to_window(totals, tenant_id);
        totals.group_by({website_id});

        ruvia::db_query query(resource);
        const auto config = query.column("config", "website");
        const auto text = [&](ruvia::db_expression value, std::string_view key) {
            return query.binary(value, ruvia::db_binary_operator::json_get_text, query.value(key));
        };
        const auto first_hostname = text(
            query.binary(query.binary(config, ruvia::db_binary_operator::json_get,
                             query.value("domains")),
                ruvia::db_binary_operator::json_get, query.value(0)),
            "hostname");
        const auto bytes = query.column("bytes", "totals");
        const auto requests = query.column("requests", "totals");
        query.with("totals", totals)
            .select({query.column("id", "website"),
                query.coalesce({query.null_if(text(config, "name"), query.value("")),
                    first_hostname, query.value("")}),
                query.cast(requests, ruvia::db_data_type::big_int),
                query.cast(bytes, ruvia::db_data_type::big_int)})
            .from("totals")
            .join(ruvia::db_join_type::inner, "sys_website",
                query.binary(query.binary(query.column("tenant_id", "website"),
                                 ruvia::db_binary_operator::equal, query.value(tenant_id)),
                    ruvia::db_binary_operator::and_value,
                    query.binary(query.column("id", "website"),
                        ruvia::db_binary_operator::equal,
                        query.column("website_id", "totals"))),
                "website")
            .where(query.unary(ruvia::db_unary_operator::is_null, query.column("deleted_at", "website")))
            .order_by(bytes, ruvia::db_order_direction::desc)
            .add_order_by(requests, ruvia::db_order_direction::desc)
            .add_order_by(query.column("id", "website"))
            .limit(top_websites);
        return query;
    }

private:
    // Same timestamp format as the website dashboard buckets.
    static constexpr std::string_view timestamp_format{"YYYY-MM-DD\"T\"HH24:MI:SSOF"};

    static ruvia::db_expression current_hour(ruvia::db_query& query) {
        return query.call("date_trunc", {query.value("hour"), query.call("now")});
    }
    static ruvia::db_expression window_start(ruvia::db_query& query) {
        return query.binary(current_hour(query), ruvia::db_binary_operator::subtract,
            query.cast(query.value("23 hours"), ruvia::db_data_type::interval));
    }
    static void restrict_to_window(ruvia::db_query& query, const std::string& tenant_id) {
        const auto occurred_at = query.column("occurred_at", "access");
        query
            .where(query.binary(query.column("tenant_id", "access"),
                ruvia::db_binary_operator::equal, query.value(tenant_id)))
            .and_where(query.binary(occurred_at, ruvia::db_binary_operator::greater_equal,
                window_start(query)))
            .and_where(query.binary(
                occurred_at, ruvia::db_binary_operator::less,
                query.binary(current_hour(query), ruvia::db_binary_operator::add,
                    query.cast(query.value("1 hour"), ruvia::db_data_type::interval))));
    }
    template <typename row_type>
    static std::int64_t number(const row_type& row, std::size_t index) {
        return row[index].template as<std::int64_t>().value_or(0);
    }
};

inline const overview_traffic_service& get_overview_traffic_service() {
    static const overview_traffic_service service;
    return service;
}

}  // namespace service::overview
