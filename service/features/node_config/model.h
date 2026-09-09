#pragma once

#include <string>
#include <vector>

namespace service::node_config {

struct NodeEndpointData final {
    std::string id;
    std::string ipAddress;
    std::string lineCode;
};

struct NodeConfigData final {
    std::vector<NodeEndpointData> endpoints;
};

} // namespace service::node_config
