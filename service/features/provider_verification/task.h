#pragma once

#include <cstdint>
#include <string>

namespace service::provider_verification::detail {

struct verification_task final {
    std::string id_;
    std::string tenant_id_;
    std::string provider_id_;
    std::string kind_;
    std::string provider_;
    std::string name_;
    std::string account_id_;
    std::int64_t provider_revision_;
    std::int64_t generation_;
    std::int64_t failures_;
    std::string config_json_;
    std::string runtime_json_;
};

}  // namespace service::provider_verification::detail
