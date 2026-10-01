#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>

#include <asio/ip/address.hpp>
#include <ruvia/web/Model.h>
#include <ruvia/web/Validation.h>

#include "service/common/ip_address.h"
#include "service/common/uuid.h"

namespace service::node_config {

inline constexpr std::size_t kMaxNodeEndpoints{8};

RUVIA_MODEL(NodeEndpointInput,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("ip_address", ipAddress, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String));
RUVIA_MODEL(NodeConfigInput,
    RUVIA_OPTIONAL_FIELD(endpoints, ruvia::Array<NodeEndpointInput>));
RUVIA_MODEL(NodeEndpointOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("ip_address", ipAddress, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String));
RUVIA_MODEL(NodeConfigOutput,
    RUVIA_REQUIRED_FIELD(endpoints, ruvia::Array<NodeEndpointOutput>));

[[nodiscard]] inline bool hasUniqueEndpoints(const ruvia::Array<NodeEndpointInput>& values) {
    std::unordered_set<std::string> ids;
    std::set<asio::ip::address> addresses;
    ids.reserve(values.size());
    for (const auto& endpoint : values) {
        const auto& id = endpoint.get<"id">();
        const auto& address = endpoint.get<"ipAddress">();
        const auto parsedId = service::common::parseUuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        if (!parsedId || !address || !ids.insert(*parsedId).second) return false;
        const auto parsedAddress = service::common::parseIpAddress(address->view());
        if (!parsedAddress || !addresses.insert(*parsedAddress).second) return false;
    }
    return true;
}

inline void validateEndpoint(const NodeEndpointInput& endpoint, std::string_view path,
                             ruvia::Validator& validator) {
    const auto& id = endpoint.get<"id">();
    if (!id || !service::common::parseUuid(
                   id ? std::optional<std::string_view>{id->view()} : std::nullopt)) {
        validator.add(std::string(path) + ".id", "format", "Endpoint ID 不正确");
    }

    const auto& ipAddress = endpoint.get<"ipAddress">();
    const auto ipPath = std::string(path) + ".ip_address";
    if (!ipAddress || ipAddress->empty()) {
        validator.add(ipPath, "required", "IP 地址不能为空");
    } else if (ipAddress->size() > 45 || !service::common::parseIpAddress(ipAddress->view())) {
        validator.add(ipPath, "format", "IP 地址格式不正确");
    }

    const auto& lineCode = endpoint.get<"lineCode">();
    const auto linePath = std::string(path) + ".line_code";
    if (!lineCode || lineCode->empty()) {
        validator.add(linePath, "required", "请选择 DNS 线路");
    } else if (lineCode->size() > 64) {
        validator.add(linePath, "too_big", "DNS 线路不正确");
    }
}

inline void validate(const NodeConfigInput& config, ruvia::Validator& validator) {
    const auto& endpoints = config.get<"endpoints">();
    validator.required(endpoints, "config.endpoints", "请至少填写一个 IP");
    if (!endpoints) return;
    if (endpoints->empty()) {
        validator.add("config.endpoints", "too_small", "请至少填写一个 IP");
    } else if (endpoints->size() > kMaxNodeEndpoints) {
        validator.add("config.endpoints", "too_big", "最多配置8个 IP");
    }
    if (!hasUniqueEndpoints(*endpoints)) {
        validator.add("config.endpoints", "custom", "Endpoint ID 和 IP 地址不能重复");
    }
    for (std::size_t index = 0; index < endpoints->size(); ++index) {
        validateEndpoint((*endpoints)[index],
            "config.endpoints[" + std::to_string(index) + "]", validator);
    }
}

} // namespace service::node_config
