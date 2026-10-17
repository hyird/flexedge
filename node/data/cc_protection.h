#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flexedge::node {

// All timestamps are steady-clock milliseconds in this node process, never wall time.
struct cc_key final {
    std::string website_, policy_, rule_, address_;
    bool operator==(const cc_key&) const = default;
};
struct cc_key_hash final {
    std::size_t operator()(const cc_key& value) const noexcept {
        std::size_t seed{};
        for (const auto* part : {&value.website_, &value.policy_, &value.rule_, &value.address_}) {
            seed ^= std::hash<std::string>{}(*part) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};
struct cc_limits final {
    std::uint32_t requests_{20}, window_ms_{10000}, mitigation_ms_{60000};
    bool operator==(const cc_limits&) const = default;
    void validate() const {
        if (!requests_ || requests_ > 1000000000 || window_ms_ < 1000 ||
            window_ms_ > 3600000 || mitigation_ms_ < 1000 || mitigation_ms_ > 86400000) {
            throw std::invalid_argument("invalid CC limits");
        }
    }
};
struct cc_count final {
    cc_key key_;
    cc_limits limits_;
    std::uint64_t bucket_{}, count_{};
};
struct cc_batch final {
    std::uint64_t instance_{}, sequence_{}, observed_ms_{};
    std::vector<cc_count> counts_{};
};
struct cc_dispositions final {
    std::uint64_t sequence_{};
    std::uint64_t saturated_until_{};
    std::unordered_map<cc_key, std::uint64_t, cc_key_hash> blocked_until_{};
};
enum class cc_decision { allow,
    block,
    overloaded };

inline std::uint64_t cc_now_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

namespace cc_detail {
inline constexpr std::size_t bucket_count = 64;
inline std::uint64_t width(const cc_limits& limits) noexcept {
    return (limits.window_ms_ + bucket_count - 1) / bucket_count;
}
inline std::uint64_t add(std::uint64_t left, std::uint64_t right) noexcept {
    return left > (std::numeric_limits<std::uint64_t>::max)() - right
               ? (std::numeric_limits<std::uint64_t>::max)()
               : left + right;
}
struct bucket final {
    std::uint64_t slot_{}, count_{};
};
// Sixty-four time buckets give a bounded sliding-window approximation. A bucket is
// retained until its end leaves the window, so rounding never drops recent requests.
inline bool live(std::uint64_t slot, std::uint64_t now, const cc_limits& limits) noexcept {
    return (slot + 1) * width(limits) + limits.window_ms_ > now;
}
}  // namespace cc_detail

// One per data Worker. No atomics, locks or mutable counter objects cross loops.
class cc_worker final {
public:
    explicit cc_worker(std::uint64_t instance, std::size_t capacity = 16384)
        : instance_(instance),
          capacity_(capacity) {}
    cc_worker(const cc_worker&) = delete;
    cc_worker& operator=(const cc_worker&) = delete;
    cc_worker(cc_worker&&) noexcept = default;
    cc_worker& operator=(cc_worker&&) noexcept = default;

    cc_decision check(const cc_key& key, const cc_limits& limits, std::uint64_t now) {
        if (dispositions_ && dispositions_->saturated_until_ > now) {
            return cc_decision::overloaded;
        }
        if (dispositions_) {
            const auto found = dispositions_->blocked_until_.find(key);
            if (found != dispositions_->blocked_until_.end() && found->second > now) {
                return cc_decision::block;
            }
        }
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            if (entries_.size() >= capacity_) {
                return cc_decision::overloaded;
            }
            limits.validate();
            found = entries_.emplace(key, entry{.limits_ = limits}).first;
        }
        auto& value = found->second;
        if (value.limits_ != limits) {
            throw std::logic_error("CC identity reused with different limits");
        }
        if (value.blocked_until_ > now) {
            return cc_decision::block;
        }
        const auto slot = now / cc_detail::width(limits);
        auto& bucket = value.buckets_[slot % value.buckets_.size()];
        if (bucket.slot_ != slot) {
            bucket = {.slot_ = slot};
        }
        bucket.count_ = cc_detail::add(bucket.count_, 1);
        value.dirty_ = true;
        value.last_seen_ = now;
        std::uint64_t sum{};
        for (const auto& item : value.buckets_) {
            if (cc_detail::live(item.slot_, now, limits)) {
                sum = cc_detail::add(sum, item.count_);
            }
        }
        if (sum > limits.requests_) {
            value.blocked_until_ = now + limits.mitigation_ms_;
            return cc_decision::block;
        }
        return cc_decision::allow;
    }

    // Send the returned immutable batch; acknowledge only if its channel accepted it.
    // This is synchronous on the owner loop, so no request can modify a count in between.
    std::unique_ptr<const cc_batch> report(std::uint64_t now) {
        cc_batch result{.instance_ = instance_, .sequence_ = ++sequence_, .observed_ms_ = now};
        std::erase_if(entries_, [&](const auto& pair) {
            const auto& value = pair.second;
            return !value.dirty_ && value.blocked_until_ <= now &&
                   now >= value.last_seen_ + value.limits_.window_ms_ + cc_detail::width(value.limits_);
        });
        for (const auto& [key, value] : entries_) {
            if (!value.dirty_) {
                continue;
            }
            for (const auto& bucket : value.buckets_) {
                if (bucket.count_ && cc_detail::live(bucket.slot_, now, value.limits_)) {
                    result.counts_.push_back({key, value.limits_, bucket.slot_, bucket.count_});
                }
            }
        }
        return std::make_unique<const cc_batch>(std::move(result));
    }
    void acknowledge() noexcept {
        for (auto& [key, value] : entries_) {
            (void)key;
            value.dirty_ = false;
        }
    }
    void publish(std::shared_ptr<const cc_dispositions> value) noexcept {
        if (value && (!dispositions_ || value->sequence_ > dispositions_->sequence_)) {
            dispositions_ = std::move(value);
        }
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

private:
    struct entry final {
        cc_limits limits_{};
        std::array<cc_detail::bucket, cc_detail::bucket_count + 1> buckets_{};
        std::uint64_t last_seen_{}, blocked_until_{};
        bool dirty_{};
    };
    std::uint64_t instance_{}, sequence_{};
    std::size_t capacity_;
    std::unordered_map<cc_key, entry, cc_key_hash> entries_;
    std::shared_ptr<const cc_dispositions> dispositions_;
};

// Node control loop only. Per-source absolute bucket counts make retries idempotent;
// Worker identities are scoped to this node process: this pool cannot restart an
// individual Worker (retirement terminates the node). Process restart resets both sides.
// No request is handed to this loop.
class cc_aggregator final {
public:
    explicit cc_aggregator(std::size_t worker_count, std::size_t capacity = 65536)
        : worker_count_(worker_count),
          capacity_(capacity) {}

    void merge(const cc_batch& batch, std::uint64_t now) {
        if (batch.instance_ >= worker_count_ || batch.observed_ms_ > now ||
            batch.sequence_ <= sequences_[batch.instance_]) {
            return;
        }
        sequences_[batch.instance_] = batch.sequence_;
        for (const auto& count : batch.counts_) {
            count.limits_.validate();
            if (!cc_detail::live(count.bucket_, now, count.limits_) ||
                count.bucket_ > batch.observed_ms_ / cc_detail::width(count.limits_)) {
                continue;
            }
            auto found = entries_.find(count.key_);
            if (found == entries_.end()) {
                if (entries_.size() >= capacity_) {
                    saturated_until_ = now + count.limits_.window_ms_;
                    continue;
                }
                found = entries_.emplace(count.key_, entry{.limits_ = count.limits_}).first;
            }
            auto& value = found->second;
            if (value.limits_ != count.limits_) {
                throw std::logic_error("CC report has conflicting limits");
            }
            const auto index = count.bucket_ % value.buckets_.size();
            auto& bucket = value.buckets_[index];
            if (bucket.slot_ != count.bucket_) {
                bucket = {.slot_ = count.bucket_};
            }
            auto& previous = bucket.sources_[batch.instance_];
            if (count.count_ > previous) {
                bucket.total_ = cc_detail::add(bucket.total_, count.count_ - previous);
                previous = count.count_;
                value.changed_ = true;
            }
        }
    }

    std::shared_ptr<const cc_dispositions> snapshot(std::uint64_t now) {
        auto result = std::make_shared<cc_dispositions>();
        result->sequence_ = ++sequence_;
        result->saturated_until_ = saturated_until_ > now ? saturated_until_ : 0;
        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            auto& [key, value] = *iterator;
            std::uint64_t sum{};
            for (auto& bucket : value.buckets_) {
                if (cc_detail::live(bucket.slot_, now, value.limits_)) {
                    sum = cc_detail::add(sum, bucket.total_);
                } else {
                    bucket = {};
                }
            }
            // Do not renew a mitigation just because the same counters were resent.
            if (value.changed_ && sum > value.limits_.requests_ && value.blocked_until_ <= now) {
                value.blocked_until_ = now + value.limits_.mitigation_ms_;
            }
            value.changed_ = false;
            if (value.blocked_until_ > now) {
                result->blocked_until_.emplace(key, value.blocked_until_);
            }
            if (sum == 0 && value.blocked_until_ <= now) {
                iterator = entries_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        return result;
    }

private:
    struct bucket final {
        std::uint64_t slot_{}, total_{};
        std::map<std::uint64_t, std::uint64_t> sources_{};
    };
    struct entry final {
        cc_limits limits_{};
        std::array<bucket, cc_detail::bucket_count + 1> buckets_{};
        std::uint64_t blocked_until_{};
        bool changed_{};
    };
    std::size_t worker_count_, capacity_;
    std::uint64_t sequence_{}, saturated_until_{};
    std::unordered_map<std::uint64_t, std::uint64_t> sequences_;
    std::unordered_map<cc_key, entry, cc_key_hash> entries_;
};

}  // namespace flexedge::node
