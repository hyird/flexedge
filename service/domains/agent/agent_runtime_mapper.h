#pragma once

#include <ruvia/web/model_json.h>

#include <string>

#include "service/domains/agent/agent_types.h"
#include "service/features/node_runtime/transport.h"

namespace service::agent {

template <typename runtime_type>
std::string heartbeat_runtime_json(runtime_type& c, const heartbeat_report& report) {
    service::node_runtime::node_runtime_output runtime({.resource_ = c.pool()});
    runtime.set<"agent_version">(report.agent_version_);
    runtime.set<"cpu_usage">(report.cpu_usage_);
    runtime.set<"memory_usage">(report.memory_usage_);
    runtime.set<"traffic_out_bps">(report.traffic_out_bps_);
    runtime.set<"connection_count">(report.connection_count_);
    if (report.connected_ip_count_) {
        runtime.set<"connected_ip_count">(*report.connected_ip_count_);
    }
    runtime.set<"load1m">(report.load1m_);
    runtime.set<"queued_log_events">(report.queued_log_events_);
    runtime.set<"dropped_log_events">(report.dropped_log_events_);
    runtime.set<"health">(report.health_);
    runtime.set<"last_error">(report.last_error_);
    auto& origin_health = runtime.ensure<"origin_health">();
    origin_health.reserve(report.origin_health_.size());
    for (const auto& report_item : report.origin_health_) {
        auto& item = origin_health.emplace_back(ruvia::model_options{.resource_ = c.pool()});
        item.set<"website_id">(report_item.website_id_);
        item.set<"origin_id">(report_item.origin_id_);
        item.set<"status">(report_item.status_);
        item.set<"checked_at_unix_millis">(report_item.checked_at_unix_millis_);
        item.set<"latency_millis">(report_item.latency_millis_);
        if (!report_item.last_error_.empty()) {
            item.set<"last_error">(report_item.last_error_);
        }
    }
    const auto json = ruvia::to_json(runtime, {.resource_ = c.pool()});
    return std::string(json.data(), json.size());
}

}  // namespace service::agent
