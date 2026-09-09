#pragma once
#include <cstdint>
#include "node/proto/edge_control.pb.h"
#include "service/domains/agent/agent.types.h"
namespace service::agent {
inline HeartbeatReport toHeartbeatReport(const flexedge::node::v2::Heartbeat& value) {
    HeartbeatReport result{
        .nodeId = value.node_id(),
        .appliedNodeSpecRevision = value.applied_node_spec_revision(),
        .activeReleaseId = value.active_release_id(),
        .activeManifestDigest = value.active_manifest_digest(),
        .agentVersion = value.agent_version(),
        .cpuUsage = value.cpu_usage(),
        .memoryUsage = value.memory_usage(),
        .trafficOutBps = value.traffic_out_bps(),
        .connectionCount = value.connection_count(),
        .load1m = value.load_1m(),
        .queuedLogEvents = static_cast<std::int64_t>(value.queued_log_events()),
        .droppedLogEvents = static_cast<std::int64_t>(value.dropped_log_events()),
        .health = value.health(),
        .lastError = value.last_error(),
        .originHealth = {},
    };
    result.originHealth.reserve(value.origin_health_size());
    for (const auto& item : value.origin_health()) {
        result.originHealth.push_back({.websiteId = item.website_id(),
                                       .originId = item.origin_id(),
                                       .status = item.status(),
                                       .checkedAtUnixMillis = item.checked_at_unix_millis(),
                                       .latencyMillis = item.latency_millis(),
                                       .lastError = item.last_error()});
    }
    return result;
}

} // namespace service::agent
