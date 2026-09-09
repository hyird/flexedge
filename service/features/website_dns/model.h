#pragma once

#include <optional>
#include <string>
#include <vector>

namespace service::website_dns {

struct WebsiteDomainRuntimeData final {
    std::optional<std::string> id;
    std::optional<std::string> resolutionStatus;
    std::optional<std::string> lastVerifiedAt;
    std::optional<std::string> lastError;
};

struct WebsiteRuntimeData final {
    std::vector<WebsiteDomainRuntimeData> domainStates;
};

} // namespace service::website_dns
