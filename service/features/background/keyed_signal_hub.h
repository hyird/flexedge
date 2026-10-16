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
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace service::background {

// Hashes std::string keys and std::string_view lookups alike, so publishing
// with a borrowed key allocates nothing.
struct string_key_hash final {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

// Worker-local wakeups keyed by what a waiter observes. Every Web Worker owns
// its hubs (Ruvia worker state) and only that Worker's readers and committers
// publish to them; no hub is shared between Workers. A signal carries no data:
// the waiter re-reads its authoritative state, so a capacity-one channel
// coalesces signals without losing a change. One subscription may register
// under several keys, each with its own `value_type`, and still owns a single
// wakeup channel. With a transparent `hash_type`, lookups accept any type the
// key compares equal to.
template <typename key_type, typename value_type = std::monostate, typename hash_type = std::hash<key_type>>
class keyed_signal_hub {
    struct registration final {
        std::uint64_t subscription_;
        value_type value_;
        ruvia::channel_sender<std::uint64_t> sender_;
    };
    using equal_type = std::conditional_t<requires { typename hash_type::is_transparent; }, std::equal_to<>, std::equal_to<key_type>>;
    struct state final {
        std::unordered_map<key_type, std::vector<registration>, hash_type, equal_type> registrations_;
        std::size_t subscriptions_{};
        std::uint64_t next_subscription_{1};
        std::uint64_t next_signal_{1};

        void unsubscribe(const key_type& key, std::uint64_t id) noexcept {
            const auto found = registrations_.find(key);
            if (found == registrations_.end()) {
                return;
            }
            std::erase_if(found->second, [id](const registration& value) { return value.subscription_ == id; });
            if (found->second.empty()) {
                registrations_.erase(found);
            }
        }
    };

public:
    class subscription final {
    public:
        subscription(const subscription&) = delete;
        subscription& operator=(const subscription&) = delete;
        subscription(subscription&& other) noexcept
            : state_(std::move(other.state_)),
              id_(std::exchange(other.id_, 0)),
              keys_(std::move(other.keys_)),
              receiver_(std::move(other.receiver_)) {}
        subscription& operator=(subscription&&) = delete;
        ~subscription() {
            close();
        }

        [[nodiscard]] ruvia::task<ruvia::worker_wait_result<std::uint64_t>> receive_for(
            std::chrono::steady_clock::duration timeout, ruvia::stop_token stop_token) const {
            if (!receiver_) {
                throw std::logic_error("signal subscription is closed");
            }
            return receiver_->receive_for(timeout, std::move(stop_token));
        }

        // Unregisters every key now; later signals no longer reach this owner.
        void close() noexcept {
            if (!state_) {
                return;
            }
            if (receiver_) {
                receiver_->close();
            }
            for (const auto& key : keys_) {
                state_->unsubscribe(key, id_);
            }
            --state_->subscriptions_;
            state_.reset();
            receiver_.reset();
        }

    private:
        friend class keyed_signal_hub;
        subscription(std::shared_ptr<state> owner, std::uint64_t id, std::vector<key_type> keys,
            ruvia::channel_receiver<std::uint64_t> receiver)
            : state_(std::move(owner)),
              id_(id),
              keys_(std::move(keys)),
              receiver_(std::move(receiver)) {}

        std::shared_ptr<state> state_;
        std::uint64_t id_{};
        std::vector<key_type> keys_;
        std::optional<ruvia::channel_receiver<std::uint64_t>> receiver_;
    };

    keyed_signal_hub() = default;
    keyed_signal_hub(const keyed_signal_hub&) = delete;
    keyed_signal_hub& operator=(const keyed_signal_hub&) = delete;
    keyed_signal_hub(keyed_signal_hub&&) noexcept = default;
    keyed_signal_hub& operator=(keyed_signal_hub&&) noexcept = default;

    [[nodiscard]] subscription subscribe(const ruvia::worker_handle& worker, key_type key, value_type value = {}) {
        std::vector<std::pair<key_type, value_type>> registrations;
        registrations.emplace_back(std::move(key), std::move(value));
        return subscribe(worker, std::move(registrations));
    }

    [[nodiscard]] subscription subscribe(const ruvia::worker_handle& worker,
        std::vector<std::pair<key_type, value_type>> registrations) {
        if (registrations.empty()) {
            throw std::invalid_argument("signal subscription requires a key");
        }
        // Senders outlive the request that subscribed: never allocate them
        // from a request-scoped memory resource.
        auto [sender, receiver] = ruvia::make_channel<std::uint64_t>(
            worker, {.capacity_ = 1, .resource_ = std::pmr::new_delete_resource()});
        const auto id = state_->next_subscription_++;
        std::vector<key_type> keys;
        keys.reserve(registrations.size());
        for (auto& [key, value] : registrations) {
            state_->registrations_[key].push_back({.subscription_ = id, .value_ = std::move(value), .sender_ = sender});
            keys.push_back(std::move(key));
        }
        ++state_->subscriptions_;
        return subscription(state_, id, std::move(keys), std::move(receiver));
    }

    // Wakes every subscription registered under `key`.
    template <typename lookup_type>
    void publish(const lookup_type& key) {
        publish(key, [](value_type&) { return true; });
    }

    // `visit(value)` decides per registration of `key` whether it is woken;
    // it may update the registration's value before the wakeup.
    template <typename lookup_type, typename visitor_type>
    void publish(const lookup_type& key, visitor_type&& visit) {
        const auto found = state_->registrations_.find(key);
        if (found == state_->registrations_.end()) {
            return;
        }
        const auto signal = state_->next_signal_++;
        for (auto& value : found->second) {
            if (visit(value.value_)) {
                (void)value.sender_.send(signal);
            }
        }
    }

    // `visit(key, value)` decides for every registration whether it is woken.
    template <typename visitor_type>
    void publish_if(visitor_type&& visit) {
        const auto signal = state_->next_signal_++;
        for (auto& [key, values] : state_->registrations_) {
            for (auto& value : values) {
                if (visit(key, value.value_)) {
                    (void)value.sender_.send(signal);
                }
            }
        }
    }

    // Signals may have been missed: every subscription re-reads once.
    void publish_all() {
        publish_if([](const key_type&, value_type&) { return true; });
    }

    template <typename lookup_type>
    [[nodiscard]] bool contains(const lookup_type& key) const {
        return state_->registrations_.find(key) != state_->registrations_.end();
    }

    // Number of live subscriptions, however many keys each one registered.
    [[nodiscard]] std::size_t size() const noexcept {
        return state_->subscriptions_;
    }

private:
    std::shared_ptr<state> state_{std::make_shared<state>()};
};

}  // namespace service::background
