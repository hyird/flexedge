#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace service::dns {

struct ProviderZone final {
    std::string id;
    std::string name;
    std::string status;
};

struct ProviderRecord final {
    std::string id;
    std::string type;
    std::string name;
    std::string content;
    std::int64_t ttl;
    std::optional<std::int64_t> priority;
    bool proxied;
    std::string lineCode;
};

struct ProviderLine final {
    std::string code;
    std::string name;
    std::string displayName;
};

} // namespace service::dns
