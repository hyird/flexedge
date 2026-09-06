#pragma once

#include <cstdint>
#include <string>

namespace service::provider_verification::detail {

struct VerificationTask final {
    std::string id;
    std::string tenantId;
    std::string providerId;
    std::string kind;
    std::string provider;
    std::string name;
    std::string accountId;
    std::int64_t providerRevision;
    std::int64_t generation;
    std::int64_t failures;
    std::string configJson;
    std::string runtimeJson;
};

} // namespace service::provider_verification::detail
