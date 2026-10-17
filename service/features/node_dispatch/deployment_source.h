#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/cache_policy.h"

namespace service::node_dispatch {

struct available_certificate final {
    std::string id_;
    std::vector<std::string> domains_;
    std::string certificate_chain_pem_;
    std::string private_key_envelope_;
};

struct deployment_website_source final {
    std::string id_;
    std::int64_t revision_;
    bool enabled_;
    std::string config_json_;
    std::optional<flexedge::cache::policy> response_cache_{};
};

struct cluster_deployment_source final {
    std::string cluster_id_;
    std::string tenant_id_;
    std::string access_domain_;
    bool enabled_;
    std::vector<deployment_website_source> websites_;
    std::unordered_map<std::string, std::vector<available_certificate>> certificates_by_website_;
};

}  // namespace service::node_dispatch
