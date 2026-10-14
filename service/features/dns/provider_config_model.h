#pragma once

#include <string>

namespace service::dns {

struct dns_provider_config_data final {
    std::string credential_envelope_;
    std::string credential_hint_;
};

}  // namespace service::dns
