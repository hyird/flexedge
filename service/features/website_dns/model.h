#pragma once

#include <optional>
#include <string>
#include <vector>

namespace service::website_dns {

struct website_domain_runtime_data final {
    std::optional<std::string> id_;
    std::optional<std::string> resolution_status_;
    std::optional<std::string> last_verified_at_;
    std::optional<std::string> last_error_;
};

struct website_runtime_data final {
    std::vector<website_domain_runtime_data> domain_states_;
};

}  // namespace service::website_dns
