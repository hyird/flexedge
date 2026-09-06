#pragma once

#include <cstdint>
#include <string>

#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>

#include "service/common/http.h"
#include "service/domains/node/node.types.h"
#include "service/features/node_config/model.h"
#include "service/features/node_runtime/model.h"

namespace service::node {

[[noreturn]] inline void throwCorruptNodeConfig() {
    service::common::throwAppError(service::common::kServerErrorCode, "聚合配置损坏", 500);
}

inline std::string serializeNodeConfig(ruvia::Context& c,
                                       const service::node_config::NodeConfigData& input) {
    const auto output = service::node_config::toOutput(input, {.resource = c.resource()});
    const auto json = ruvia::toJson(output, {.resource = c.resource()});
    return std::string(json.data(), json.size());
}

template <typename Row>
inline NodeRuntimeDto nodeRuntimeDto(ruvia::Context& c,
                                     const service::node_runtime::NodeRuntimeData& input,
                                     const Row& row) {
    NodeRuntimeDto output(c);
    output.set<"registrationStatus">(row[7].value().value_or("pending"));
    output.set<"connectionStatus">(row[8].value().value_or("unregistered"));
    output.set<"appliedNodeSpecRevision">(row[10].template as<std::int64_t>().value_or(0));
    if (const auto& lastHeartbeatAt = row[9].value()) {
        output.set<"lastHeartbeatAt">(*lastHeartbeatAt);
    }
    if (input.agentVersion) {
        output.set<"agentVersion">(*input.agentVersion);
    }
    if (input.cpuUsage) {
        output.set<"cpuUsage">(*input.cpuUsage);
    }
    if (input.memoryUsage) {
        output.set<"memoryUsage">(*input.memoryUsage);
    }
    if (input.trafficOutBps) {
        output.set<"trafficOutBps">(*input.trafficOutBps);
    }
    if (input.connectionCount) {
        output.set<"connectionCount">(*input.connectionCount);
    }
    if (input.load1m) {
        output.set<"load1m">(*input.load1m);
    }
    if (input.queuedLogEvents) {
        output.set<"queuedLogEvents">(*input.queuedLogEvents);
    }
    if (input.droppedLogEvents) {
        output.set<"droppedLogEvents">(*input.droppedLogEvents);
    }
    if (input.health) {
        output.set<"health">(*input.health);
    }
    if (input.lastError) {
        output.set<"lastError">(*input.lastError);
    }
    if (const auto& activeReleaseId = row[15].value()) {
        output.set<"activeReleaseId">(*activeReleaseId);
    }
    if (const auto& activeManifestDigest = row[16].value()) {
        output.set<"activeManifestDigest">(*activeManifestDigest);
    }
    return output;
}

template <typename Row> inline void fillNode(ruvia::Context& c, NodeDto& item, const Row& row) {
    const auto config = service::node_config::parseStored(row[5].value().value_or("{}"),
                                                          {.resource = c.resource()});
    const auto runtime = service::node_runtime::parseStored(row[6].value().value_or("{}"),
                                                            {.resource = c.resource()});
    if (!config || !runtime) {
        throwCorruptNodeConfig();
    }

    item.set<"id">(row[0].value().value_or(""));
    item.set<"clusterId">(row[1].value().value_or(""));
    item.set<"clusterName">(row[2].value().value_or(""));
    item.set<"name">(row[13].value().value_or(""));
    item.set<"status">(row[14].value().value_or(""));
    item.set<"revision">(row[3].template as<std::int64_t>().value_or(1));
    item.set<"nodeSpecRevision">(row[4].template as<std::int64_t>().value_or(1));
    item.set<"config">(service::node_config::toOutput(*config, {.resource = c.resource()}));
    item.set<"runtime">(nodeRuntimeDto(c, *runtime, row));
    item.set<"createdAt">(row[11].value().value_or(""));
    item.set<"updatedAt">(row[12].value().value_or(""));
}

} // namespace service::node
