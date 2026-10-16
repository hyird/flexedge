#pragma once

#include <string>

namespace service::auth {

struct authenticated_principal final {
    std::string admin_id_;
    std::string system_tenant_id_;
};

}  // namespace service::auth
