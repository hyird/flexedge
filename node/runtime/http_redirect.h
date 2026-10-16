#pragma once

#include <ruvia/http/http_status.h>

#include <optional>
#include <string>
#include <string_view>

#include "common/route_policy.h"
#include "node/runtime/compiled_config.h"

namespace flexedge::node {

struct http_redirect final {
    ruvia::http_status_code status_{ruvia::http_status::moved_permanently};
    std::string location_;
};

inline std::optional<http_redirect>
http_to_https_redirect(const compiled_config& config, std::string_view host, std::string_view target) {
    const auto* website = config.website(host);
    const auto* domain = config.domain(host);
    if (website == nullptr || domain == nullptr || !website->force_https() ||
        !website->https_enabled() || !domain->https_enabled()) {
        return std::nullopt;
    }
    if (target.empty() || target.front() != '/') {
        target = "/";
    }
    http_redirect result;
    result.location_ = "https://" + flexedge::route_policy::hostname(host) + std::string(target);
    return result;
}

}  // namespace flexedge::node
