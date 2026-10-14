#pragma once

#include <string>

namespace service::auth {

struct auth_user_record final {
    std::string id_;
    std::string username_;
    std::string nickname_;
    std::string status_;
};

}  // namespace service::auth
