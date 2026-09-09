#pragma once

#include <string>

namespace service::auth {

struct AuthUserRecord final {
    std::string id;
    std::string username;
    std::string nickname;
    std::string status;
};

} // namespace service::auth
