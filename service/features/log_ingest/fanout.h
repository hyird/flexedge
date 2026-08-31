#pragma once

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

#include <ruvia/core/Channel.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/features/log_ingest/notifications.h"
#include "service/features/logging/logger.h"

namespace service::log_ingest::fanout {

inline constexpr auto kSseHeartbeatInterval = std::chrono::seconds(15);
inline constexpr auto kReaderRetryDelay = std::chrono::seconds(1);
inline constexpr std::size_t kSubscriberSignalCapacity{1};

class Hub final {
  private:
    struct Topic final {
        std::string tenantId;
        std::string kind;
        std::string resourceId;

        friend bool operator==(const Topic&, const Topic&) = default;
    };

  public:
    class Subscription final {
      public:
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), topic_(std::move(other.topic_)),
              id_(std::exchange(other.id_, 0)), receiver_(std::move(other.receiver_)) {}

        Subscription& operator=(Subscription&&) = delete;

        ~Subscription() { close(); }

        [[nodiscard]] ruvia::Task<ruvia::WorkerWaitResult<std::uint64_t>>
        receiveFor(std::chrono::steady_clock::duration timeout, ruvia::StopToken stopToken) const {
            if (!receiver_) {
                throw std::logic_error("log subscription receiver is not initialized");
            }
            return receiver_->receiveFor(timeout, std::move(stopToken));
        }

      private:
        friend class Hub;

        Subscription(Hub& owner, Topic topic, std::uint64_t id,
                     ruvia::ChannelReceiver<std::uint64_t> receiver)
            : owner_(&owner), topic_(std::move(topic)), id_(id), receiver_(std::move(receiver)) {}

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

        Hub* owner_{};
        Topic topic_;
        std::uint64_t id_{};
        std::optional<ruvia::ChannelReceiver<std::uint64_t>> receiver_;
    };

    [[nodiscard]] Subscription subscribeAccess(const ruvia::WorkerHandle& worker,
                                               std::string_view tenantId,
                                               std::string_view websiteId) {
        return subscribe(worker, {.tenantId = std::string(tenantId),
                                  .kind = std::string(notifications::kKindAccess),
                                  .resourceId = std::string(websiteId)});
    }

    [[nodiscard]] Subscription subscribeNode(const ruvia::WorkerHandle& worker,
                                             std::string_view tenantId, std::string_view nodeId) {
        return subscribe(worker, {.tenantId = std::string(tenantId),
                                  .kind = std::string(notifications::kKindNode),
                                  .resourceId = std::string(nodeId)});
    }

    void publish(const notifications::Notification& notification) {
        const auto topic = topicFor(notification);
        if (!topic) {
            return;
        }

        std::vector<Subscriber> subscribers;
        {
            const std::lock_guard lock(mutex_);
            const auto found = subscribers_.find(*topic);
            if (found == subscribers_.end()) {
                return;
            }
            subscribers = found->second;
        }

        const auto signal = nextSignal_.fetch_add(1, std::memory_order_relaxed);
        std::vector<std::uint64_t> closed;
        for (const auto& subscriber : subscribers) {
            const auto result = subscriber.sender.send(signal);
            if (result.status() == ruvia::ChannelSendStatus::kClosed ||
                result.status() == ruvia::ChannelSendStatus::kWorkerStopping) {
                closed.push_back(subscriber.id);
            }
        }
        if (!closed.empty()) {
            removeClosed(*topic, closed);
        }
    }

    void markReady() {
        if (ready_.exchange(true, std::memory_order_release)) {
            return;
        }
        std::vector<ruvia::ChannelSender<std::uint64_t>> senders;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& [_, subscribers] : subscribers_) {
                for (const auto& subscriber : subscribers) {
                    senders.push_back(subscriber.sender);
                }
            }
        }
        const auto signal = nextSignal_.fetch_add(1, std::memory_order_relaxed);
        for (const auto& sender : senders) {
            (void)sender.send(signal);
        }
    }

  private:
    struct TopicHash final {
        [[nodiscard]] std::size_t operator()(const Topic& value) const noexcept {
            const auto first = std::hash<std::string>{}(value.tenantId);
            const auto second = std::hash<std::string>{}(value.kind);
            const auto third = std::hash<std::string>{}(value.resourceId);
            return first ^ (second << 1U) ^ (third << 2U);
        }
    };

    struct Subscriber final {
        std::uint64_t id;
        ruvia::ChannelSender<std::uint64_t> sender;
    };

    [[nodiscard]] static std::optional<Topic>
    topicFor(const notifications::Notification& notification) {
        if (notification.tenantId.empty()) {
            return std::nullopt;
        }
        if (notification.kind == notifications::kKindAccess && !notification.websiteId.empty()) {
            return Topic{.tenantId = notification.tenantId,
                         .kind = std::string(notifications::kKindAccess),
                         .resourceId = notification.websiteId};
        }
        if (notification.kind == notifications::kKindNode && !notification.nodeId.empty()) {
            return Topic{.tenantId = notification.tenantId,
                         .kind = std::string(notifications::kKindNode),
                         .resourceId = notification.nodeId};
        }
        return std::nullopt;
    }

    [[nodiscard]] Subscription subscribe(const ruvia::WorkerHandle& worker, Topic topic) {
        if (topic.tenantId.empty() || topic.resourceId.empty()) {
            throw std::invalid_argument("log notification subscription requires a resource");
        }
        auto [sender, receiver] =
            ruvia::makeChannel<std::uint64_t>(worker, {.capacity = kSubscriberSignalCapacity});
        const std::lock_guard lock(mutex_);
        const auto id = nextSubscriptionId_++;
        subscribers_[topic].push_back({.id = id, .sender = std::move(sender)});
        return Subscription(*this, std::move(topic), id, std::move(receiver));
    }

    void unsubscribe(const Topic& topic, std::uint64_t id) noexcept {
        const std::lock_guard lock(mutex_);
        const auto found = subscribers_.find(topic);
        if (found == subscribers_.end()) {
            return;
        }
        auto& values = found->second;
        std::erase_if(values, [id](const Subscriber& subscriber) { return subscriber.id == id; });
        if (values.empty()) {
            subscribers_.erase(found);
        }
    }

    void removeClosed(const Topic& topic, const std::vector<std::uint64_t>& closed) noexcept {
        const std::lock_guard lock(mutex_);
        const auto found = subscribers_.find(topic);
        if (found == subscribers_.end()) {
            return;
        }
        auto& values = found->second;
        std::erase_if(values, [&closed](const Subscriber& subscriber) {
            return std::ranges::find(closed, subscriber.id) != closed.end();
        });
        if (values.empty()) {
            subscribers_.erase(found);
        }
    }

    std::mutex mutex_;
    std::unordered_map<Topic, std::vector<Subscriber>, TopicHash> subscribers_;
    std::atomic_bool ready_{};
    std::atomic<std::uint64_t> nextSignal_{1};
    std::uint64_t nextSubscriptionId_{1};
};

inline Hub& hub() {
    static Hub value;
    return value;
}

inline ruvia::Task<void> run(ruvia::WebWorkerContext& context) {
    std::string cursor;
    bool initialized{};
    while (!context.stopToken().stopRequested()) {
        bool retry{};
        try {
            if (!initialized) {
                cursor = co_await notifications::currentCursor(context.redis());
                hub().markReady();
                initialized = true;
            }
            auto notifications = co_await notifications::read(context.redis(), cursor);
            for (const auto& notification : notifications) {
                cursor = notification.id;
                hub().publish(notification);
            }
        } catch (const std::exception& error) {
            if (!context.stopToken().stopRequested()) {
                service::logging::error("Log notification fanout failure: " +
                                        std::string(error.what()));
                retry = true;
            }
        } catch (...) {
            if (!context.stopToken().stopRequested()) {
                service::logging::error("Log notification fanout failed with unknown error");
                retry = true;
            }
        }
        if (retry &&
            co_await ruvia::sleepFor(context.worker(), kReaderRetryDelay, context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
            co_return;
        }
    }
}

} // namespace service::log_ingest::fanout
