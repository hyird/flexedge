#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/core/worker_wait_result.h>
#include <ruvia/web/web_worker.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/features/live_resource/fanout.h"
#include "service/features/log_ingest/notifications.h"
#include "service/features/logging/logger.h"

namespace service::log_ingest::fanout {

inline constexpr auto sse_heartbeat_interval = std::chrono::seconds(15);
inline constexpr auto reader_retry_delay = std::chrono::seconds(1);
inline constexpr std::size_t subscriber_signal_capacity{1};

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

    void publish(const notifications::notification& notification, ruvia::worker_id_type worker) {
        if (notification.resource_type_ == notifications::log_resource_type::access) {
            service::live_resource::get_hub().publish_for_worker(worker, notification.tenant_id_,
                service::live_resource::resource::access_history, notification.resource_id_);
            // Every web worker reads every notification; the process-wide
            // gate delivers the tenant traffic aggregate at most once per
            // interval regardless of which worker observed it first.
            service::live_resource::get_hub().publish_coalesced(
                notification.tenant_id_, service::live_resource::resource::overview_traffic,
                service::live_resource::traffic_refresh_interval);
        }
        const auto topic = topic_for(notification);
        if (!topic) {
            return;
        }

        std::vector<subscriber> subscribers;
        {
            const std::lock_guard lock(mutex_);
            const auto found = subscribers_.find(*topic);
            if (found == subscribers_.end()) {
                return;
            }
            for (const auto& subscriber : found->second) {
                if (subscriber.worker_ == worker) {
                    subscribers.push_back(subscriber);
                }
            }
        }

        const auto signal = next_signal_.fetch_add(1, std::memory_order_relaxed);
        std::vector<std::uint64_t> closed;
        for (const auto& subscriber : subscribers) {
            const auto result = subscriber.sender_.send(signal);
            if (result.status() == ruvia::channel_send_status::closed ||
                result.status() == ruvia::channel_send_status::worker_stopping) {
                closed.push_back(subscriber.id_);
            }
        }
        if (!closed.empty()) {
            remove_closed(*topic, closed);
        }
    }

    void mark_ready() {
        if (ready_.exchange(true, std::memory_order_release)) {
            return;
        }
        std::vector<ruvia::channel_sender<std::uint64_t>> senders;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& [_, subscribers] : subscribers_) {
                for (const auto& subscriber : subscribers) {
                    senders.push_back(subscriber.sender_);
                }
            }
        }
        const auto signal = next_signal_.fetch_add(1, std::memory_order_relaxed);
        for (const auto& sender : senders) {
            (void)sender.send(signal);
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
        ruvia::worker_id_type worker_;
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
        const std::lock_guard lock(mutex_);
        const auto id = next_subscription_id_++;
        subscribers_[topic].push_back({.id_ = id, .worker_ = worker.id(), .sender_ = std::move(sender)});
        return subscription(*this, std::move(topic), id, std::move(receiver));
    }

    void unsubscribe(const topic& topic, std::uint64_t id) noexcept {
        const std::lock_guard lock(mutex_);
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

    void remove_closed(const topic& topic, const std::vector<std::uint64_t>& closed) noexcept {
        const std::lock_guard lock(mutex_);
        const auto found = subscribers_.find(topic);
        if (found == subscribers_.end()) {
            return;
        }
        auto& values = found->second;
        std::erase_if(values, [&closed](const subscriber& subscriber) {
            return std::ranges::find(closed, subscriber.id_) != closed.end();
        });
        if (values.empty()) {
            subscribers_.erase(found);
        }
    }

    std::mutex mutex_;
    std::unordered_map<topic, std::vector<subscriber>, topic_hash> subscribers_;
    std::atomic_bool ready_{};
    std::atomic<std::uint64_t> next_signal_{1};
    std::uint64_t next_subscription_id_{1};
};

inline hub& get_hub() {
    static hub value;
    return value;
}

template <typename context_t_type>
inline ruvia::task<void> run(context_t_type& context) {
    std::string cursor;
    bool initialized{};
    while (!context.get_stop_token().stop_requested()) {
        bool retry{};
        try {
            if (!initialized) {
                cursor = co_await notifications::current_cursor(context.redis());
                get_hub().mark_ready();
                initialized = true;
            }
            auto batch = co_await notifications::read(context.redis(), cursor);
            if (batch.cursor_) {
                cursor = *batch.cursor_;
            }
            for (const auto& notification : batch.notifications_) {
                get_hub().publish(notification, context.worker().id());
            }
        } catch (const std::exception& error) {
            if (!context.get_stop_token().stop_requested()) {
                service::logging::error("Log notification fanout failure: " +
                                        std::string(error.what()));
                retry = true;
            }
        } catch (...) {
            if (!context.get_stop_token().stop_requested()) {
                service::logging::error("Log notification fanout failed with unknown error");
                retry = true;
            }
        }
        if (retry &&
            co_await ruvia::sleep_for(context.worker(), reader_retry_delay,
                context.get_stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
    }
}

}  // namespace service::log_ingest::fanout
