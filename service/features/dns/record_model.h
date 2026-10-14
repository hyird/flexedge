#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace service::dns {

struct provider_zone final {
    std::string id_;
    std::string name_;
    std::string status_;
};

struct provider_record final {
    std::string id_;
    std::string type_;
    std::string name_;
    std::string content_;
    std::int64_t ttl_;
    std::optional<std::int64_t> priority_;
    bool proxied_;
    std::string line_code_;
};

struct provider_line final {
    std::string code_;
    std::string name_;
    std::string display_name_;
};

}  // namespace service::dns
