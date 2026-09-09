#pragma once

#include "service/features/node_dispatch/fanout.h"

namespace service::node_dispatch::notifications {
// Separate hub: node heartbeats must not wake configuration watches.
inline service::node_dispatch::fanout::Hub& hub() {
    static service::node_dispatch::fanout::Hub instance;
    return instance;
}
inline void published(std::string_view tenantId) {
    hub().publish(tenantId);
}
} // namespace service::node_dispatch::notifications
