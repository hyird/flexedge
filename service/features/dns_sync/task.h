#pragma once

#include <cstdint>
#include <string>

#include "service/features/sync_runtime/state.h"

namespace service::dns_sync::detail {

struct dns_task final {
    std::string id_;
    std::string tenant_id_;
    std::string resource_id_;
    service::sync_runtime::marker_operation operation_;
    std::int64_t version_;
    std::int64_t failures_;
};

}  // namespace service::dns_sync::detail
