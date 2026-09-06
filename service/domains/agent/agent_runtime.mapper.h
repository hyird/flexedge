#pragma once

#include <string>

#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>

#include "service/domains/agent/agent.types.h"
#include "service/features/node_runtime/model.h"

namespace service::agent {

inline std::string heartbeatRuntimeJson(ruvia::Context& c, const HeartbeatReport& report) {
    service::node_runtime::NodeRuntimeOutput runtime(c);
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
    auto& originHealth = runtime.ensure<"originHealth">();
    originHealth.reserve(report.originHealth.size());
    for (const auto& reportItem : report.originHealth) {
        auto& item = originHealth.emplace_back(c);
        item.set<"websiteId">(reportItem.websiteId);
        item.set<"originId">(reportItem.originId);
        item.set<"status">(reportItem.status);
        item.set<"checkedAtUnixMillis">(reportItem.checkedAtUnixMillis);
        item.set<"latencyMillis">(reportItem.latencyMillis);
        if (!reportItem.lastError.empty()) {
            item.set<"lastError">(reportItem.lastError);
        }
    }
    const auto json = ruvia::toJson(runtime, {.resource = c.resource()});
    return std::string(json.data(), json.size());
}

} // namespace service::agent
