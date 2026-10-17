#pragma once

#include <ruvia/web/model_json.h>

#include <string>
#include <unordered_set>
#include <utility>

#include "ruvia/web/model.h"

#include "service/features/live_resource/notifications.h"
#include "service/features/node_channel/heartbeat_store.h"
#include "service/features/node_channel/types.h"
#include "service/features/node_runtime/mapper.h"
#include "service/features/node_runtime/transport.h"

namespace service::node_channel {

RUVIA_MODEL(node_runtime_patch, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_revision", node_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD(runtime, service::node_runtime::node_runtime_dto));
RUVIA_MODEL(
    origin_runtime_patch, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_id", node_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_revision", node_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("reported_at", reported_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("origin_states", origin_states,
        ruvia::array<service::node_runtime::website_origin_runtime_dto>));

// The reported runtime fields the stored runtime and the node DTO share.
template <typename runtime_model>
void set_reported_runtime(runtime_model& runtime, const heartbeat_report& report) {
    runtime.template set<"agent_version">(report.agent_version_);
    runtime.template set<"cpu_usage">(report.cpu_usage_);
    runtime.template set<"memory_usage">(report.memory_usage_);
    if (report.cpu_count_) {
        runtime.template set<"cpu_count">(*report.cpu_count_);
    }
    if (report.memory_total_bytes_) {
        runtime.template set<"memory_total_bytes">(*report.memory_total_bytes_);
    }
    if (report.disk_total_bytes_) {
        runtime.template set<"disk_total_bytes">(*report.disk_total_bytes_);
    }
    runtime.template set<"traffic_out_bps">(report.traffic_out_bps_);
    runtime.template set<"connection_count">(report.connection_count_);
    if (report.connected_ip_count_) {
        runtime.template set<"connected_ip_count">(*report.connected_ip_count_);
    }
    runtime.template set<"load1m">(report.load1m_);
    runtime.template set<"queued_log_events">(report.queued_log_events_);
    runtime.template set<"dropped_log_events">(report.dropped_log_events_);
    runtime.template set<"health">(report.health_);
    runtime.template set<"last_error">(report.last_error_);
}

// The `sys_node.runtime` JSON a heartbeat stores.
template <typename runtime_type>
std::string stored_runtime_json(runtime_type& c, const heartbeat_report& report) {
    service::node_runtime::node_runtime_output runtime({.resource_ = c.pool()});
    set_reported_runtime(runtime, report);
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

// The node runtime patch a heartbeat publishes.
template <typename runtime_type>
std::string runtime_patch(runtime_type& c, const heartbeat_report& report, const heartbeat_update& update) {
    service::node_runtime::node_runtime_dto runtime({.resource_ = c.pool()});
    runtime.set<"registration_status">("registered");
    runtime.set<"connection_status">(update.status_ == "enabled" ? "online" : "offline");
    runtime.set<"last_heartbeat_at">(update.at_);
    runtime.set<"applied_node_spec_revision">(report.applied_node_spec_revision_);
    runtime.set<"active_release_id">(report.active_release_id_);
    runtime.set<"active_manifest_digest">(report.active_manifest_digest_);
    set_reported_runtime(runtime, report);
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
void add_origin_runtime(runtime_type& c, service::live_resource::notifications::resource_notification& notification,
    const heartbeat_report& report, const heartbeat_update& update) {
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

}  // namespace service::node_channel
