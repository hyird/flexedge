#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <ruvia/core/Channel.h>
#include "service/features/live_resource/query_key.h"

namespace service::live_resource {

// Only owned, allocator-independent bytes cross worker boundaries. In
// particular, request DTOs and HttpError objects must never enter this cache.
struct Snapshot final {
    std::string data;
    bool failure{};
};

class SnapshotCapacityError final : public std::runtime_error {
  public:
    SnapshotCapacityError() : std::runtime_error("live resource cache capacity exceeded") {}
};

class SnapshotCache final {
  public:
    using Clock = std::chrono::steady_clock;
    struct Limits final {
        std::size_t groups{1024};
        std::size_t subscribers{8192};
        std::size_t flights{128};
        std::size_t bytes{32 * 1024 * 1024};
        std::size_t snapshotBytes{4 * 1024 * 1024};
        std::size_t keyBytes{4096};
        // Lazy expiry bounds reuse of time-dependent projections. Heartbeats
        // never load resources; expiry is checked only when a read is requested.
        Clock::duration maxAge{std::chrono::seconds(15)};
    };
    struct Stats final {
        std::size_t groups{}, subscribers{}, flights{}, bytes{};
    };
    struct Result final {
        std::shared_ptr<const Snapshot> snapshot;
        std::uint64_t version{};
        std::uint64_t patchEpoch{};
    };

  private:
    struct Waiter final {
        ruvia::ChannelSender<Result> sender;
    };
    struct Entry final {
        SnapshotKey key;
        std::size_t subscribers{};
        std::uint64_t version{1};
        std::uint64_t patchEpoch{};
        bool loading{};
        std::shared_ptr<const Snapshot> snapshot;
        Clock::time_point expires{}, used{};
        std::vector<std::weak_ptr<Waiter>> waiters;
    };
    struct State final {
        explicit State(Limits value) : limits(value) {}
        std::mutex mutex;
        Limits limits;
        std::unordered_map<SnapshotKey, std::shared_ptr<Entry>, SnapshotKeyHash> entries;
        std::size_t subscribers{}, flights{}, bytes{};

        void clear(Entry& entry) {
            if (entry.snapshot) {
                bytes -= entry.snapshot->data.size();
                entry.snapshot.reset();
            }
        }
    };

  public:
    class Ticket final {
      public:
        // Called once by the detached worker, including when posting fails.
        void complete(std::shared_ptr<const Snapshot> snapshot) const {
            std::vector<std::shared_ptr<Waiter>> waiters;
            Result result;
            {
                const std::lock_guard lock(state_->mutex);
                if (!entry_->loading)
                    return;
                entry_->loading = false;
                --state_->flights;
                for (auto& weak : entry_->waiters)
                    if (auto waiter = weak.lock())
                        waiters.push_back(std::move(waiter));
                entry_->waiters.clear();
                // A business commit invalidated this read, or everyone
                // disconnected. Never install or distribute its old result.
                if (entry_->version == version_ && entry_->subscribers != 0) {
                    result = {std::move(snapshot), version_, patchEpoch_};
                    const auto bytes = result.snapshot->data.size();
                    if (entry_->patchEpoch == patchEpoch_ && !result.snapshot->failure &&
                        bytes <= state_->limits.snapshotBytes &&
                        bytes <= state_->limits.bytes) {
                        while (state_->bytes > state_->limits.bytes - bytes) {
                            std::shared_ptr<Entry> oldest;
                            for (const auto& [key, value] : state_->entries) {
                                (void)key;
                                if (value->snapshot && (!oldest || value->used < oldest->used))
                                    oldest = value;
                            }
                            if (!oldest)
                                break;
                            state_->clear(*oldest);
                            ++oldest->version;
                        }
                        state_->clear(*entry_);
                        entry_->snapshot = result.snapshot;
                        entry_->used = Clock::now();
                        entry_->expires = entry_->used + state_->limits.maxAge;
                        state_->bytes += bytes;
                    }
                }
            }
            // Empty results ask readers to join one new-generation read.
            for (const auto& waiter : waiters)
                (void)waiter->sender.send(result);
        }

      private:
        friend class SnapshotCache;
        Ticket(std::shared_ptr<State> state, std::shared_ptr<Entry> entry, std::uint64_t version,
               std::uint64_t patchEpoch)
            : state_(std::move(state)), entry_(std::move(entry)), version_(version), patchEpoch_(patchEpoch) {}
        std::shared_ptr<State> state_;
        std::shared_ptr<Entry> entry_;
        std::uint64_t version_;
        std::uint64_t patchEpoch_;
    };

    struct Read final {
        Result ready;
        std::optional<Ticket> ticket;
        // Weak waiter storage means an aborted request cannot accumulate
        // channels while a slow DB query finishes for the other subscribers.
        std::shared_ptr<Waiter> waiter;
        ruvia::ChannelReceiver<Result> receiver;
    };

    class Lease final {
      public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) noexcept = default;
        ~Lease() {
            if (!entry_)
                return;
            const std::lock_guard lock(state_->mutex);
            --state_->subscribers;
            if (--entry_->subscribers == 0) {
                state_->clear(*entry_);
                state_->entries.erase(entry_->key);
                entry_->waiters.clear();
            }
        }

        Read read(const ruvia::WorkerHandle& worker, Clock::time_point now = Clock::now()) const {
            auto [sender, receiver] = ruvia::makeChannel<Result>(
                worker, {.capacity = 1, .resource = std::pmr::new_delete_resource()});
            Read result{{}, {}, {}, std::move(receiver)};
            const std::lock_guard lock(state_->mutex);
            if (entry_->snapshot && now < entry_->expires) {
                entry_->used = now;
                result.ready = {entry_->snapshot, entry_->version, entry_->patchEpoch};
                return result;
            }
            if (entry_->snapshot) {
                state_->clear(*entry_);
                ++entry_->version;
            }
            if (!entry_->loading && state_->flights >= state_->limits.flights)
                throw SnapshotCapacityError();
            result.waiter = std::make_shared<Waiter>(std::move(sender));
            std::erase_if(entry_->waiters, [](const auto& weak) { return weak.expired(); });
            entry_->waiters.push_back(result.waiter);
            if (!entry_->loading) {
                result.ticket = Ticket(state_, entry_, entry_->version, entry_->patchEpoch);
                entry_->loading = true;
                ++state_->flights;
            }
            return result;
        }
        bool current(std::uint64_t version) const {
            const std::lock_guard lock(state_->mutex);
            return entry_->version == version;
        }
        std::uint64_t patchEpoch() const {
            const std::lock_guard lock(state_->mutex);
            return entry_->patchEpoch;
        }
        bool current(const Result& result, std::uint64_t minimumPatchEpoch = 0) const {
            const std::lock_guard lock(state_->mutex);
            // A subscriber can replay all patches delivered since it joined.
            // A late join cannot use a query begun before that boundary: some
            // intervening patches were never in its mailbox. It joins the next
            // read once, without starving on subsequent high-rate heartbeats.
            return entry_->version == result.version &&
                   result.patchEpoch >= std::max(joinedEpoch_, minimumPatchEpoch);
        }

      private:
        friend class SnapshotCache;
        Lease(std::shared_ptr<State> state, std::shared_ptr<Entry> entry)
            : state_(std::move(state)), entry_(std::move(entry)), joinedEpoch_(entry_->patchEpoch) {}
        std::shared_ptr<State> state_;
        std::shared_ptr<Entry> entry_;
        std::uint64_t joinedEpoch_;
    };

    SnapshotCache() : SnapshotCache(Limits{}) {}
    explicit SnapshotCache(Limits limits) : state_(std::make_shared<State>(limits)) {}
    Lease subscribe(SnapshotKey key) {
        if (key.tenant.empty() || key.query.empty())
            throw std::invalid_argument("snapshot subscription requires tenant and query key");
        const std::lock_guard lock(state_->mutex);
        if (key.tenant.size() + key.id.size() + key.query.size() > state_->limits.keyBytes ||
            state_->subscribers >= state_->limits.subscribers)
            throw SnapshotCapacityError();
        auto found = state_->entries.find(key);
        if (found == state_->entries.end()) {
            if (state_->entries.size() >= state_->limits.groups)
                throw SnapshotCapacityError();
            auto entry = std::make_shared<Entry>();
            entry->key = key;
            found = state_->entries.emplace(std::move(key), std::move(entry)).first;
        }
        ++state_->subscribers;
        ++found->second->subscribers;
        return Lease(state_, found->second);
    }

    void invalidate(std::string_view tenant, Resource resource, std::string_view id = {},
                    bool detailOnly = false, bool patch = false) {
        const std::lock_guard lock(state_->mutex);
        for (const auto& [key, entry] : state_->entries) {
            if (key.tenant != tenant || key.resource != resource ||
                (!key.id.empty() && !id.empty() && key.id != id) ||
                (detailOnly && key.id.empty()))
                continue;
            if (patch)
                ++entry->patchEpoch;
            else
                ++entry->version;
            state_->clear(*entry);
        }
    }
    Stats stats() const {
        const std::lock_guard lock(state_->mutex);
        return {state_->entries.size(), state_->subscribers, state_->flights, state_->bytes};
    }

  private:
    std::shared_ptr<State> state_;
};

} // namespace service::live_resource
