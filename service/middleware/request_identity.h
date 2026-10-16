#pragma once

#include <ruvia/web/context.h>

#include <string>

#include "service/features/auth_session/principal.h"

namespace service::middleware {

inline const service::auth::authenticated_principal& current_principal(ruvia::context& c) {
    return c.request_state<service::auth::authenticated_principal>();
}

inline const std::string& current_tenant_id(ruvia::context& c) {
    return current_principal(c).system_tenant_id_;
}

}  // namespace service::middleware
