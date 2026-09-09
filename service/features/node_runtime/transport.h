#pragma once

#include <ruvia/web/Model.h>

namespace service::node_runtime {

RUVIA_REQUEST_MODEL(OriginHealthInput,
                    RUVIA_OPTIONAL_FIELD_NAME("website_id", websiteId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("origin_id", originId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("checked_at_unix_millis", checkedAtUnixMillis,
                                              ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("latency_millis", latencyMillis, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));
RUVIA_RESPONSE_MODEL(
    OriginHealthOutput, RUVIA_REQUIRED_FIELD_NAME("website_id", websiteId, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("origin_id", originId, ruvia::String),
    RUVIA_REQUIRED_FIELD(status, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("checked_at_unix_millis", checkedAtUnixMillis, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("latency_millis", latencyMillis, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));

RUVIA_REQUEST_MODEL(NodeRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("agent_version", agentVersion, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("cpu_usage", cpuUsage, ruvia::Double),
                    RUVIA_OPTIONAL_FIELD_NAME("memory_usage", memoryUsage, ruvia::Double),
                    RUVIA_OPTIONAL_FIELD_NAME("traffic_out_bps", trafficOutBps, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("connection_count", connectionCount, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("load_1m", load1m, ruvia::Double),
                    RUVIA_OPTIONAL_FIELD_NAME("queued_log_events", queuedLogEvents, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("dropped_log_events", droppedLogEvents, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("origin_health", originHealth,
                                              ruvia::Array<OriginHealthInput>),
                    RUVIA_OPTIONAL_FIELD(health, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));

RUVIA_RESPONSE_MODEL(
    NodeRuntimeOutput,
    RUVIA_OPTIONAL_FIELD_NAME("agent_version", agentVersion, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_usage", cpuUsage, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("memory_usage", memoryUsage, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("traffic_out_bps", trafficOutBps, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("connection_count", connectionCount, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("load_1m", load1m, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("queued_log_events", queuedLogEvents, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("dropped_log_events", droppedLogEvents, ruvia::Int64,
                              RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("origin_health", originHealth, ruvia::Array<OriginHealthOutput>),
    RUVIA_OPTIONAL_FIELD(health, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));

} // namespace service::node_runtime
