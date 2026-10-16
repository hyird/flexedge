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

}  // namespace service::website
