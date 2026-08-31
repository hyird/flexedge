#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::website_dns {

RUVIA_REQUEST_MODEL(WebsiteDomainRuntimeInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("resolution_status", resolutionStatus, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", lastVerifiedAt, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));
RUVIA_REQUEST_MODEL(WebsiteRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("domain_states", domainStates,
                                              ruvia::Array<WebsiteDomainRuntimeInput>));

RUVIA_RESPONSE_MODEL(
    WebsiteDomainRuntimeOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("resolution_status", resolutionStatus, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", lastVerifiedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));
RUVIA_RESPONSE_MODEL(WebsiteRuntimeOutput,
                     RUVIA_REQUIRED_FIELD_NAME("domain_states", domainStates,
                                               ruvia::Array<WebsiteDomainRuntimeOutput>));

struct WebsiteDomainRuntimeData final {
    std::optional<std::string> id;
    std::optional<std::string> resolutionStatus;
    std::optional<std::string> lastVerifiedAt;
    std::optional<std::string> lastError;
};

struct WebsiteRuntimeData final {
    std::vector<WebsiteDomainRuntimeData> domainStates;
};

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

} // namespace service::website_dns
