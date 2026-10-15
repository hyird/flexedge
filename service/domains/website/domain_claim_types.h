#pragma once

#include <optional>
#include <string>
#include <vector>

namespace service::website {

struct domain_claim {
    std::string id_;
    std::string key_;
    std::string dns_mode_;
    std::optional<std::string> dns_zone_id_{};
};

struct dns_zone_reference {
    std::string id_;
    std::string domain_;
};

// Configured names of an available certificate (also known before issuance).
struct certificate_reference {
    std::string id_;
    std::vector<std::string> names_;
};

}  // namespace service::website
