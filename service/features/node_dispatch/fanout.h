#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace service::node_dispatch::fanout {

inline constexpr std::size_t subscriber_signal_capacity{1};

// Worker-local wakeups of node control connections waiting for a desired
// state change. Every Web Worker owns one (Ruvia worker state); its resource
// notification reader wakes only the connections this worker holds. A
// separate hub from live_resource: node heartbeats must not wake
// configuration watches.
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
        const auto id = next_subscription_id_++;
        auto key = std::string(tenant_id);
        subscribers_[key].push_back({.id_ = id, .sender_ = std::move(sender)});
        return subscription(*this, std::move(key), id, std::move(receiver));
    }

    void publish(std::string_view tenant_id) {
        const auto found = subscribers_.find(std::string(tenant_id));
        if (found == subscribers_.end()) {
            return;
        }
        wake(found->second);
    }

    // Notifications may have been missed: every connection re-reads once.
    void publish_all() {
        for (auto& [tenant_id, subscribers] : subscribers_) {
            (void)tenant_id;
            wake(subscribers);
        }
    }

private:
    struct subscriber final {
        std::uint64_t id_;
        ruvia::channel_sender<std::uint64_t> sender_;
    };

    void wake(std::vector<subscriber>& subscribers) {
        const auto signal = next_signal_++;
        std::erase_if(subscribers, [signal](const subscriber& subscriber) {
            const auto result = subscriber.sender_.send(signal);
            return result.status() == ruvia::channel_send_status::closed ||
                   result.status() == ruvia::channel_send_status::worker_stopping;
        });
    }

    void unsubscribe(std::string_view tenant_id, std::uint64_t id) noexcept {
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

    std::unordered_map<std::string, std::vector<subscriber>> subscribers_;
    std::uint64_t next_signal_{1};
    std::uint64_t next_subscription_id_{1};
};

// The calling worker's node dispatch hub (registered with use_worker_state).
template <typename context_type>
hub& get_hub(context_type& context) {
    return context.template worker_state<hub>();
}

}  // namespace service::node_dispatch::fanout
