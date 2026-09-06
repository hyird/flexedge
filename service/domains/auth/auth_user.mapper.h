#pragma once

#include <string>

#include <ruvia/web/Context.h>

#include "service/domains/auth/auth.types.h"

namespace service::auth {

inline AuthUserInfoDto authUserInfo(ruvia::Context& c, const std::string& adminId,
                                    const std::string& username, const std::string& nickname,
                                    const std::string& status) {
    AuthUserInfoDto info(c);
    info.set<"id">(adminId);
    info.set<"username">(username);
    info.set<"nickname">(nickname);
    info.set<"status">(status);
    return info;
}

} // namespace service::auth
