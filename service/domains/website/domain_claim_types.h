#pragma once

#include <optional>
#include <string>

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

}  // namespace service::website
