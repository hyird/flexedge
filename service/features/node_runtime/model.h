#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace service::node_runtime {

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

} // namespace service::node_runtime
