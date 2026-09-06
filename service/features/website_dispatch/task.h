#pragma once

#include <cstdint>
#include <string>

namespace service::website_dispatch::detail {

struct WebsiteMarker final {
    std::string id;
    std::string tenantId;
    std::string resourceId;
    std::int64_t version{};
    std::int64_t failures{};
};

} // namespace service::website_dispatch::detail
