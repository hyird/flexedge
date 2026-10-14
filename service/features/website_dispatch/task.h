#pragma once

#include <cstdint>
#include <string>

namespace service::website_dispatch::detail {

struct website_marker final {
    std::string id_;
    std::string tenant_id_;
    std::string resource_id_;
    std::int64_t version_{};
    std::int64_t failures_{};
};

}  // namespace service::website_dispatch::detail
