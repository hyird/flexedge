#pragma once

#include <cstdint>
#include <string>

#include "service/features/sync_runtime/state.h"

namespace service::dns_sync::detail {

struct DnsTask final {
    std::string id;
    std::string tenantId;
    std::string resourceId;
    service::sync_runtime::MarkerOperation operation;
    std::int64_t version;
    std::int64_t failures;
};

} // namespace service::dns_sync::detail
