#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace service::node_dispatch {

struct AvailableCertificate final {
    std::string id;
    std::vector<std::string> domains;
    std::string certificateChainPem;
    std::string privateKeyEnvelope;
};

struct DeploymentWebsiteSource final {
    std::string id;
    std::int64_t revision;
    bool enabled;
    std::string configJson;
};

struct ClusterDeploymentSource final {
    std::string clusterId;
    std::string tenantId;
    std::string accessDomain;
    bool enabled;
    std::vector<DeploymentWebsiteSource> websites;
    std::unordered_map<std::string, std::vector<AvailableCertificate>> certificatesByWebsite;
};

} // namespace service::node_dispatch
