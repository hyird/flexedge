#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::node_config {

RUVIA_REQUEST_MODEL(NodeEndpointInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("ip_address", ipAddress, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String));
RUVIA_REQUEST_MODEL(NodeConfigInput,
                    RUVIA_OPTIONAL_FIELD(endpoints, ruvia::Array<NodeEndpointInput>));
RUVIA_RESPONSE_MODEL(NodeEndpointOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("ip_address", ipAddress, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String));
RUVIA_RESPONSE_MODEL(NodeConfigOutput,
                     RUVIA_REQUIRED_FIELD(endpoints, ruvia::Array<NodeEndpointOutput>));

struct NodeEndpointData final {
    std::string id;
    std::string ipAddress;
    std::string lineCode;
};

struct NodeConfigData final {
    std::vector<NodeEndpointData> endpoints;
};

[[nodiscard]] inline std::optional<NodeEndpointData> normalize(const NodeEndpointInput& input) {
    const auto& id = input.get<"id">();
    const auto& ipAddress = input.get<"ipAddress">();
    const auto& lineCode = input.get<"lineCode">();
    if (!id || !ipAddress || !lineCode) {
        return std::nullopt;
    }
    return NodeEndpointData{.id = std::string(id->view()),
                            .ipAddress = std::string(ipAddress->view()),
                            .lineCode = std::string(lineCode->view())};
}

[[nodiscard]] inline std::optional<NodeConfigData> normalize(const NodeConfigInput& input) {
    const auto& endpoints = input.get<"endpoints">();
    if (!endpoints) {
        return std::nullopt;
    }

    NodeConfigData result;
    result.endpoints.reserve(endpoints->size());
    for (const auto& endpoint : *endpoints) {
        auto normalized = normalize(endpoint);
        if (!normalized) {
            return std::nullopt;
        }
        result.endpoints.push_back(std::move(*normalized));
    }
    return result;
}

[[nodiscard]] inline bool complete(const NodeEndpointInput& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline bool complete(const NodeConfigInput& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline std::optional<NodeConfigData>
parseStored(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<NodeConfigInput> input = ruvia::fromJson<NodeConfigInput>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline NodeConfigOutput toOutput(const NodeConfigData& input,
                                               ruvia::ModelOptions options = {}) {
    NodeConfigOutput output(options);
    auto& endpoints = output.ensure<"endpoints">();
    endpoints.reserve(input.endpoints.size());
    for (const auto& endpoint : input.endpoints) {
        auto& item = endpoints.emplace_back(options);
        item.set<"id">(endpoint.id);
        item.set<"ipAddress">(endpoint.ipAddress);
        item.set<"lineCode">(endpoint.lineCode);
    }
    return output;
}

} // namespace service::node_config
