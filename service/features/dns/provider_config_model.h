#pragma once

#include <string>

namespace service::dns {

struct DnsProviderConfigData final {
    std::string credentialEnvelope;
    std::string credentialHint;
};


} // namespace service::dns
