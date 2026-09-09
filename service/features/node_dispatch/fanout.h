#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
#include <ruvia/web/WebWorker.h>

namespace service::node_dispatch::fanout {

inline constexpr auto kSseHeartbeatInterval = std::chrono::seconds(15);
inline constexpr std::size_t kSubscriberSignalCapacity{1};

class Hub final {
  public:
    class Subscription final {
      public:
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), tenantId_(std::move(other.tenantId_)),
              id_(std::exchange(other.id_, 0)), receiver_(std::move(other.receiver_)) {}

        Subscription& operator=(Subscription&&) = delete;

        ~Subscription() { close(); }

        [[nodiscard]] ruvia::Task<ruvia::WorkerWaitResult<std::uint64_t>>
        receiveFor(std::chrono::steady_clock::duration timeout, ruvia::StopToken stopToken) const {
            if (!receiver_) {
                throw std::logic_error("node dispatch subscription receiver is not initialized");
            }
            return receiver_->receiveFor(timeout, std::move(stopToken));
        }

      private:
        friend class Hub;

        Subscription(Hub& owner, std::string tenantId, std::uint64_t id,
                     ruvia::ChannelReceiver<std::uint64_t> receiver)
            : owner_(&owner), tenantId_(std::move(tenantId)), id_(id), receiver_(std::move(receiver)) {}

        void close() noexcept {
            if (owner_ == nullptr) {
                return;
            }
            if (receiver_) {
                receiver_->close();
            }
            owner_->unsubscribe(tenantId_, id_);
            owner_ = nullptr;
            receiver_.reset();
        }

        Hub* owner_{};
        std::string tenantId_;
        std::uint64_t id_{};
        std::optional<ruvia::ChannelReceiver<std::uint64_t>> receiver_;
    };

    [[nodiscard]] Subscription subscribe(const ruvia::WorkerHandle& worker,
                                         std::string_view tenantId) {
        if (tenantId.empty()) {
            throw std::invalid_argument("node dispatch subscription requires a tenant");
        }
        auto [sender, receiver] =
            ruvia::makeChannel<std::uint64_t>(worker, {.capacity = kSubscriberSignalCapacity});
        const std::lock_guard lock(mutex_);
        const auto id = nextSubscriptionId_++;
        auto key = std::string(tenantId);
        subscribers_[key].push_back({.id = id, .sender = std::move(sender)});
        return Subscription(*this, std::move(key), id, std::move(receiver));
    }

    void publish(std::string_view tenantId) {
        std::vector<Subscriber> subscribers;
        {
            const std::lock_guard lock(mutex_);
            const auto found = subscribers_.find(std::string(tenantId));
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
            removeClosed(tenantId, closed);
        }
    }

  private:
    struct Subscriber final {
        std::uint64_t id;
        ruvia::ChannelSender<std::uint64_t> sender;
    };

    void unsubscribe(std::string_view tenantId, std::uint64_t id) noexcept {
        const std::lock_guard lock(mutex_);
        const auto found = subscribers_.find(std::string(tenantId));
        if (found == subscribers_.end()) {
            return;
        }
        auto& values = found->second;
        std::erase_if(values, [id](const Subscriber& subscriber) { return subscriber.id == id; });
        if (values.empty()) {
            subscribers_.erase(found);
        }
    }

    void removeClosed(std::string_view tenantId, const std::vector<std::uint64_t>& closed) noexcept {
        const std::lock_guard lock(mutex_);
        const auto found = subscribers_.find(std::string(tenantId));
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
    std::unordered_map<std::string, std::vector<Subscriber>> subscribers_;
    std::atomic<std::uint64_t> nextSignal_{1};
    std::uint64_t nextSubscriptionId_{1};
};

inline Hub& hub() {
    static Hub value;
    return value;
}

} // namespace service::node_dispatch::fanout
