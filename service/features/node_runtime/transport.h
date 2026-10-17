#pragma once

#include "ruvia/web/model.h"

namespace service::node_runtime {

RUVIA_MODEL(origin_health_input,
    RUVIA_OPTIONAL_FIELD_NAME("website_id", website_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("origin_id", origin_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("checked_at_unix_millis", checked_at_unix_millis,
        ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("latency_millis", latency_millis, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string));
RUVIA_MODEL(
    origin_health_output, RUVIA_REQUIRED_FIELD_NAME("website_id", website_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("origin_id", origin_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("checked_at_unix_millis", checked_at_unix_millis, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("latency_millis", latency_millis, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY));

RUVIA_MODEL(node_runtime_input,
    RUVIA_OPTIONAL_FIELD_NAME("agent_version", agent_version, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_usage", cpu_usage, ruvia::double_value),
    RUVIA_OPTIONAL_FIELD_NAME("memory_usage", memory_usage, ruvia::double_value),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_count", cpu_count, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("memory_total_bytes", memory_total_bytes, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("disk_total_bytes", disk_total_bytes, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("disk_usage", disk_usage, ruvia::double_value),
    RUVIA_OPTIONAL_FIELD_NAME("traffic_out_bps", traffic_out_bps, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("connection_count", connection_count, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("connected_ip_count", connected_ip_count, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("load_1m", load1m, ruvia::double_value),
    RUVIA_OPTIONAL_FIELD_NAME("queued_log_events", queued_log_events, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("dropped_log_events", dropped_log_events, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("origin_health", origin_health,
        ruvia::array<origin_health_input>),
    RUVIA_OPTIONAL_FIELD(health, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string));

RUVIA_MODEL(
    node_runtime_output,
    RUVIA_OPTIONAL_FIELD_NAME("agent_version", agent_version, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_usage", cpu_usage, ruvia::double_value, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("memory_usage", memory_usage, ruvia::double_value, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_count", cpu_count, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("memory_total_bytes", memory_total_bytes, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("disk_total_bytes", disk_total_bytes, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("disk_usage", disk_usage, ruvia::double_value, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("traffic_out_bps", traffic_out_bps, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("connection_count", connection_count, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("connected_ip_count", connected_ip_count, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("load_1m", load1m, ruvia::double_value, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("queued_log_events", queued_log_events, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("dropped_log_events", dropped_log_events, ruvia::int64,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("origin_health", origin_health, ruvia::array<origin_health_output>),
    RUVIA_OPTIONAL_FIELD(health, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY));

RUVIA_MODEL(
    node_runtime_dto,
    RUVIA_REQUIRED_FIELD_NAME("registration_status", registration_status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("connection_status", connection_status, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("last_heartbeat_at", last_heartbeat_at, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("applied_node_spec_revision", applied_node_spec_revision, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("active_release_id", active_release_id, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("active_manifest_digest", active_manifest_digest, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("agent_version", agent_version, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_usage", cpu_usage, ruvia::double_value, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("memory_usage", memory_usage, ruvia::double_value, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_count", cpu_count, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("memory_total_bytes", memory_total_bytes, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("disk_total_bytes", disk_total_bytes, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("disk_usage", disk_usage, ruvia::double_value, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("traffic_out_bps", traffic_out_bps, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("connection_count", connection_count, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("connected_ip_count", connected_ip_count, ruvia::int64,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("load_1m", load1m, ruvia::double_value, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("queued_log_events", queued_log_events, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("dropped_log_events", dropped_log_events, ruvia::int64,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(health, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY));

RUVIA_MODEL(
    website_origin_runtime_dto, RUVIA_REQUIRED_FIELD_NAME("node_id", node_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_name", node_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("origin_id", origin_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("checked_at_unix_millis", checked_at_unix_millis, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("latency_millis", latency_millis, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY));

}  // namespace service::node_runtime
