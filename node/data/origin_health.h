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

class OriginHealthRegistry final {
  public:
    struct Snapshot final {
        std::string websiteId;
        std::string originId;
        std::string status;
        std::int64_t checkedAtUnixMillis{};
        std::uint32_t latencyMillis{};
        std::string lastError;
    };
    struct Key final {
        std::string websiteId;
        std::string originId;

        bool operator==(const Key&) const = default;
    };

    struct KeyView final {
        std::string_view websiteId;
        std::string_view originId;
    };

    struct KeyHash final {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(const Key& value) const noexcept {
            return (*this)(KeyView{value.websiteId, value.originId});
        }

        [[nodiscard]] std::size_t operator()(KeyView value) const noexcept {
            const auto left = std::hash<std::string_view>{}(value.websiteId);
            const auto right = std::hash<std::string_view>{}(value.originId);
            return left ^ (right + 0x9e3779b9U + (left << 6U) + (left >> 2U));
        }
    };

    struct KeyEqual final {
        using is_transparent = void;

        [[nodiscard]] bool operator()(const Key& left, const Key& right) const noexcept {
            return left == right;
        }

        [[nodiscard]] bool operator()(const Key& left, KeyView right) const noexcept {
            return left.websiteId == right.websiteId && left.originId == right.originId;
        }

        [[nodiscard]] bool operator()(KeyView left, const Key& right) const noexcept {
            return (*this)(right, left);
        }
    };

    using KeySet = std::unordered_set<Key, KeyHash, KeyEqual>;

    [[nodiscard]] static Key key(std::string_view websiteId, std::string_view originId) {
        return {.websiteId = std::string(websiteId), .originId = std::string(originId)};
    }

    [[nodiscard]] bool healthy(std::string_view websiteId, std::string_view originId) const {
        const auto states = snapshot();
        const auto found = states->find(KeyView{websiteId, originId});
        return found == states->end() ||
               isHealthy(found->second->status.load(std::memory_order_relaxed));
    }

    [[nodiscard]] bool allHealthy() const {
        const auto states = snapshot();
        for (const auto& [_, state] : *states) {
            if (!isHealthy(state->status.load(std::memory_order_relaxed))) {
                return false;
            }
        }
        return true;
    }

    void success(std::string_view websiteId, std::string_view originId, std::uint32_t threshold) {
        const auto state = stateFor(websiteId, originId);
        updateStatus(state->status, true, threshold);
    }

    void failure(std::string_view websiteId, std::string_view originId, std::uint32_t threshold) {
        const auto state = stateFor(websiteId, originId);
        updateStatus(state->status, false, threshold);
    }

    void recordProbe(std::string_view websiteId, std::string_view originId, bool healthy,
                     std::uint32_t threshold, std::uint32_t latencyMillis,
                     std::string_view error = {}) {
        const auto state = stateFor(websiteId, originId);
        updateStatus(state->status, healthy, threshold);
        state->checkedAtUnixMillis.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count(),
                                         std::memory_order_relaxed);
        state->latencyMillis.store(latencyMillis, std::memory_order_relaxed);
        std::scoped_lock lock(state->detailMutex);
        state->lastError.assign(error);
    }

    [[nodiscard]] std::vector<Snapshot> reports() const {
        const auto states = snapshot();
        std::vector<Snapshot> result;
        result.reserve(states->size());
        for (const auto& [key, state] : *states) {
            Snapshot item{.websiteId = key.websiteId,
                          .originId = key.originId,
                          .status = "unknown",
                          .checkedAtUnixMillis =
                              state->checkedAtUnixMillis.load(std::memory_order_relaxed),
                          .latencyMillis = state->latencyMillis.load(std::memory_order_relaxed),
                          .lastError = {}};
            if (item.checkedAtUnixMillis != 0) {
                item.status = isHealthy(state->status.load(std::memory_order_relaxed))
                                  ? "healthy"
                                  : "unhealthy";
            }
            std::scoped_lock lock(state->detailMutex);
            item.lastError = state->lastError;
            result.push_back(std::move(item));
        }
        return result;
    }

    [[nodiscard]] bool claimDue(std::string_view websiteId, std::string_view originId,
                                std::chrono::seconds interval) {
        const auto state = stateFor(websiteId, originId);
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        const auto minimumAge =
            std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count();
        auto previous = state->lastProbeNs.load(std::memory_order_relaxed);
        for (;;) {
            if (previous != 0 && now - previous < minimumAge) {
                return false;
            }
            if (state->lastProbeNs.compare_exchange_weak(previous, now,
                                                         std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    void retain(const KeySet& keys) {
        auto current = snapshot();
        for (;;) {
            auto next = std::make_shared<StateMap>();
            next->reserve(keys.size());
            for (const auto& value : keys) {
                const auto found = current->find(value);
                next->emplace(value,
                              found == current->end() ? std::make_shared<State>() : found->second);
            }
            std::shared_ptr<const StateMap> desired = std::move(next);
            if (states_.compare_exchange_weak(current, std::move(desired),
                                              std::memory_order_release,
                                              std::memory_order_acquire)) {
                return;
            }
        }
    }

  private:
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) State final {
        std::atomic<std::uint64_t> status{kHealthyBit};
        std::atomic<std::int64_t> lastProbeNs{};
        std::atomic<std::int64_t> checkedAtUnixMillis{};
        std::atomic<std::uint32_t> latencyMillis{};
        std::mutex detailMutex;
        std::string lastError;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    using StateMap = std::unordered_map<Key, std::shared_ptr<State>, KeyHash, KeyEqual>;

    [[nodiscard]] std::shared_ptr<const StateMap> snapshot() const noexcept {
        return states_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::shared_ptr<State> stateFor(std::string_view websiteId,
                                                  std::string_view originId) {
        auto current = snapshot();
        for (;;) {
            const auto found = current->find(KeyView{websiteId, originId});
            if (found != current->end()) {
                return found->second;
            }
            auto next = std::make_shared<StateMap>(*current);
            auto state = std::make_shared<State>();
            next->emplace(key(websiteId, originId), state);
            std::shared_ptr<const StateMap> desired = std::move(next);
            if (states_.compare_exchange_weak(current, std::move(desired),
                                              std::memory_order_release,
                                              std::memory_order_acquire)) {
                return state;
            }
        }
    }

    static constexpr std::uint64_t kHealthyBit{std::uint64_t{1} << 63U};
    static constexpr std::uint64_t kFailureBit{std::uint64_t{1} << 62U};
    static constexpr std::uint64_t kCountMask{kFailureBit - 1};

    [[nodiscard]] static bool isHealthy(std::uint64_t status) noexcept {
        return (status & kHealthyBit) != 0;
    }

    static void updateStatus(std::atomic<std::uint64_t>& target, bool successful,
                             std::uint32_t threshold) noexcept {
        auto current = target.load(std::memory_order_relaxed);
        for (;;) {
            const auto sameDirection = ((current & kFailureBit) == 0) == successful;
            const auto currentCount = current & kCountMask;
            const auto nextCount = sameDirection ? (std::min)(currentCount + 1, kCountMask) : 1;
            auto next = (current & kHealthyBit) | (successful ? 0 : kFailureBit) | nextCount;
            if (nextCount >= threshold) {
                if (successful) {
                    next |= kHealthyBit;
                } else {
                    next &= ~kHealthyBit;
                }
            }
            if (target.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    std::atomic<std::shared_ptr<const StateMap>> states_{std::make_shared<StateMap>()};
};

} // namespace flexedge::node
