#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/ModelJson.h>

#include "service/features/node_runtime/model.h"
#include "service/features/node_runtime/transport.h"

namespace service::node_runtime {

[[nodiscard]] inline NodeRuntimeData normalize(const NodeRuntimeInput& input) {
    NodeRuntimeData result;
    if (const auto& value = input.get<"agentVersion">()) {
        result.agentVersion = std::string(value->view());
    }
    if (const auto& value = input.get<"cpuUsage">()) {
        result.cpuUsage = value->value;
    }
    if (const auto& value = input.get<"memoryUsage">()) {
        result.memoryUsage = value->value;
    }
    if (const auto& value = input.get<"trafficOutBps">()) {
        result.trafficOutBps = value->value;
    }
    if (const auto& value = input.get<"connectionCount">()) {
        result.connectionCount = value->value;
    }
    if (const auto& value = input.get<"load1m">()) {
        result.load1m = value->value;
    }
    if (const auto& value = input.get<"queuedLogEvents">()) {
        result.queuedLogEvents = value->value;
    }
    if (const auto& value = input.get<"droppedLogEvents">()) {
        result.droppedLogEvents = value->value;
    }
    if (const auto& values = input.get<"originHealth">()) {
        result.originHealth.reserve(values->size());
        for (const auto& value : *values) {
            const auto& websiteId = value.get<"websiteId">();
            const auto& originId = value.get<"originId">();
            const auto& status = value.get<"status">();
            const auto& checkedAtUnixMillis = value.get<"checkedAtUnixMillis">();
            const auto& latencyMillis = value.get<"latencyMillis">();
            if (!websiteId || !originId || !status || !checkedAtUnixMillis || !latencyMillis) {
                continue;
            }
            NodeRuntimeData::OriginHealth item{.websiteId = std::string(websiteId->view()),
                                               .originId = std::string(originId->view()),
                                               .status = std::string(status->view()),
                                               .checkedAtUnixMillis = checkedAtUnixMillis->value,
                                               .latencyMillis = latencyMillis->value,
                                               .lastError = std::nullopt};
            if (const auto& lastError = value.get<"lastError">()) {
                item.lastError = std::string(lastError->view());
            }
            result.originHealth.push_back(std::move(item));
        }
    }
    if (const auto& value = input.get<"health">()) {
        result.health = std::string(value->view());
    }
    if (const auto& value = input.get<"lastError">()) {
        result.lastError = std::string(value->view());
    }
    return result;
}

[[nodiscard]] inline std::optional<NodeRuntimeData>
parseStored(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<NodeRuntimeInput> input = ruvia::fromJson<NodeRuntimeInput>(json, options);
    return input ? std::optional<NodeRuntimeData>{normalize(*input)} : std::nullopt;
}

} // namespace service::node_runtime
