#pragma once

#include <ruvia/web/context.h>

#include <string>

#include "service/domains/auth/auth_types.h"

namespace service::auth {

inline auth_user_info_dto auth_user_info(ruvia::context& c, const std::string& admin_id,
    const std::string& username, const std::string& nickname,
    const std::string& status) {
    auth_user_info_dto info({.resource_ = c.pool()});
    info.set<"id">(admin_id);
    info.set<"username">(username);
    info.set<"nickname">(nickname);
    info.set<"status">(status);
    return info;
}

}  // namespace service::auth
