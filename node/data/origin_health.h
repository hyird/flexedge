#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flexedge::node {

class origin_health_registry final {
public:
    struct snapshot final {
        std::string website_id_;
        std::string origin_id_;
        std::string status_;
        std::int64_t checked_at_unix_millis_{};
        std::uint32_t latency_millis_{};
        std::string last_error_;
    };
    struct key final {
        std::string website_id_;
        std::string origin_id_;

        bool operator==(const key&) const = default;
    };

    struct key_view final {
        std::string_view website_id_;
        std::string_view origin_id_;
    };

    struct key_hash final {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(const key& value) const noexcept {
            return (*this)(key_view{value.website_id_, value.origin_id_});
        }

        [[nodiscard]] std::size_t operator()(key_view value) const noexcept {
            const auto left = std::hash<std::string_view>{}(value.website_id_);
            const auto right = std::hash<std::string_view>{}(value.origin_id_);
            return left ^ (right + 0x9e3779b9U + (left << 6U) + (left >> 2U));
        }
    };

    struct key_equal final {
        using is_transparent = void;

        [[nodiscard]] bool operator()(const key& left, const key& right) const noexcept {
            return left == right;
        }

        [[nodiscard]] bool operator()(const key& left, key_view right) const noexcept {
            return left.website_id_ == right.website_id_ && left.origin_id_ == right.origin_id_;
        }

        [[nodiscard]] bool operator()(key_view left, const key& right) const noexcept {
            return (*this)(right, left);
        }
    };

    using key_set = std::unordered_set<key, key_hash, key_equal>;

private:
    struct state;
    using state_map = std::unordered_map<key, std::shared_ptr<state>, key_hash, key_equal>;

public:
    class prepared_states final {
    public:
        prepared_states(const prepared_states&) = default;
        prepared_states& operator=(const prepared_states&) = default;

    private:
        friend class origin_health_registry;
        explicit prepared_states(std::shared_ptr<const state_map> states)
            : states_(std::move(states)) {}
        std::shared_ptr<const state_map> states_;
    };

    [[nodiscard]] static key get_key(std::string_view website_id, std::string_view origin_id) {
        return {.website_id_ = std::string(website_id), .origin_id_ = std::string(origin_id)};
    }

    [[nodiscard]] bool healthy(std::string_view website_id, std::string_view origin_id) const {
        const auto states = get_snapshot();
        const auto found = states->find(key_view{website_id, origin_id});
        return found == states->end() ||
               is_healthy(found->second->status_.load(std::memory_order_relaxed));
    }

    [[nodiscard]] bool all_healthy() const {
        const auto states = get_snapshot();
        for (const auto& [_, state] : *states) {
            if (!is_healthy(state->status_.load(std::memory_order_relaxed))) {
                return false;
            }
        }
        return true;
    }

    class target final {
    public:
        target() = default;
        void success(std::uint32_t threshold) const noexcept {
            if (state_) {
                update_status(state_->status_, true, threshold);
            }
        }
        void failure(std::uint32_t threshold) const noexcept {
            if (state_) {
                update_status(state_->status_, false, threshold);
            }
        }
        void record_probe(bool healthy, std::uint32_t threshold,
            std::uint32_t latency_millis, std::string_view error = {}) {
            const auto& state = state_;
            if (!state) {
                return;
            }
            update_status(state->status_, healthy, threshold);
            state->checked_at_unix_millis_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
            state->latency_millis_.store(latency_millis, std::memory_order_relaxed);
            std::scoped_lock lock(state->detail_mutex_);
            state->last_error_.assign(error);
        }

    private:
        friend class origin_health_registry;
        explicit target(std::shared_ptr<state> state)
            : state_(std::move(state)) {}
        std::shared_ptr<state> state_;
    };

    [[nodiscard]] target get_target(std::string_view website_id,
        std::string_view origin_id) const {
        return target(state_for(website_id, origin_id));
    }

    [[nodiscard]] std::vector<snapshot> reports() const {
        const auto states = get_snapshot();
        std::vector<snapshot> result;
        result.reserve(states->size());
        for (const auto& [key, state] : *states) {
            snapshot item{.website_id_ = key.website_id_,
                .origin_id_ = key.origin_id_,
                .status_ = "unknown",
                .checked_at_unix_millis_ =
                    state->checked_at_unix_millis_.load(std::memory_order_relaxed),
                .latency_millis_ = state->latency_millis_.load(std::memory_order_relaxed),
                .last_error_ = {}};
            if (item.checked_at_unix_millis_ != 0) {
                item.status_ = is_healthy(state->status_.load(std::memory_order_relaxed))
                                   ? "healthy"
                                   : "unhealthy";
            }
            std::scoped_lock lock(state->detail_mutex_);
            item.last_error_ = state->last_error_;
            result.push_back(std::move(item));
        }
        return result;
    }

    [[nodiscard]] bool claim_due(std::string_view website_id, std::string_view origin_id,
        std::chrono::seconds interval) {
        const auto state = state_for(website_id, origin_id);
        if (!state) {
            return false;
        }
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        const auto minimum_age =
            std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count();
        auto previous = state->last_probe_ns_.load(std::memory_order_relaxed);
        for (;;) {
            if (previous != 0 && now - previous < minimum_age) {
                return false;
            }
            if (state->last_probe_ns_.compare_exchange_weak(previous, now,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    // Prepare without changing the active registry. Shared state objects preserve
    // probe results received while the configuration transaction is pending.
    [[nodiscard]] prepared_states prepare(const key_set& keys) const {
        const auto current = get_snapshot();
        auto next = std::make_shared<state_map>();
        next->reserve(keys.size());
        for (const auto& value : keys) {
            const auto found = current->find(value);
            next->emplace(value,
                found == current->end() ? std::make_shared<state>() : found->second);
        }
        return prepared_states(std::move(next));
    }

    void publish(const prepared_states& states) noexcept {
        states_.store(states.states_, std::memory_order_release);
    }

private:
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) state final {
        std::atomic<std::uint64_t> status_{healthy_bit};
        std::atomic<std::int64_t> last_probe_ns_{};
        std::atomic<std::int64_t> checked_at_unix_millis_{};
        std::atomic<std::uint32_t> latency_millis_{};
        std::mutex detail_mutex_;
        std::string last_error_;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    [[nodiscard]] std::shared_ptr<const state_map> get_snapshot() const noexcept {
        return states_.load(std::memory_order_acquire);
    }

    // Membership belongs to configuration publication. Late request/probe
    // results must never resurrect a retired origin.
    [[nodiscard]] std::shared_ptr<state> state_for(std::string_view website_id,
        std::string_view origin_id) const {
        const auto current = get_snapshot();
        const auto found = current->find(key_view{website_id, origin_id});
        return found == current->end() ? nullptr : found->second;
    }

    static constexpr std::uint64_t healthy_bit{std::uint64_t{1} << 63U};
    static constexpr std::uint64_t failure_bit{std::uint64_t{1} << 62U};
    static constexpr std::uint64_t count_mask{failure_bit - 1};

    [[nodiscard]] static bool is_healthy(std::uint64_t status) noexcept {
        return (status & healthy_bit) != 0;
    }

    static void update_status(std::atomic<std::uint64_t>& target, bool successful,
        std::uint32_t threshold) noexcept {
        auto current = target.load(std::memory_order_relaxed);
        for (;;) {
            const auto same_direction = ((current & failure_bit) == 0) == successful;
            const auto current_count = current & count_mask;
            const auto next_count = same_direction ? (std::min)(current_count + 1, count_mask) : 1;
            auto next = (current & healthy_bit) | (successful ? 0 : failure_bit) | next_count;
            if (next_count >= threshold) {
                if (successful) {
                    next |= healthy_bit;
                } else {
                    next &= ~healthy_bit;
                }
            }
            if (target.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    std::atomic<std::shared_ptr<const state_map>> states_{std::make_shared<state_map>()};
};

}  // namespace flexedge::node
