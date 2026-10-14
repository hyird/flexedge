#pragma once

#include "service/features/node_dispatch/fanout.h"

namespace service::node_dispatch::notifications {
// Separate hub: node heartbeats must not wake configuration watches.
inline service::node_dispatch::fanout::hub& hub() {
    static service::node_dispatch::fanout::hub instance;
    return instance;
}
inline void published(std::string_view tenant_id) {
    hub().publish(tenant_id);
}
}  // namespace service::node_dispatch::notifications
