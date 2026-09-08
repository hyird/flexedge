#pragma once

#include "service/features/node_runtime/fanout.h"

namespace service::node_dispatch::notifications {
// Separate hub: node heartbeats must not wake configuration watches.
inline service::node_runtime::fanout::Hub& hub() {
    static service::node_runtime::fanout::Hub instance;
    return instance;
}
inline void published(std::string_view tenantId) {
    hub().publish(tenantId);
    // Visible pending status changes at commit, before agents acknowledge.
    service::node_runtime::fanout::hub().publish(tenantId);
}
} // namespace service::node_dispatch::notifications
