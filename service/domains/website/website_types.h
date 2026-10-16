#pragma once

#include <optional>
#include <string>
#include <utility>

#include "ruvia/web/model.h"

#include "service/features/website_config/mapper.h"

namespace service::website {

RUVIA_MODEL(website_save_input, RUVIA_OPTIONAL_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD(config, service::website_config::website_config_input));

struct website_save_data final {
    std::string status_;
    service::website_config::website_config_data config_;
};

[[nodiscard]] inline std::optional<website_save_data> normalize(const website_save_input& input) {
    const auto& status = input.get<"status">();
    const auto& config = input.get<"config">();
    if (!status || !config) {
        return std::nullopt;
    }
    auto normalized_config = service::website_config::normalize(*config);
    if (!normalized_config) {
        return std::nullopt;
    }
    return website_save_data{.status_ = std::string(status->view()),
        .config_ = std::move(*normalized_config)};
}

RUVIA_MODEL(
    website_domain_runtime_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("access_protocol", access_protocol, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resolution_status", resolution_status, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", last_verified_at, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(
    website_origin_runtime_dto, RUVIA_REQUIRED_FIELD_NAME("node_id", node_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_name", node_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("origin_id", origin_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("checked_at_unix_millis", checked_at_unix_millis, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("latency_millis", latency_millis, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY));

RUVIA_MODEL(website_origin_source_dto,
    RUVIA_REQUIRED_FIELD_NAME("node_id", node_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_revision", node_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("reported_at", reported_at, ruvia::string));
RUVIA_MODEL(website_runtime_dto,
    RUVIA_OPTIONAL_FIELD_NAME("origin_sources", origin_sources,
        ruvia::array<website_origin_source_dto>, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("domain_states", domain_states,
        ruvia::array<website_domain_runtime_dto>),
    RUVIA_REQUIRED_FIELD_NAME("origin_states", origin_states,
        ruvia::array<website_origin_runtime_dto>),
    RUVIA_REQUIRED_FIELD_NAME("deploy_status", deploy_status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_node_count", target_node_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("synced_node_count", synced_node_count, ruvia::int64));

RUVIA_MODEL(website_certificate_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(domains, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(usable, ruvia::bool_value));

RUVIA_MODEL(website_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("cluster_id", cluster_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("cluster_name", cluster_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("access_domain", access_domain, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD(config, service::website_config::website_config_output),
    RUVIA_REQUIRED_FIELD(certificates, ruvia::array<website_certificate_dto>),
    RUVIA_REQUIRED_FIELD(runtime, website_runtime_dto),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string));

RUVIA_MODEL(website_page_data_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<website_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64));
RUVIA_MODEL(website_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, website_page_data_dto));
RUVIA_MODEL(website_detail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, website_dto));
// `operation_id`: the task the creation started.
RUVIA_MODEL(website_created_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("operation_id", operation_id, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(website_created_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, website_created_dto));

RUVIA_MODEL(
    website_dashboard_summary_dto,
    RUVIA_REQUIRED_FIELD_NAME("previous_month_peak_bps", previous_month_peak_bps, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("current_month_peak_bps", current_month_peak_bps, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("today_peak_bps", today_peak_bps, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("current_bandwidth_bps", current_bandwidth_bps, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("today_unique_ips", today_unique_ips, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("today_response_bytes", today_response_bytes, ruvia::int64));
RUVIA_MODEL(website_dashboard_series_point_dto,
    RUVIA_REQUIRED_FIELD(timestamp, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("request_count", request_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("response_bytes", response_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("bandwidth_bps", bandwidth_bps, ruvia::int64));
RUVIA_MODEL(website_dashboard_ranking_dto, RUVIA_REQUIRED_FIELD(label, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("request_count", request_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("response_bytes", response_bytes, ruvia::int64));
RUVIA_MODEL(
    website_dashboard_client_ip_ranking_dto, RUVIA_REQUIRED_FIELD(label, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("request_count", request_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("response_bytes", response_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(asn, ruvia::string), RUVIA_REQUIRED_FIELD_NAME("as_name", as_name, ruvia::string));
RUVIA_MODEL(
    website_dashboard_dto, RUVIA_REQUIRED_FIELD(summary, website_dashboard_summary_dto),
    RUVIA_REQUIRED_FIELD(hourly, ruvia::array<website_dashboard_series_point_dto>),
    RUVIA_REQUIRED_FIELD(daily, ruvia::array<website_dashboard_series_point_dto>),
    RUVIA_REQUIRED_FIELD_NAME("status_codes", status_codes,
        ruvia::array<website_dashboard_ranking_dto>),
    RUVIA_REQUIRED_FIELD(methods, ruvia::array<website_dashboard_ranking_dto>),
    RUVIA_REQUIRED_FIELD(asns, ruvia::array<website_dashboard_ranking_dto>),
    RUVIA_REQUIRED_FIELD(countries, ruvia::array<website_dashboard_ranking_dto>),
    RUVIA_REQUIRED_FIELD(hosts, ruvia::array<website_dashboard_ranking_dto>),
    RUVIA_REQUIRED_FIELD(referers, ruvia::array<website_dashboard_ranking_dto>),
    RUVIA_REQUIRED_FIELD(paths, ruvia::array<website_dashboard_ranking_dto>),
    RUVIA_REQUIRED_FIELD_NAME("client_ips_by_bytes", client_ips_by_bytes,
        ruvia::array<website_dashboard_client_ip_ranking_dto>),
    RUVIA_REQUIRED_FIELD_NAME("client_ips_by_requests", client_ips_by_requests,
        ruvia::array<website_dashboard_client_ip_ranking_dto>));
RUVIA_MODEL(website_dashboard_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, website_dashboard_dto));

RUVIA_MODEL(
    website_access_log_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("occurred_at", occurred_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_id", node_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_name", node_name, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("client_ip", client_ip, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("client_ip_location", client_ip_location, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("client_ip_asn", client_ip_asn, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("client_ip_as_name", client_ip_as_name, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(protocol, ruvia::string), RUVIA_REQUIRED_FIELD(method, ruvia::string),
    RUVIA_REQUIRED_FIELD(host, ruvia::string), RUVIA_REQUIRED_FIELD(target, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("status_code", status_code, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("request_bytes", request_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("response_bytes", response_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("duration_ms", duration_ms, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("user_agent", user_agent, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(referer, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("request_headers", request_headers, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("request_body", request_body, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("request_body_truncated", request_body_truncated, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD_NAME("tls_fingerprint", tls_fingerprint, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("response_headers", response_headers, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("query_string", query_string, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(cookies, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(website_access_log_tail_data_dto,
    RUVIA_REQUIRED_FIELD(list, ruvia::array<website_access_log_dto>),
    RUVIA_OPTIONAL_FIELD(cursor, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(website_access_log_tail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, website_access_log_tail_data_dto));
RUVIA_MODEL(website_access_log_page_data_dto,
    RUVIA_REQUIRED_FIELD(list, ruvia::array<website_access_log_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64));
RUVIA_MODEL(website_access_log_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, website_access_log_page_data_dto));

}  // namespace service::website
