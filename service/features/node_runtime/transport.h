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

}  // namespace service::node_runtime
