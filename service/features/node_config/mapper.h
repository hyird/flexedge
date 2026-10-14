#pragma once

#include <ruvia/web/model_json.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/node_config/model.h"
#include "service/features/node_config/transport.h"

namespace service::node_config {

[[nodiscard]] inline std::optional<node_endpoint_data> normalize(const node_endpoint_input& input) {
    const auto& id = input.get<"id">();
    const auto& ip_address = input.get<"ip_address">();
    const auto& line_code = input.get<"line_code">();
    if (!id || !ip_address || !line_code) {
        return std::nullopt;
    }
    return node_endpoint_data{.id_ = std::string(id->view()),
        .ip_address_ = std::string(ip_address->view()),
        .line_code_ = std::string(line_code->view())};
}

[[nodiscard]] inline std::optional<node_config_data> normalize(const node_config_input& input) {
    const auto& endpoints = input.get<"endpoints">();
    if (!endpoints) {
        return std::nullopt;
    }
    node_config_data result;
    result.endpoints_.reserve(endpoints->size());
    for (const auto& endpoint : *endpoints) {
        auto normalized = normalize(endpoint);
        if (!normalized) {
            return std::nullopt;
        }
        result.endpoints_.push_back(std::move(*normalized));
    }
    return result;
}

[[nodiscard]] inline bool complete(const node_endpoint_input& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline bool complete(const node_config_input& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline std::optional<node_config_data>
parse_stored(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<node_config_input> input = ruvia::from_json<node_config_input>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline node_config_output to_output(const node_config_data& input,
    ruvia::model_options options = {}) {
    node_config_output output(options);
    auto& endpoints = output.ensure<"endpoints">();
    endpoints.reserve(input.endpoints_.size());
    for (const auto& endpoint : input.endpoints_) {
        auto& item = endpoints.emplace_back(options);
        item.set<"id">(endpoint.id_);
        item.set<"ip_address">(endpoint.ip_address_);
        item.set<"line_code">(endpoint.line_code_);
    }
    return output;
}

}  // namespace service::node_config
