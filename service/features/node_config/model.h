#pragma once

#include <string>
#include <vector>

namespace service::node_config {

struct node_endpoint_data final {
    std::string id_;
    std::string ip_address_;
    std::string line_code_;
};

struct node_config_data final {
    std::vector<node_endpoint_data> endpoints_;
};

}  // namespace service::node_config
