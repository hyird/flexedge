#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flexedge::node {

struct origin_health_key final {
    std::string website_id_;
    std::string origin_id_;

    bool operator==(const origin_health_key&) const = default;
};

struct origin_health_key_view final {
    std::string_view website_id_;
    std::string_view origin_id_;
};

struct origin_health_key_hash final {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(const origin_health_key& value) const noexcept {
        return (*this)(origin_health_key_view{value.website_id_, value.origin_id_});
    }

    [[nodiscard]] std::size_t operator()(origin_health_key_view value) const noexcept {
        const auto left = std::hash<std::string_view>{}(value.website_id_);
        const auto right = std::hash<std::string_view>{}(value.origin_id_);
        return left ^ (right + 0x9e3779b9U + (left << 6U) + (left >> 2U));
    }
};

struct origin_health_key_equal final {
    using is_transparent = void;

    [[nodiscard]] bool operator()(const origin_health_key& left,
        const origin_health_key& right) const noexcept {
        return left == right;
    }

    [[nodiscard]] bool operator()(const origin_health_key& left,
        origin_health_key_view right) const noexcept {
        return left.website_id_ == right.website_id_ && left.origin_id_ == right.origin_id_;
    }

    [[nodiscard]] bool operator()(origin_health_key_view left,
        const origin_health_key& right) const noexcept {
        return (*this)(right, left);
    }
};

using origin_health_key_set =
    std::unordered_set<origin_health_key, origin_health_key_hash, origin_health_key_equal>;

struct origin_health_report final {
    std::string website_id_;
    std::string origin_id_;
    std::string status_;
    std::int64_t checked_at_unix_millis_{};
    std::uint32_t latency_millis_{};
    std::string last_error_;
};

// Immutable health of every configured origin, published by the owner loop and read by any
// thread. Origins outside the snapshot are treated as healthy.
class origin_health_snapshot final {
public:
    struct origin final {
        // Identifies one membership of the origin; a retired and re-added origin gets a new id
        // so outcomes recorded against the old membership are discarded.
        std::uint64_t id_{};
        bool healthy_{true};
        bool last_success_{true};
        std::int64_t checked_at_unix_millis_{};
        std::uint32_t latency_millis_{};
        std::string last_error_;

        // Recording `success` would change neither health nor the direction of the streak.
        [[nodiscard]] bool settled(bool success) const noexcept {
            return healthy_ == success && last_success_ == success;
        }
    };

    using origin_map =
        std::unordered_map<origin_health_key, origin, origin_health_key_hash, origin_health_key_equal>;

    origin_health_snapshot() = default;
    explicit origin_health_snapshot(origin_map origins)
        : origins_(std::move(origins)) {}

    [[nodiscard]] const origin* find(std::string_view website_id,
        std::string_view origin_id) const noexcept {
        const auto found = origins_.find(origin_health_key_view{website_id, origin_id});
        return found == origins_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] bool healthy(std::string_view website_id, std::string_view origin_id) const noexcept {
        const auto* value = find(website_id, origin_id);
        return value == nullptr || value->healthy_;
    }

    [[nodiscard]] bool all_healthy() const noexcept {
        return std::ranges::all_of(origins_, [](const auto& entry) { return entry.second.healthy_; });
    }

    [[nodiscard]] std::vector<origin_health_report> reports() const {
        std::vector<origin_health_report> result;
        result.reserve(origins_.size());
        for (const auto& [key, value] : origins_) {
            result.push_back({.website_id_ = key.website_id_,
                .origin_id_ = key.origin_id_,
                .status_ = value.checked_at_unix_millis_ == 0 ? "unknown"
                           : value.healthy_                   ? "healthy"
                                                              : "unhealthy",
                .checked_at_unix_millis_ = value.checked_at_unix_millis_,
                .latency_millis_ = value.latency_millis_,
                .last_error_ = value.last_error_});
        }
        return result;
    }

private:
    origin_map origins_;
};

namespace detail {

// Mutable health of one origin membership. Only the owner loop touches it.
struct origin_health_entry final {
    std::uint64_t id_{};
    bool healthy_{true};
    bool last_success_{true};
    std::uint32_t streak_{};
    std::chrono::steady_clock::time_point last_probe_{};
    bool probed_{};
    std::int64_t checked_at_unix_millis_{};
    std::uint32_t latency_millis_{};
    std::string last_error_;

    // Returns whether the snapshot-visible state changed.
    bool record(bool success, std::uint32_t threshold) noexcept {
        const auto previous_healthy = healthy_;
        const auto previous_success = last_success_;
        if (success == last_success_) {
            streak_ = streak_ == (std::numeric_limits<std::uint32_t>::max)() ? streak_ : streak_ + 1;
        } else {
            last_success_ = success;
            streak_ = 1;
        }
        if (streak_ >= threshold) {
            healthy_ = success;
        }
        return healthy_ != previous_healthy || last_success_ != previous_success;
    }
};

}  // namespace detail

// Records active probe results for one origin membership; owner loop only. Results for a
// retired membership land on the detached entry and are never published.
class origin_probe_target final {
public:
    origin_probe_target() = default;

    void record_probe(bool healthy, std::uint32_t threshold, std::uint32_t latency_millis,
        std::string_view error = {}) {
        if (!entry_) {
            return;
        }
        (void)entry_->record(healthy, threshold);
        entry_->checked_at_unix_millis_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                              .count();
        entry_->latency_millis_ = latency_millis;
        entry_->last_error_.assign(error);
    }

private:
    friend class origin_health_state;
    explicit origin_probe_target(std::shared_ptr<detail::origin_health_entry> entry)
        : entry_(std::move(entry)) {}

    std::shared_ptr<detail::origin_health_entry> entry_;
};

// Health of the configured origins. Single-threaded: only the owner loop reads or writes it;
// other threads see it through the immutable snapshots it builds.
class origin_health_state final {
    using entry_map = std::unordered_map<origin_health_key,
        std::shared_ptr<detail::origin_health_entry>, origin_health_key_hash, origin_health_key_equal>;

public:
    // The next membership and its snapshot, built without changing the active state. Kept
    // entries are shared, so results recorded while the configuration is pending survive.
    class prepared final {
    public:
        prepared(prepared&&) noexcept = default;
        prepared& operator=(prepared&&) noexcept = default;
        prepared(const prepared&) = delete;
        prepared& operator=(const prepared&) = delete;

        [[nodiscard]] const std::shared_ptr<const origin_health_snapshot>& snapshot() const noexcept {
            return snapshot_;
        }

    private:
        friend class origin_health_state;
        prepared(entry_map entries, std::shared_ptr<const origin_health_snapshot> snapshot)
            : entries_(std::move(entries)),
              snapshot_(std::move(snapshot)) {}

        entry_map entries_;
        std::shared_ptr<const origin_health_snapshot> snapshot_;
    };

    [[nodiscard]] prepared prepare(const origin_health_key_set& keys) {
        entry_map next;
        next.reserve(keys.size());
        for (const auto& key : keys) {
            const auto found = entries_.find(key);
            if (found != entries_.end()) {
                next.emplace(key, found->second);
                continue;
            }
            auto entry = std::make_shared<detail::origin_health_entry>();
            entry->id_ = ++last_id_;
            next.emplace(key, std::move(entry));
        }
        auto next_snapshot = build(next);
        return prepared(std::move(next), std::move(next_snapshot));
    }

    // The snapshot of `next` misses results recorded after it was prepared; publish a fresh
    // snapshot once activated.
    void activate(prepared&& next) noexcept {
        entries_ = std::move(next.entries_);
    }

    // Applies a request outcome for membership `id`. Returns whether the snapshot changed.
    bool record(origin_health_key_view key, std::uint64_t id, bool success,
        std::uint32_t threshold) noexcept {
        const auto found = entries_.find(key);
        return found != entries_.end() && found->second->id_ == id &&
               found->second->record(success, threshold);
    }

    [[nodiscard]] origin_probe_target probe_target(origin_health_key_view key) const {
        const auto found = entries_.find(key);
        return found == entries_.end() ? origin_probe_target{} : origin_probe_target(found->second);
    }

    // Claims the next active probe of `key` once `interval` has elapsed since the previous one.
    [[nodiscard]] bool claim_due(origin_health_key_view key, std::chrono::seconds interval) {
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            return false;
        }
        auto& entry = *found->second;
        const auto now = std::chrono::steady_clock::now();
        if (entry.probed_ && now - entry.last_probe_ < interval) {
            return false;
        }
        entry.probed_ = true;
        entry.last_probe_ = now;
        return true;
    }

    [[nodiscard]] std::shared_ptr<const origin_health_snapshot> snapshot() const {
        return build(entries_);
    }

private:
    [[nodiscard]] static std::shared_ptr<const origin_health_snapshot> build(const entry_map& entries) {
        origin_health_snapshot::origin_map origins;
        origins.reserve(entries.size());
        for (const auto& [key, entry] : entries) {
            origins.emplace(key, origin_health_snapshot::origin{.id_ = entry->id_,
                                     .healthy_ = entry->healthy_,
                                     .last_success_ = entry->last_success_,
                                     .checked_at_unix_millis_ = entry->checked_at_unix_millis_,
                                     .latency_millis_ = entry->latency_millis_,
                                     .last_error_ = entry->last_error_});
        }
        return std::make_shared<const origin_health_snapshot>(std::move(origins));
    }

    entry_map entries_;
    std::uint64_t last_id_{};
};

}  // namespace flexedge::node
