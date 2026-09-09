#pragma once

#include <optional>
#include <string>

namespace service::website {

struct DomainClaim {
    std::string id;
    std::string key;
    std::string dnsMode;
    std::optional<std::string> dnsZoneId{};
};

struct DnsZoneReference {
    std::string id;
    std::string domain;
};

} // namespace service::website
