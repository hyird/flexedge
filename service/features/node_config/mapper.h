#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/ModelJson.h>

#include "service/features/node_config/model.h"
#include "service/features/node_config/transport.h"

namespace service::node_config {

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
