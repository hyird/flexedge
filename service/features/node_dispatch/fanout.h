#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "service/features/background/keyed_signal_hub.h"
#include "service/features/live_resource/notifications.h"

namespace service::node_dispatch::fanout {

// Worker-local wakeups of node control connections waiting for a desired
// state change, keyed by tenant. Every Web Worker owns one (Ruvia worker
// state); its notification reader wakes only the connections this worker
// holds. A separate hub from live_resource: node heartbeats must not wake
// configuration watches.
struct hub final : service::background::keyed_signal_hub<std::string, std::monostate, service::background::string_key_hash> {};

// The calling worker's node dispatch hub (registered with use_worker_state).
template <typename context_type>
hub& get_hub(context_type& context) {
    return context.template worker_state<hub>();
}

// Wakes this worker's node control connections when committed resource
// notifications change their tenant's desired state. The notification is
// published only after the outbox refreshed the node channel projection.
struct desired_state_waits final {
    using stream = service::live_resource::notifications::stream;

    // `recovered_at`: when this reader last recovered from losing the stream;
    // an owed recovery of a producer from before that instant is covered.
    template <typename context_type>
    static void deliver(context_type& context, const service::live_resource::notifications::resource_notification& value,
        std::optional<std::int64_t> recovered_at) {
        namespace notifications = service::live_resource::notifications;
        auto& waits = get_hub(context);
        if (value.missed_until_ && (!recovered_at || *recovered_at < *value.missed_until_)) {
            if (value.tenant_id_ == notifications::all_tenants) {
                waits.publish_all();
            } else {
                waits.publish(value.tenant_id_);
            }
        }
        if (value.node_config_ && value.tenant_id_ != notifications::all_tenants) {
            waits.publish(value.tenant_id_);
        }
    }
    // Notifications may have been missed: every connection re-reads once.
    template <typename context_type>
    static ruvia::task<void> recover(context_type& context, ruvia::redis_handle) {
        get_hub(context).publish_all();
        co_return;
    }
};

}  // namespace service::node_dispatch::fanout
