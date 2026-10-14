#pragma once
#include <cstdint>

#include "node/proto/edge_control.pb.h"
#include "service/domains/agent/agent_types.h"
namespace service::agent {
inline heartbeat_report to_heartbeat_report(const flexedge::node::v2::Heartbeat& value) {
    heartbeat_report result{
        .node_id_ = value.node_id(),
        .applied_node_spec_revision_ = value.applied_node_spec_revision(),
        .active_release_id_ = value.active_release_id(),
        .active_manifest_digest_ = value.active_manifest_digest(),
        .agent_version_ = value.agent_version(),
        .cpu_usage_ = value.cpu_usage(),
        .memory_usage_ = value.memory_usage(),
        .traffic_out_bps_ = value.traffic_out_bps(),
        .connection_count_ = value.connection_count(),
        .load1m_ = value.load_1m(),
        .queued_log_events_ = static_cast<std::int64_t>(value.queued_log_events()),
        .dropped_log_events_ = static_cast<std::int64_t>(value.dropped_log_events()),
        .health_ = value.health(),
        .last_error_ = value.last_error(),
        .origin_health_ = {},
    };
    result.origin_health_.reserve(value.origin_health_size());
    for (const auto& item : value.origin_health()) {
        result.origin_health_.push_back({.website_id_ = item.website_id(),
            .origin_id_ = item.origin_id(),
            .status_ = item.status(),
            .checked_at_unix_millis_ = item.checked_at_unix_millis(),
            .latency_millis_ = item.latency_millis(),
            .last_error_ = item.last_error()});
    }
    return result;
}

}  // namespace service::agent
