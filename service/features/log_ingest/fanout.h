#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/web/redis/redis.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "service/features/background/keyed_signal_hub.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/log_ingest/notifications.h"

namespace service::log_ingest::fanout {

// Worker-local log tail wakeups: every Web Worker owns one (Ruvia worker
// state) and its log notification reader wakes only the tails it holds.
class hub final {
    struct topic final {
        std::string tenant_id_;
        notifications::log_resource_type resource_type_;
        std::string resource_id_;

        friend bool operator==(const topic&, const topic&) = default;
    };
    struct topic_hash final {
        [[nodiscard]] std::size_t operator()(const topic& value) const noexcept {
            const auto first = std::hash<std::string>{}(value.tenant_id_);
            const auto second = std::hash<int>{}(static_cast<int>(value.resource_type_));
            const auto third = std::hash<std::string>{}(value.resource_id_);
            return first ^ (second << 1U) ^ (third << 2U);
        }
    };
    using tails = service::background::keyed_signal_hub<topic, std::monostate, topic_hash>;

public:
    using subscription = tails::subscription;

    [[nodiscard]] subscription subscribe(const ruvia::worker_handle& worker,
        notifications::log_resource_type resource_type,
        std::string_view tenant_id, std::string_view resource_id) {
        if (tenant_id.empty() || resource_id.empty()) {
            throw std::invalid_argument("log notification subscription requires a resource");
        }
        return tails_.subscribe(worker, topic{.tenant_id_ = std::string(tenant_id),
                                            .resource_type_ = resource_type,
                                            .resource_id_ = std::string(resource_id)});
    }

    void publish(const notifications::notification& notification) {
        if (notification.tenant_id_.empty() || notification.resource_id_.empty()) {
            return;
        }
        tails_.publish(topic{.tenant_id_ = notification.tenant_id_,
            .resource_type_ = notification.resource_type_,
            .resource_id_ = notification.resource_id_});
    }

    void mark_ready() {
        if (std::exchange(ready_, true)) {
            return;
        }
        recover();
    }

    // A restored Redis reader wakes each of this worker's own tails once.
    // Tails resume from their persisted database cursor, so stream trimming
    // during the outage does not leave a connected tail permanently stale.
    void recover(std::optional<std::string_view> tenant = std::nullopt) {
        tails_.publish_if([tenant](const topic& key, std::monostate&) { return !tenant || key.tenant_id_ == *tenant; });
    }

private:
    tails tails_;
    bool ready_{};
};

// The calling worker's log tail hub (registered with use_worker_state).
template <typename context_type>
hub& get_hub(context_type& context) {
    return context.template worker_state<hub>();
}

// Delivers log notifications to the calling worker's own subscriptions.
struct log_tails final {
    using stream = notifications::stream;

    template <typename context_type>
    static void deliver(context_type& context, const notifications::notification& notification,
        std::optional<std::int64_t>) {
        if (notification.resource_type_ == notifications::log_resource_type::access) {
            auto& resources = service::live_resource::get_hub(context);
            resources.publish(notification.tenant_id_, service::live_resource::resource::access_history,
                notification.resource_id_);
            // The worker's gate recomputes the tenant traffic aggregate at most
            // once per interval, however many log batches arrive.
            resources.publish_coalesced(notification.tenant_id_, service::live_resource::resource::overview_traffic,
                service::live_resource::traffic_refresh_interval);
        }
        get_hub(context).publish(notification);
    }
    template <typename context_type>
    static void ready(context_type& context) {
        get_hub(context).mark_ready();
    }
    template <typename context_type>
    static ruvia::task<void> recover(context_type& context, ruvia::redis_handle) {
        get_hub(context).recover();
        co_return;
    }
};

// A producer that could not send its resource notifications owes the
// tenant's tails one re-read as well.
struct log_tail_recovery final {
    using stream = service::live_resource::notifications::stream;

    // `recovered_at`: when this reader last recovered from losing the stream;
    // an owed recovery of a producer from before that instant is covered.
    template <typename context_type>
    static void deliver(context_type& context, const service::live_resource::notifications::resource_notification& value,
        std::optional<std::int64_t> recovered_at) {
        if (!value.missed_until_ || (recovered_at && *recovered_at >= *value.missed_until_)) {
            return;
        }
        if (value.tenant_id_ == service::live_resource::notifications::all_tenants) {
            get_hub(context).recover();
        } else {
            get_hub(context).recover(value.tenant_id_);
        }
    }
};

}  // namespace service::log_ingest::fanout
