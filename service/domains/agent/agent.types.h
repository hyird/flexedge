#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace service::agent {

struct AgentPrincipal final {
    std::string nodeId, clusterId, tenantId, agentId;
};
struct DesiredSummary final {
    std::int64_t nodeSpecRevision{};
    std::string releaseId, manifestDigest;
};
struct HeartbeatReport final {
    struct OriginHealth final {
        std::string websiteId, originId, status;
        std::int64_t checkedAtUnixMillis{}, latencyMillis{};
        std::string lastError;
    };
    std::string nodeId;
    std::int64_t appliedNodeSpecRevision{};
    std::string activeReleaseId, activeManifestDigest, agentVersion;
    double cpuUsage{}, memoryUsage{};
    std::int64_t trafficOutBps{}, connectionCount{};
    double load1m{};
    std::int64_t queuedLogEvents{}, droppedLogEvents{};
    std::string health, lastError;
    std::vector<OriginHealth> originHealth;
};

} // namespace service::agent
