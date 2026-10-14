#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/task.h>
#include <ruvia/web/web_worker.h>

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

namespace service::node_dispatch::fanout {

inline constexpr auto sse_heartbeat_interval = std::chrono::seconds(15);
inline constexpr std::size_t subscriber_signal_capacity{1};

class hub final {
public:
    class subscription final {
    public:
        subscription(const subscription&) = delete;
        subscription& operator=(const subscription&) = delete;

        subscription(subscription&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              tenant_id_(std::move(other.tenant_id_)),
              id_(std::exchange(other.id_, 0)),
              receiver_(std::move(other.receiver_)) {}

        subscription& operator=(subscription&&) = delete;

        ~subscription() {
            close();
        }

        [[nodiscard]] ruvia::task<ruvia::worker_wait_result<std::uint64_t>>
        receive_for(std::chrono::steady_clock::duration timeout, ruvia::stop_token stop_token) const {
            if (!receiver_) {
                throw std::logic_error("node dispatch subscription receiver is not initialized");
            }
            return receiver_->receive_for(timeout, std::move(stop_token));
        }

    private:
        friend class hub;

        subscription(hub& owner, std::string tenant_id, std::uint64_t id,
            ruvia::channel_receiver<std::uint64_t> receiver)
            : owner_(&owner),
              tenant_id_(std::move(tenant_id)),
              id_(id),
              receiver_(std::move(receiver)) {}

        void close() noexcept {
            if (owner_ == nullptr) {
                return;
            }
            if (receiver_) {
                receiver_->close();
            }
            owner_->unsubscribe(tenant_id_, id_);
            owner_ = nullptr;
            receiver_.reset();
        }

        hub* owner_{};
        std::string tenant_id_;
        std::uint64_t id_{};
        std::optional<ruvia::channel_receiver<std::uint64_t>> receiver_;
    };

    [[nodiscard]] subscription subscribe(const ruvia::worker_handle& worker,
        std::string_view tenant_id) {
        if (tenant_id.empty()) {
            throw std::invalid_argument("node dispatch subscription requires a tenant");
        }
        auto [sender, receiver] =
            ruvia::make_channel<std::uint64_t>(worker, {.capacity_ = subscriber_signal_capacity});
        const std::lock_guard lock(mutex_);
        const auto id = next_subscription_id_++;
        auto key = std::string(tenant_id);
        subscribers_[key].push_back({.id_ = id, .sender_ = std::move(sender)});
        return subscription(*this, std::move(key), id, std::move(receiver));
    }

    void publish(std::string_view tenant_id) {
        std::vector<subscriber> subscribers;
        {
            const std::lock_guard lock(mutex_);
            const auto found = subscribers_.find(std::string(tenant_id));
            if (found == subscribers_.end()) {
                return;
            }
            subscribers = found->second;
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
            remove_closed(tenant_id, closed);
        }
    }

private:
    struct subscriber final {
        std::uint64_t id_;
        ruvia::channel_sender<std::uint64_t> sender_;
    };

    void unsubscribe(std::string_view tenant_id, std::uint64_t id) noexcept {
        const std::lock_guard lock(mutex_);
        const auto found = subscribers_.find(std::string(tenant_id));
        if (found == subscribers_.end()) {
            return;
        }
        auto& values = found->second;
        std::erase_if(values, [id](const subscriber& subscriber) { return subscriber.id_ == id; });
        if (values.empty()) {
            subscribers_.erase(found);
        }
    }

    void remove_closed(std::string_view tenant_id, const std::vector<std::uint64_t>& closed) noexcept {
        const std::lock_guard lock(mutex_);
        const auto found = subscribers_.find(std::string(tenant_id));
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
    std::unordered_map<std::string, std::vector<subscriber>> subscribers_;
    std::atomic<std::uint64_t> next_signal_{1};
    std::uint64_t next_subscription_id_{1};
};

inline hub& get_hub() {
    static hub value;
    return value;
}

}  // namespace service::node_dispatch::fanout
