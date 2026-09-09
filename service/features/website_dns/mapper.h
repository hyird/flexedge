#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/ModelJson.h>

#include "service/features/website_dns/model.h"
#include "service/features/website_dns/transport.h"

namespace service::website_dns {

[[nodiscard]] inline WebsiteRuntimeData normalize(const WebsiteRuntimeInput& input) {
    WebsiteRuntimeData result;
    const auto& states = input.get<"domainStates">();
    if (!states) {
        return result;
    }

    result.domainStates.reserve(states->size());
    for (const auto& state : *states) {
        WebsiteDomainRuntimeData item;
        if (const auto& value = state.get<"id">()) {
            item.id = std::string(value->view());
        }
        if (const auto& value = state.get<"resolutionStatus">()) {
            item.resolutionStatus = std::string(value->view());
        }
        if (const auto& value = state.get<"lastVerifiedAt">()) {
            item.lastVerifiedAt = std::string(value->view());
        }
        if (const auto& value = state.get<"lastError">()) {
            item.lastError = std::string(value->view());
        }
        result.domainStates.push_back(std::move(item));
    }
    return result;
}

[[nodiscard]] inline std::optional<WebsiteRuntimeData>
parseStored(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<WebsiteRuntimeInput> input =
        ruvia::fromJson<WebsiteRuntimeInput>(json, options);
    return input ? std::optional<WebsiteRuntimeData>{normalize(*input)} : std::nullopt;
}

[[nodiscard]] inline WebsiteRuntimeOutput toOutput(const WebsiteRuntimeData& input,
                                                   ruvia::ModelOptions options = {}) {
    WebsiteRuntimeOutput output(options);
    auto& states = output.ensure<"domainStates">();
    states.reserve(input.domainStates.size());
    for (const auto& state : input.domainStates) {
        if (!state.id || !state.resolutionStatus) {
            throw std::invalid_argument("incomplete website domain runtime state");
        }
        auto& item = states.emplace_back(options);
        item.set<"id">(*state.id);
        item.set<"resolutionStatus">(*state.resolutionStatus);
        if (state.lastVerifiedAt) item.set<"lastVerifiedAt">(*state.lastVerifiedAt);
        if (state.lastError) item.set<"lastError">(*state.lastError);
    }
    return output;
}

} // namespace service::website_dns
