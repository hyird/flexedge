#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ruvia/web/model.h"

#include "service/domains/agent/agent_types.h"
#include "service/domains/agent/heartbeat_store.h"
#include "service/domains/node/node_connection.h"
#include "service/domains/node/node_types.h"
#include "service/domains/website/website_types.h"
#include "service/features/live_resource/fanout.h"
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

inline std::string runtime_patch(ruvia::context& c, const service::agent::heartbeat_report& report,
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
inline void add_origin_runtime(ruvia::context& c, notifications::resource_notification& notification,
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

// Deadline-driven transitions, not periodic database polling. Every Web
// Worker owns one instance (Ruvia worker state): each heartbeat notification
// its reader delivers replaces one deadline, and only an actual expiration
// wakes that worker's node projections.
class node_deadlines final {
    struct entry {
        std::string tenant_;
        std::chrono::steady_clock::time_point deadline_;
    };

public:
    void touch(std::string_view tenant, std::string_view node,
        std::chrono::steady_clock::duration remaining = service::node::online_window) {
        auto& entry = entries_[std::string(node)];
        entry.tenant_ = std::string(tenant);
        entry.deadline_ = std::max(entry.deadline_, std::chrono::steady_clock::now() + remaining);
        if (sender_) {
            (void)sender_->send(1);
        }
    }

    // Runs on its worker for the worker's lifetime: expires deadlines and
    // delivers each offline transition to the worker's own hub.
    template <typename context_type>
    ruvia::task<void> run(context_type& context) {
        auto [sender, receiver] = ruvia::make_channel<std::uint64_t>(context.worker(), {.capacity_ = 1});
        sender_.emplace(std::move(sender));
        auto& hub_value = get_hub(context);
        while (!context.get_stop_token().stop_requested()) {
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::pair<std::string, std::string>> expired;
            std::optional<std::chrono::steady_clock::time_point> earliest;
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (it->second.deadline_ <= now) {
                    expired.emplace_back(it->second.tenant_, it->first);
                    it = entries_.erase(it);
                    continue;
                }
                earliest = earliest ? std::min(*earliest, it->second.deadline_) : it->second.deadline_;
                ++it;
            }
            for (const auto& [tenant, node] : expired) {
                hub_value.publish(tenant, resource::nodes, node);
            }
            // Without deadlines this only waits for the next touch.
            const auto wait = earliest ? *earliest - now : std::chrono::steady_clock::duration(std::chrono::hours(1));
            const auto signal = co_await receiver.receive_for(wait, context.get_stop_token());
            if (!signal.has_value() && signal.status() != ruvia::worker_wait_status::timed_out) {
                break;
            }
        }
        sender_.reset();
    }

private:
    std::unordered_map<std::string, entry> entries_;
    std::optional<ruvia::channel_sender<std::uint64_t>> sender_;
};

template <typename context_type>
node_deadlines& get_node_deadlines(context_type& context) {
    return context.template worker_state<node_deadlines>();
}

// Online nodes after a restart get the rest of their window from the stored heartbeat.
template <typename context_t_type>
inline ruvia::task<void> seed_node_deadlines(context_t_type& context) {
    ruvia::db_query query;
    const auto heard_seconds_ago = query.call("date_part",
        {query.value("epoch"), query.binary(query.call("now"), ruvia::db_binary_operator::subtract,
                                   query.column("last_heartbeat_at", "node"))});
    query.select({query.column("tenant_id", "node"), query.column("id", "node"), heard_seconds_ago})
        .from("sys_node", "node")
        .where(query.unary(ruvia::db_unary_operator::is_null, query.column("deleted_at", "node")))
        .and_where(service::node::node_online_predicate(query, "node"));
    const auto rows = co_await context.db().query(query);
    auto& deadlines = get_node_deadlines(context);
    for (const auto& row : rows) {
        const auto heard = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(row[2].template as<double>().value_or(0)));
        const auto remaining =
            std::max(std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(service::node::online_window) - heard);
        deadlines.touch(row[0].value().value_or(""), row[1].value().value_or(""), remaining);
    }
}

}  // namespace service::live_resource
