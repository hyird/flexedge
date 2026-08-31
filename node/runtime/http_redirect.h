#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <ruvia/http/HttpStatus.h>

#include "node/runtime/compiled_config.h"

namespace flexedge::node {

struct HttpRedirect final {
    ruvia::HttpStatusCode status{ruvia::http_status::kMovedPermanently};
    std::string location;
};

inline std::optional<HttpRedirect>
httpToHttpsRedirect(const CompiledConfig& config, std::string_view host, std::string_view target) {
    const auto* website = config.website(host);
    const auto* domain = config.domain(host);
    if (website == nullptr || domain == nullptr || !website->force_https() ||
        !website->https_enabled() || !domain->https_enabled()) {
        return std::nullopt;
    }
    if (target.empty() || target.front() != '/') {
        target = "/";
    }
    HttpRedirect result;
    result.location = "https://" + normalizeHostname(host) + std::string(target);
    return result;
}

} // namespace flexedge::node
