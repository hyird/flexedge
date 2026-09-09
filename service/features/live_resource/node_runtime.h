#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "service/features/live_resource/fanout.h"
#include "service/domains/node/node.types.h"
#include "service/domains/agent/agent.types.h"
#include "service/domains/agent/heartbeat.store.h"
#include "service/domains/website/website.types.h"
#include "service/features/node_runtime/mapper.h"
#include <ruvia/web/ModelJson.h>

namespace service::live_resource {

RUVIA_RESPONSE_MODEL(NodeRuntimePatch, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("node_revision", nodeRevision, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(runtime, service::node::NodeRuntimeDto));
RUVIA_RESPONSE_MODEL(
    OriginRuntimePatch, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("node_id", nodeId, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("node_revision", nodeRevision, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("reported_at", reportedAt, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("origin_states", originStates,
                              ruvia::Array<service::website::WebsiteOriginRuntimeDto>));

inline std::string runtimePatch(ruvia::Context& c, const service::agent::HeartbeatReport& report,
                                const service::agent::HeartbeatUpdate& update) {
    service::node::NodeRuntimeDto runtime(c);
    runtime.set<"registrationStatus">("registered");
    runtime.set<"connectionStatus">(update.status == "enabled" ? "online" : "offline");
    runtime.set<"lastHeartbeatAt">(update.at);
    runtime.set<"appliedNodeSpecRevision">(report.appliedNodeSpecRevision);
    runtime.set<"activeReleaseId">(report.activeReleaseId);
    runtime.set<"activeManifestDigest">(report.activeManifestDigest);
    runtime.set<"agentVersion">(report.agentVersion);
    runtime.set<"cpuUsage">(report.cpuUsage);
    runtime.set<"memoryUsage">(report.memoryUsage);
    runtime.set<"trafficOutBps">(report.trafficOutBps);
    runtime.set<"connectionCount">(report.connectionCount);
    runtime.set<"load1m">(report.load1m);
    runtime.set<"queuedLogEvents">(report.queuedLogEvents);
    runtime.set<"droppedLogEvents">(report.droppedLogEvents);
    runtime.set<"health">(report.health);
    runtime.set<"lastError">(report.lastError);
    NodeRuntimePatch patch(c);
    patch.set<"id">(report.nodeId);
    patch.set<"nodeRevision">(update.nodeRevision);
    patch.set<"runtime">(std::move(runtime));
    const auto json = ruvia::toJson(patch, {.resource = c.resource()});
    return std::string(json.data(), json.size());
}

inline void publishOriginRuntime(ruvia::Context& c, std::string_view tenant,
                                 const service::agent::HeartbeatReport& report,
                                 const service::agent::HeartbeatUpdate& update) {
    std::unordered_set<std::string> websites;
    for (const auto& state : report.originHealth)
        websites.insert(state.websiteId);
    if (const auto previous = service::node_runtime::parseStored(update.previousRuntime,
                                                                 {.resource = c.resource()})) {
        for (const auto& state : previous->originHealth)
            websites.insert(state.websiteId);
    }
    for (const auto& website : websites) {
        OriginRuntimePatch patch(c);
        patch.set<"id">(website);
        patch.set<"nodeId">(report.nodeId);
        patch.set<"nodeRevision">(update.nodeRevision);
        patch.set<"reportedAt">(update.at);
        auto& states = patch.ensure<"originStates">();
        if (update.status == "enabled") {
            for (const auto& state : report.originHealth) {
                if (state.websiteId != website)
                    continue;
                auto& item = states.emplace_back(c);
                item.set<"nodeId">(report.nodeId);
                item.set<"nodeName">(update.nodeName);
                item.set<"originId">(state.originId);
                item.set<"status">(state.status);
                item.set<"checkedAtUnixMillis">(state.checkedAtUnixMillis);
                item.set<"latencyMillis">(state.latencyMillis);
                if (!state.lastError.empty())
                    item.set<"lastError">(state.lastError);
            }
        }
        const auto json = ruvia::toJson(patch, {.resource = c.resource()});
        hub().publishOrigins(tenant, website, report.nodeId, json);
    }
}

// Deadline-driven transitions, not periodic database polling. Every accepted
// heartbeat replaces one deadline; only an actual expiration wakes node reads.
class NodeDeadlines final {
    struct Entry {
        std::string tenant;
        std::chrono::steady_clock::time_point deadline;
    };

  public:
    NodeDeadlines() : thread_([this](std::stop_token stop) { run(stop); }) {}
    ~NodeDeadlines() {
        thread_.request_stop();
        changed_.notify_all();
    }
    void touch(std::string_view tenant, std::string_view node,
               std::chrono::steady_clock::duration remaining = std::chrono::seconds(90)) {
        const std::lock_guard lock(mutex_);
        auto& entry = entries_[std::string(node)];
        entry.tenant = std::string(tenant);
        entry.deadline = std::max(entry.deadline, std::chrono::steady_clock::now() + remaining);
        changed_.notify_all();
    }

  private:
    void run(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            auto earliest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it)
                if (earliest == entries_.end() || it->second.deadline < earliest->second.deadline)
                    earliest = it;
            if (earliest == entries_.end()) {
                changed_.wait(lock);
                continue;
            }
            const auto deadline = earliest->second.deadline;
            if (deadline > std::chrono::steady_clock::now()) {
                changed_.wait_until(lock, deadline);
                continue;
            }
            auto node = earliest->first;
            auto tenant = earliest->second.tenant;
            entries_.erase(earliest);
            lock.unlock();
            hub().publish(tenant, Resource::nodes, node);
            hub().publish(tenant, Resource::overview);
            hub().publish(tenant, Resource::clusters);
            lock.lock();
        }
    }
    std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<std::string, Entry> entries_;
    std::jthread thread_;
};
inline NodeDeadlines& nodeDeadlines() {
    (void)hub(); // Ensure the deadline thread is destroyed before its hub.
    static NodeDeadlines value;
    return value;
}

inline ruvia::Task<void> seedNodeDeadlines(ruvia::WebWorkerContext& context) {
    const auto rows = co_await context.db().query(
        "SELECT tenant_id, id, GREATEST(1, CEIL(EXTRACT(EPOCH FROM "
        "(last_heartbeat_at + INTERVAL '90 seconds' - NOW())) * 1000))::bigint "
        "FROM sys_node WHERE deleted_at IS NULL AND registration_status = 'registered' "
        "AND status = 'enabled' AND last_heartbeat_at >= NOW() - INTERVAL '90 seconds'");
    for (const auto& row : rows) {
        nodeDeadlines().touch(row[0].value().value_or(""), row[1].value().value_or(""),
                              std::chrono::milliseconds(row[2].as<std::int64_t>().value_or(1)));
    }
}

} // namespace service::live_resource
