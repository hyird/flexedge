#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/core/worker_wait_result.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/features/live_resource/fanout.h"
#include "service/features/log_ingest/notifications.h"

namespace service::log_ingest::fanout {

inline constexpr auto sse_heartbeat_interval = std::chrono::seconds(15);
inline constexpr std::size_t subscriber_signal_capacity{1};

// Worker-local log tail wakeups: every Web Worker owns one (Ruvia worker
// state) and its log notification reader wakes only the tails it holds.
class hub final {
private:
    struct topic final {
        std::string tenant_id_;
        notifications::log_resource_type resource_type_;
        std::string resource_id_;

        friend bool operator==(const topic&, const topic&) = default;
    };

public:
    class subscription final {
    public:
        subscription(const subscription&) = delete;
        subscription& operator=(const subscription&) = delete;

        subscription(subscription&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              topic_(std::move(other.topic_)),
              id_(std::exchange(other.id_, 0)),
              receiver_(std::move(other.receiver_)) {}

        subscription& operator=(subscription&&) = delete;

        ~subscription() {
            close();
        }

        [[nodiscard]] ruvia::task<ruvia::worker_wait_result<std::uint64_t>>
        receive_for(std::chrono::steady_clock::duration timeout, ruvia::stop_token stop_token) const {
            if (!receiver_) {
                throw std::logic_error("log subscription receiver is not initialized");
            }
            return receiver_->receive_for(timeout, std::move(stop_token));
        }

    private:
        friend class hub;

        subscription(hub& owner, topic topic, std::uint64_t id,
            ruvia::channel_receiver<std::uint64_t> receiver)
            : owner_(&owner),
              topic_(std::move(topic)),
              id_(id),
              receiver_(std::move(receiver)) {}

        void close() noexcept {
            if (owner_ == nullptr) {
                return;
            }
            if (receiver_) {
                receiver_->close();
            }
            owner_->unsubscribe(topic_, id_);
            owner_ = nullptr;
            receiver_.reset();
        }

        hub* owner_{};
        topic topic_;
        std::uint64_t id_{};
        std::optional<ruvia::channel_receiver<std::uint64_t>> receiver_;
    };

    [[nodiscard]] subscription subscribe(const ruvia::worker_handle& worker,
        notifications::log_resource_type resource_type,
        std::string_view tenant_id, std::string_view resource_id) {
        return subscribe_topic(worker, {.tenant_id_ = std::string(tenant_id),
                                           .resource_type_ = resource_type,
                                           .resource_id_ = std::string(resource_id)});
    }

    void publish(const notifications::notification& notification) {
        const auto topic = topic_for(notification);
        if (!topic) {
            return;
        }
        const auto found = subscribers_.find(*topic);
        if (found == subscribers_.end()) {
            return;
        }
        const auto signal = next_signal_++;
        std::erase_if(found->second, [signal](const subscriber& subscriber) {
            const auto result = subscriber.sender_.send(signal);
            return result.status() == ruvia::channel_send_status::closed ||
                   result.status() == ruvia::channel_send_status::worker_stopping;
        });
        if (found->second.empty()) {
            subscribers_.erase(found);
        }
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
        const auto signal = next_signal_++;
        for (const auto& [topic, subscribers] : subscribers_) {
            if (tenant && topic.tenant_id_ != *tenant) {
                continue;
            }
            for (const auto& subscriber : subscribers) {
                (void)subscriber.sender_.send(signal);
            }
        }
    }

private:
    struct topic_hash final {
        [[nodiscard]] std::size_t operator()(const topic& value) const noexcept {
            const auto first = std::hash<std::string>{}(value.tenant_id_);
            const auto second = std::hash<int>{}(static_cast<int>(value.resource_type_));
            const auto third = std::hash<std::string>{}(value.resource_id_);
            return first ^ (second << 1U) ^ (third << 2U);
        }
    };

    struct subscriber final {
        std::uint64_t id_;
        ruvia::channel_sender<std::uint64_t> sender_;
    };

    [[nodiscard]] static std::optional<topic>
    topic_for(const notifications::notification& notification) {
        if (notification.tenant_id_.empty() || notification.resource_id_.empty()) {
            return std::nullopt;
        }
        return topic{.tenant_id_ = notification.tenant_id_,
            .resource_type_ = notification.resource_type_,
            .resource_id_ = notification.resource_id_};
    }

    [[nodiscard]] subscription subscribe_topic(const ruvia::worker_handle& worker, topic topic) {
        if (topic.tenant_id_.empty() || topic.resource_id_.empty()) {
            throw std::invalid_argument("log notification subscription requires a resource");
        }
        auto [sender, receiver] =
            ruvia::make_channel<std::uint64_t>(worker, {.capacity_ = subscriber_signal_capacity});
        const auto id = next_subscription_id_++;
        subscribers_[topic].push_back({.id_ = id, .sender_ = std::move(sender)});
        return subscription(*this, std::move(topic), id, std::move(receiver));
    }

    void unsubscribe(const topic& topic, std::uint64_t id) noexcept {
        const auto found = subscribers_.find(topic);
        if (found == subscribers_.end()) {
            return;
        }
        auto& values = found->second;
        std::erase_if(values, [id](const subscriber& subscriber) { return subscriber.id_ == id; });
        if (values.empty()) {
            subscribers_.erase(found);
        }
    }

    std::unordered_map<topic, std::vector<subscriber>, topic_hash> subscribers_;
    bool ready_{};
    std::uint64_t next_signal_{1};
    std::uint64_t next_subscription_id_{1};
};

// The calling worker's log tail hub (registered with use_worker_state).
template <typename context_type>
hub& get_hub(context_type& context) {
    return context.template worker_state<hub>();
}

// Delivers one log notification to the calling worker's own subscriptions.
template <typename context_type>
void deliver(context_type& context, const notifications::notification& notification) {
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

}  // namespace service::log_ingest::fanout
