#pragma once

#include <ruvia/web/model_json.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/website_dns/model.h"
#include "service/features/website_dns/transport.h"

namespace service::website_dns {

[[nodiscard]] inline website_runtime_data normalize(const website_runtime_input& input) {
    website_runtime_data result;
    const auto& states = input.get<"domain_states">();
    if (!states) {
        return result;
    }

    result.domain_states_.reserve(states->size());
    for (const auto& state : *states) {
        website_domain_runtime_data item;
        if (const auto& value = state.get<"id">()) {
            item.id_ = std::string(value->view());
        }
        if (const auto& value = state.get<"resolution_status">()) {
            item.resolution_status_ = std::string(value->view());
        }
        if (const auto& value = state.get<"last_verified_at">()) {
            item.last_verified_at_ = std::string(value->view());
        }
        if (const auto& value = state.get<"last_error">()) {
            item.last_error_ = std::string(value->view());
        }
        result.domain_states_.push_back(std::move(item));
    }
    return result;
}

[[nodiscard]] inline std::optional<website_runtime_data>
parse_stored(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<website_runtime_input> input =
        ruvia::from_json<website_runtime_input>(json, options);
    return input ? std::optional<website_runtime_data>{normalize(*input)} : std::nullopt;
}

[[nodiscard]] inline website_runtime_output to_output(const website_runtime_data& input,
    ruvia::model_options options = {}) {
    website_runtime_output output(options);
    auto& states = output.ensure<"domain_states">();
    states.reserve(input.domain_states_.size());
    for (const auto& state : input.domain_states_) {
        if (!state.id_ || !state.resolution_status_) {
            throw std::invalid_argument("incomplete website domain runtime state");
        }
        auto& item = states.emplace_back(options);
        item.set<"id">(*state.id_);
        item.set<"resolution_status">(*state.resolution_status_);
        if (state.last_verified_at_) {
            item.set<"last_verified_at">(*state.last_verified_at_);
        }
        if (state.last_error_) {
            item.set<"last_error">(*state.last_error_);
        }
    }
    return output;
}

}  // namespace service::website_dns
