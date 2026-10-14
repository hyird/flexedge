#pragma once

#include <ruvia/web/model.h>
#include <ruvia/web/validation.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>

#include <asio/ip/address.hpp>

#include "service/common/ip_address.h"
#include "service/common/uuid.h"

namespace service::node_config {

inline constexpr std::size_t max_node_endpoints{8};

RUVIA_MODEL(node_endpoint_input,
    RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("ip_address", ip_address, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("line_code", line_code, ruvia::string));
RUVIA_MODEL(node_config_input,
    RUVIA_OPTIONAL_FIELD(endpoints, ruvia::array<node_endpoint_input>));
RUVIA_MODEL(node_endpoint_output, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("ip_address", ip_address, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("line_code", line_code, ruvia::string));
RUVIA_MODEL(node_config_output,
    RUVIA_REQUIRED_FIELD(endpoints, ruvia::array<node_endpoint_output>));

[[nodiscard]] inline bool has_unique_endpoints(const ruvia::array<node_endpoint_input>& values) {
    std::unordered_set<std::string> ids;
    std::set<asio::ip::address> addresses;
    ids.reserve(values.size());
    for (const auto& endpoint : values) {
        const auto& id = endpoint.get<"id">();
        const auto& address = endpoint.get<"ip_address">();
        const auto parsed_id = service::common::parse_uuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        if (!parsed_id || !address || !ids.insert(*parsed_id).second) {
            return false;
        }
        const auto parsed_address = service::common::parse_ip_address(address->view());
        if (!parsed_address || !addresses.insert(*parsed_address).second) {
            return false;
        }
    }
    return true;
}

inline void validate_endpoint(const node_endpoint_input& endpoint, std::string_view path,
    ruvia::validator& validator) {
    const auto& id = endpoint.get<"id">();
    if (!id || !service::common::parse_uuid(
                   id ? std::optional<std::string_view>{id->view()} : std::nullopt)) {
        validator.add(std::string(path) + ".id", "format", "Endpoint ID 不正确");
    }

    const auto& ip_address = endpoint.get<"ip_address">();
    const auto ip_path = std::string(path) + ".ip_address";
    if (!ip_address || ip_address->empty()) {
        validator.add(ip_path, "required", "IP 地址不能为空");
    } else if (ip_address->size() > 45 || !service::common::parse_ip_address(ip_address->view())) {
        validator.add(ip_path, "format", "IP 地址格式不正确");
    }

    const auto& line_code = endpoint.get<"line_code">();
    const auto line_path = std::string(path) + ".line_code";
    if (!line_code || line_code->empty()) {
        validator.add(line_path, "required", "请选择 DNS 线路");
    } else if (line_code->size() > 64) {
        validator.add(line_path, "too_big", "DNS 线路不正确");
    }
}

inline void validate(const node_config_input& config, ruvia::validator& validator) {
    const auto& endpoints = config.get<"endpoints">();
    validator.required(endpoints, "config.endpoints", "请至少填写一个 IP");
    if (!endpoints) {
        return;
    }
    if (endpoints->empty()) {
        validator.add("config.endpoints", "too_small", "请至少填写一个 IP");
    } else if (endpoints->size() > max_node_endpoints) {
        validator.add("config.endpoints", "too_big", "最多配置8个 IP");
    }
    if (!has_unique_endpoints(*endpoints)) {
        validator.add("config.endpoints", "custom", "Endpoint ID 和 IP 地址不能重复");
    }
    for (std::size_t index = 0; index < endpoints->size(); ++index) {
        validate_endpoint((*endpoints)[index],
            "config.endpoints[" + std::to_string(index) + "]", validator);
    }
}

}  // namespace service::node_config
