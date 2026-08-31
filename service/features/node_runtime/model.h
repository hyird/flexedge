#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::node_runtime {

RUVIA_REQUEST_MODEL(OriginHealthInput,
                    RUVIA_OPTIONAL_FIELD_NAME("website_id", websiteId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("origin_id", originId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("checked_at_unix_millis", checkedAtUnixMillis,
                                              ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("latency_millis", latencyMillis, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));
RUVIA_RESPONSE_MODEL(
    OriginHealthOutput, RUVIA_REQUIRED_FIELD_NAME("website_id", websiteId, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("origin_id", originId, ruvia::String),
    RUVIA_REQUIRED_FIELD(status, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("checked_at_unix_millis", checkedAtUnixMillis, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("latency_millis", latencyMillis, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));

RUVIA_REQUEST_MODEL(NodeRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("agent_version", agentVersion, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("cpu_usage", cpuUsage, ruvia::Double),
                    RUVIA_OPTIONAL_FIELD_NAME("memory_usage", memoryUsage, ruvia::Double),
                    RUVIA_OPTIONAL_FIELD_NAME("traffic_out_bps", trafficOutBps, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("connection_count", connectionCount, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("load_1m", load1m, ruvia::Double),
                    RUVIA_OPTIONAL_FIELD_NAME("queued_log_events", queuedLogEvents, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("dropped_log_events", droppedLogEvents, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("origin_health", originHealth,
                                              ruvia::Array<OriginHealthInput>),
                    RUVIA_OPTIONAL_FIELD(health, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));

RUVIA_RESPONSE_MODEL(
    NodeRuntimeOutput,
    RUVIA_OPTIONAL_FIELD_NAME("agent_version", agentVersion, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_usage", cpuUsage, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("memory_usage", memoryUsage, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("traffic_out_bps", trafficOutBps, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("connection_count", connectionCount, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("load_1m", load1m, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("queued_log_events", queuedLogEvents, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("dropped_log_events", droppedLogEvents, ruvia::Int64,
                              RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("origin_health", originHealth, ruvia::Array<OriginHealthOutput>),
    RUVIA_OPTIONAL_FIELD(health, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));

struct NodeRuntimeData final {
    struct OriginHealth final {
        std::string websiteId;
        std::string originId;
        std::string status;
        std::int64_t checkedAtUnixMillis{};
        std::int64_t latencyMillis{};
        std::optional<std::string> lastError;
    };
    std::optional<std::string> agentVersion;
    std::optional<double> cpuUsage;
    std::optional<double> memoryUsage;
    std::optional<std::int64_t> trafficOutBps;
    std::optional<std::int64_t> connectionCount;
    std::optional<double> load1m;
    std::optional<std::int64_t> queuedLogEvents;
    std::optional<std::int64_t> droppedLogEvents;
    std::vector<OriginHealth> originHealth;
    std::optional<std::string> health;
    std::optional<std::string> lastError;
};

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
