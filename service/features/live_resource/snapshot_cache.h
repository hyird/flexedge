#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/worker_handle.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/features/live_resource/query_key.h"

namespace service::live_resource {

// Only owned, allocator-independent bytes cross worker boundaries. In
// particular, request DTOs and HttpError objects must never enter this cache.
struct snapshot final {
    std::string data_;
    bool failure_{};
};

class snapshot_capacity_error final : public std::runtime_error {
public:
    snapshot_capacity_error()
        : std::runtime_error("live resource cache capacity exceeded") {}
};

class snapshot_cache final {
public:
    using clock = std::chrono::steady_clock;
    struct limits final {
        std::size_t groups_{1024};
        std::size_t subscribers_{8192};
        std::size_t flights_{128};
        std::size_t bytes_{32 * 1024 * 1024};
        std::size_t snapshot_bytes_{4 * 1024 * 1024};
        std::size_t key_bytes_{4096};
        // Lazy expiry bounds reuse of time-dependent projections. Heartbeats
        // never load resources; expiry is checked only when a read is requested.
        clock::duration max_age_{std::chrono::seconds(15)};
    };
    struct stats final {
        std::size_t groups_{}, subscribers_{}, flights_{}, bytes_{};
    };
    struct result final {
        std::shared_ptr<const snapshot> snapshot_;
        std::uint64_t version_{};
        std::uint64_t patch_epoch_{};
    };

private:
    struct waiter final {
        ruvia::channel_sender<result> sender_;
    };
    struct entry final {
        snapshot_key key_;
        std::size_t subscribers_{};
        std::uint64_t version_{1};
        std::uint64_t patch_epoch_{};
        bool loading_{};
        std::uint64_t next_flight_generation_{};
        std::uint64_t flight_generation_{};
        std::shared_ptr<const snapshot> snapshot_;
        clock::time_point expires_{}, used_{};
        std::vector<std::weak_ptr<waiter>> waiters_;
    };
    struct state final {
        explicit state(limits value)
            : limits_(value) {}
        std::mutex mutex_;
        limits limits_;
        std::unordered_map<snapshot_key, std::shared_ptr<entry>, snapshot_key_hash> entries_;
        std::size_t subscribers_{}, flights_{}, bytes_{};

        void clear(entry& entry) {
            if (entry.snapshot_) {
                bytes_ -= entry.snapshot_->data_.size();
                entry.snapshot_.reset();
            }
        }
    };

public:
    class ticket final {
    public:
        void complete(std::shared_ptr<const snapshot> snapshot) const {
            finish(std::move(snapshot));
        }

        // Release an interrupted flight without publishing a snapshot.
        void cancel() const {
            finish({});
        }

    private:
        friend class snapshot_cache;
        ticket(std::shared_ptr<state> state, std::shared_ptr<entry> entry, std::uint64_t version,
            std::uint64_t patch_epoch, std::uint64_t flight_generation)
            : state_(std::move(state)),
              entry_(std::move(entry)),
              version_(version),
              patch_epoch_(patch_epoch),
              flight_generation_(flight_generation) {}

        void finish(std::shared_ptr<const snapshot> snapshot) const {
            std::vector<std::weak_ptr<waiter>> waiters;
            result result;
            {
                const std::lock_guard lock(state_->mutex_);
                if (!entry_->loading_ || entry_->flight_generation_ != flight_generation_) {
                    return;
                }
                entry_->loading_ = false;
                --state_->flights_;
                waiters.swap(entry_->waiters_);
                // A business commit invalidated this read, or everyone
                // disconnected. Never install or distribute its old result.
                if (snapshot && entry_->version_ == version_ && entry_->subscribers_ != 0) {
                    result = {std::move(snapshot), version_, patch_epoch_};
                    const auto bytes = result.snapshot_->data_.size();
                    if (entry_->patch_epoch_ == patch_epoch_ && !result.snapshot_->failure_ &&
                        bytes <= state_->limits_.snapshot_bytes_ && bytes <= state_->limits_.bytes_) {
                        while (state_->bytes_ > state_->limits_.bytes_ - bytes) {
                            std::shared_ptr<entry> oldest;
                            for (const auto& [key, value] : state_->entries_) {
                                (void)key;
                                if (value->snapshot_ && (!oldest || value->used_ < oldest->used_)) {
                                    oldest = value;
                                }
                            }
                            if (!oldest) {
                                break;
                            }
                            state_->clear(*oldest);
                            ++oldest->version_;
                        }
                        state_->clear(*entry_);
                        entry_->snapshot_ = result.snapshot_;
                        entry_->used_ = clock::now();
                        entry_->expires_ = entry_->used_ + state_->limits_.max_age_;
                        state_->bytes_ += bytes;
                    }
                }
            }
            // Do not allocate while releasing a generation: an old ticket must
            // never strand or accidentally release the next flight.
            for (const auto& weak : waiters) {
                if (auto waiter = weak.lock()) {
                    (void)waiter->sender_.send(result);
                }
            }
        }

        std::shared_ptr<state> state_;
        std::shared_ptr<entry> entry_;
        std::uint64_t version_;
        std::uint64_t patch_epoch_;
        std::uint64_t flight_generation_;
    };

    struct read final {
        result ready_;
        std::optional<ticket> ticket_;
        // Weak waiter storage means an aborted request cannot accumulate
        // channels while a slow DB query finishes for the other subscribers.
        std::shared_ptr<waiter> waiter_;
        ruvia::channel_receiver<result> receiver_;
    };

    class lease final {
    public:
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
        lease(lease&&) noexcept = default;
        ~lease() {
            if (!entry_) {
                return;
            }
            const std::lock_guard lock(state_->mutex_);
            --state_->subscribers_;
            if (--entry_->subscribers_ == 0) {
                state_->clear(*entry_);
                state_->entries_.erase(entry_->key_);
                entry_->waiters_.clear();
            }
        }

        service::live_resource::snapshot_cache::read read(const ruvia::worker_handle& worker, clock::time_point now = clock::now()) const {
            auto [sender, receiver] = ruvia::make_channel<result>(
                worker, {.capacity_ = 1, .resource_ = std::pmr::new_delete_resource()});
            service::live_resource::snapshot_cache::read result{{}, {}, {}, std::move(receiver)};
            const std::lock_guard lock(state_->mutex_);
            if (entry_->snapshot_ && now < entry_->expires_) {
                entry_->used_ = now;
                result.ready_ = {entry_->snapshot_, entry_->version_, entry_->patch_epoch_};
                return result;
            }
            if (entry_->snapshot_) {
                state_->clear(*entry_);
                ++entry_->version_;
            }
            if (!entry_->loading_ && state_->flights_ >= state_->limits_.flights_) {
                throw snapshot_capacity_error();
            }
            if (!entry_->loading_ && entry_->next_flight_generation_ ==
                                         (std::numeric_limits<std::uint64_t>::max)()) {
                throw std::overflow_error("snapshot flight generation exhausted");
            }
            result.waiter_ = std::make_shared<waiter>(std::move(sender));
            std::erase_if(entry_->waiters_, [](const auto& weak) { return weak.expired(); });
            entry_->waiters_.push_back(result.waiter_);
            if (!entry_->loading_) {
                const auto generation = ++entry_->next_flight_generation_;
                result.ticket_ = ticket(state_, entry_, entry_->version_, entry_->patch_epoch_, generation);
                entry_->flight_generation_ = generation;
                entry_->loading_ = true;
                ++state_->flights_;
            }
            return result;
        }
        bool current(std::uint64_t version) const {
            const std::lock_guard lock(state_->mutex_);
            return entry_->version_ == version;
        }
        std::uint64_t patch_epoch() const {
            const std::lock_guard lock(state_->mutex_);
            return entry_->patch_epoch_;
        }
        bool current(const result& result, std::uint64_t minimum_patch_epoch = 0) const {
            const std::lock_guard lock(state_->mutex_);
            // A subscriber can replay all patches delivered since it joined.
            // A late join cannot use a query begun before that boundary: some
            // intervening patches were never in its mailbox. It joins the next
            // read once, without starving on subsequent high-rate heartbeats.
            return entry_->version_ == result.version_ &&
                   result.patch_epoch_ >= std::max(joined_epoch_, minimum_patch_epoch);
        }

    private:
        friend class snapshot_cache;
        lease(std::shared_ptr<state> state, std::shared_ptr<entry> entry)
            : state_(std::move(state)),
              entry_(std::move(entry)),
              joined_epoch_(entry_->patch_epoch_) {}
        std::shared_ptr<state> state_;
        std::shared_ptr<entry> entry_;
        std::uint64_t joined_epoch_;
    };

    snapshot_cache()
        : snapshot_cache(limits{}) {}
    explicit snapshot_cache(limits limits)
        : state_(std::make_shared<state>(limits)) {}
    lease subscribe(snapshot_key key) {
        if (key.tenant_.empty() || key.query_.empty()) {
            throw std::invalid_argument("snapshot subscription requires tenant and query key");
        }
        const std::lock_guard lock(state_->mutex_);
        if (key.tenant_.size() + key.id_.size() + key.query_.size() > state_->limits_.key_bytes_ ||
            state_->subscribers_ >= state_->limits_.subscribers_) {
            throw snapshot_capacity_error();
        }
        auto found = state_->entries_.find(key);
        if (found == state_->entries_.end()) {
            if (state_->entries_.size() >= state_->limits_.groups_) {
                throw snapshot_capacity_error();
            }
            auto entry_value = std::make_shared<entry>();
            entry_value->key_ = key;
            found = state_->entries_.emplace(std::move(key), std::move(entry_value)).first;
        }
        ++state_->subscribers_;
        ++found->second->subscribers_;
        return lease(state_, found->second);
    }

    void invalidate(std::string_view tenant, resource resource, std::string_view id = {},
        bool detail_only = false, bool patch = false) {
        const std::lock_guard lock(state_->mutex_);
        for (const auto& [key, entry] : state_->entries_) {
            if (key.tenant_ != tenant || key.resource_ != resource ||
                (!key.id_.empty() && !id.empty() && key.id_ != id) ||
                (detail_only && key.id_.empty())) {
                continue;
            }
            if (patch) {
                ++entry->patch_epoch_;
            } else {
                ++entry->version_;
            }
            state_->clear(*entry);
        }
    }
    stats get_stats() const {
        const std::lock_guard lock(state_->mutex_);
        return {state_->entries_.size(), state_->subscribers_, state_->flights_, state_->bytes_};
    }

private:
    std::shared_ptr<state> state_;
};

}  // namespace service::live_resource
