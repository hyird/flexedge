#pragma once

#include <ruvia/web/context.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/config/auth.h"
#include "service/features/auth_session/session_credential.h"

namespace service::auth {

inline constexpr std::string_view session_cookie_name{"flexedge_session"};

inline ruvia::cookie_options session_cookie_options() {
    const auto& config = service::config::auth_config();
    return {
        .path_ = "/api",
        .same_site_ = ruvia::cookie_same_site::strict,
        .max_age_ = config.session_expires_in_,
        .http_only_ = ruvia::cookie_attribute_policy::emit,
        .secure_ = config.cookie_secure_ ? ruvia::cookie_attribute_policy::emit
                                         : ruvia::cookie_attribute_policy::omit,
    };
}

inline void set_session_cookie(ruvia::context& c, const session_credential& credential) {
    const auto value = encode_session_credential(credential);
    c.set_cookie(
        {.name_ = session_cookie_name, .value_ = value.view(), .attributes_ = session_cookie_options()});
}

inline void delete_session_cookie(ruvia::context& c) {
    c.delete_cookie({.name_ = session_cookie_name, .attributes_ = session_cookie_options()});
}

inline std::optional<session_credential> read_session_cookie(ruvia::context& c) {
    const auto cookie = c.req().cookie(session_cookie_name);
    if (!cookie) {
        return std::nullopt;
    }
    return parse_session_credential(*cookie);
}

}  // namespace service::auth
