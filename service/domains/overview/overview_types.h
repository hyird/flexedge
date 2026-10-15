#pragma once

#include "ruvia/web/model.h"

namespace service::overview {

RUVIA_MODEL(overview_resource_counts_dto,
    RUVIA_REQUIRED_FIELD_NAME("website_count", website_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("domain_count", domain_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("certificate_count", certificate_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("cluster_count", cluster_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("node_count", node_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("online_node_count", online_node_count, ruvia::int64));

RUVIA_MODEL(
    overview_issue_counts_dto,
    RUVIA_REQUIRED_FIELD_NAME("dns_zone_issue_count", dns_zone_issue_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("certificate_expiring_count", certificate_expiring_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("certificate_failed_count", certificate_failed_count, ruvia::int64));

// Task state is not part of the overview: its widgets read the task list
// once (docs/task-center.md).
RUVIA_MODEL(overview_data_dto, RUVIA_REQUIRED_FIELD(resources, overview_resource_counts_dto),
    RUVIA_REQUIRED_FIELD(issues, overview_issue_counts_dto));

RUVIA_MODEL(overview_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, overview_data_dto));

RUVIA_MODEL(overview_traffic_summary_dto,
    RUVIA_REQUIRED_FIELD_NAME("request_count", request_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("response_bytes", response_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("peak_bandwidth_bps", peak_bandwidth_bps, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("status_5xx_count", status5xx_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("unique_client_ips", unique_client_ips, ruvia::int64));

RUVIA_MODEL(overview_traffic_point_dto, RUVIA_REQUIRED_FIELD(timestamp, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("request_count", request_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("response_bytes", response_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("bandwidth_bps", bandwidth_bps, ruvia::int64));

RUVIA_MODEL(overview_status_class_dto, RUVIA_REQUIRED_FIELD(label, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("request_count", request_count, ruvia::int64));

RUVIA_MODEL(overview_top_website_dto, RUVIA_REQUIRED_FIELD_NAME("website_id", website_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("request_count", request_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("response_bytes", response_bytes, ruvia::int64));

RUVIA_MODEL(overview_traffic_coverage_dto,
    RUVIA_REQUIRED_FIELD_NAME("logging_website_count", logging_website_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("website_count", website_count, ruvia::int64));

RUVIA_MODEL(overview_traffic_dto, RUVIA_REQUIRED_FIELD_NAME("window_hours", window_hours, ruvia::int64),
    RUVIA_REQUIRED_FIELD(summary, overview_traffic_summary_dto),
    RUVIA_REQUIRED_FIELD(hourly, ruvia::array<overview_traffic_point_dto>),
    RUVIA_REQUIRED_FIELD_NAME("status_classes", status_classes,
        ruvia::array<overview_status_class_dto>),
    RUVIA_REQUIRED_FIELD_NAME("top_websites", top_websites,
        ruvia::array<overview_top_website_dto>),
    RUVIA_REQUIRED_FIELD(coverage, overview_traffic_coverage_dto));

RUVIA_MODEL(overview_traffic_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, overview_traffic_dto));

}  // namespace service::overview
