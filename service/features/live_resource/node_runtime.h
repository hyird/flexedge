#pragma once

#include <ruvia/web/model_json.h>

#include <string>
#include <unordered_set>
#include <utility>

#include "ruvia/web/model.h"

#include "service/domains/agent/agent_types.h"
#include "service/domains/agent/heartbeat_store.h"
#include "service/domains/node/node_types.h"
#include "service/domains/website/website_types.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/node_runtime/mapper.h"

namespace service::live_resource {

RUVIA_MODEL(node_runtime_patch, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_revision", node_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD(runtime, service::node::node_runtime_dto));
RUVIA_MODEL(
    origin_runtime_patch, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_id", node_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_revision", node_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("reported_at", reported_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("origin_states", origin_states,
        ruvia::array<service::website::website_origin_runtime_dto>));

template <typename runtime_type>
std::string runtime_patch(runtime_type& c, const service::agent::heartbeat_report& report,
    const service::agent::heartbeat_update& update) {
    service::node::node_runtime_dto runtime({.resource_ = c.pool()});
    runtime.set<"registration_status">("registered");
    runtime.set<"connection_status">(update.status_ == "enabled" ? "online" : "offline");
    runtime.set<"last_heartbeat_at">(update.at_);
    runtime.set<"applied_node_spec_revision">(report.applied_node_spec_revision_);
    runtime.set<"active_release_id">(report.active_release_id_);
    runtime.set<"active_manifest_digest">(report.active_manifest_digest_);
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
    node_runtime_patch patch({.resource_ = c.pool()});
    patch.set<"id">(report.node_id_);
    patch.set<"node_revision">(update.node_revision_);
    patch.set<"runtime">(std::move(runtime));
    const auto json = ruvia::to_json(patch, {.resource_ = c.pool()});
    return std::string(json.data(), json.size());
}

// Adds the origin-runtime patch of every website this heartbeat (or the
// node's previous report) covers to the heartbeat's notification.
template <typename runtime_type>
void add_origin_runtime(runtime_type& c, notifications::resource_notification& notification,
    const service::agent::heartbeat_report& report, const service::agent::heartbeat_update& update) {
    std::unordered_set<std::string> websites;
    for (const auto& state : report.origin_health_) {
        websites.insert(state.website_id_);
    }
    if (const auto previous = service::node_runtime::parse_stored(update.previous_runtime_,
            {.resource_ = c.pool()})) {
        for (const auto& state : previous->origin_health_) {
            websites.insert(state.website_id_);
        }
    }
    for (const auto& website : websites) {
        origin_runtime_patch patch({.resource_ = c.pool()});
        patch.set<"id">(website);
        patch.set<"node_id">(report.node_id_);
        patch.set<"node_revision">(update.node_revision_);
        patch.set<"reported_at">(update.at_);
        auto& states = patch.ensure<"origin_states">();
        if (update.status_ == "enabled") {
            for (const auto& state : report.origin_health_) {
                if (state.website_id_ != website) {
                    continue;
                }
                auto& item = states.emplace_back(ruvia::model_options{.resource_ = c.pool()});
                item.set<"node_id">(report.node_id_);
                item.set<"node_name">(update.node_name_);
                item.set<"origin_id">(state.origin_id_);
                item.set<"status">(state.status_);
                item.set<"checked_at_unix_millis">(state.checked_at_unix_millis_);
                item.set<"latency_millis">(state.latency_millis_);
                if (!state.last_error_.empty()) {
                    item.set<"last_error">(state.last_error_);
                }
            }
        }
        const auto json = ruvia::to_json(patch, {.resource_ = c.pool()});
        notification.origin_runtime(website, std::string(json.data(), json.size()));
    }
}

}  // namespace service::live_resource
