#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/worker_handle.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "service/features/live_resource/query_key.h"

namespace service::live_resource {

// A flight distributes owned, allocator-independent bytes only to its
// current readers. Request DTOs and HttpError objects never enter this state.
struct snapshot final {
    std::string data_;
    bool failure_{};
};

// The last snapshot bytes one stream part wrote, kept as size + SHA-256 so
// equal bytes are never written twice however often the stream re-reads its
// snapshot, without pinning the bytes themselves.
class written_snapshot final {
public:
    // Records `bytes`; true when they must be written (nothing recorded yet,
    // different bytes, or a digest that could not be computed).
    bool replace(std::string_view bytes) {
        const auto value = fingerprint_of(bytes);
        const bool changed = !value || !last_ || *last_ != *value;
        last_ = value;
        return changed;
    }
    // The next bytes are written whatever they are (initial snapshot,
    // recovery after missed notifications, after an error frame).
    void forget() noexcept {
        last_.reset();
    }

private:
    struct fingerprint final {
        std::size_t size_{};
        std::array<unsigned char, 32> sha256_{};
        friend bool operator==(const fingerprint&, const fingerprint&) = default;
    };
    static std::optional<fingerprint> fingerprint_of(std::string_view bytes) {
        fingerprint value{.size_ = bytes.size(), .sha256_ = {}};
        unsigned int length = 0;
        if (EVP_Digest(bytes.data(), bytes.size(), value.sha256_.data(), &length, EVP_sha256(), nullptr) != 1 ||
            length != value.sha256_.size()) {
            return std::nullopt;
        }
        return value;
    }
    std::optional<fingerprint> last_;
};

class snapshot_capacity_error final : public std::runtime_error {
public:
    snapshot_capacity_error()
        : std::runtime_error("live resource read capacity exceeded") {}
};

// One Web Worker's connection-owned read coordination. Completed bytes live
// only in readers' channels; this object never retains a reusable snapshot.
class snapshot_reads final {
public:
    using clock = std::chrono::steady_clock;
    struct limits final {
        std::size_t groups_{1024};
        std::size_t subscribers_{8192};
        std::size_t flights_{128};
        std::size_t key_bytes_{4096};
    };
    struct stats final {
        std::size_t groups_{}, subscribers_{}, flights_{};
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
        std::vector<std::weak_ptr<waiter>> waiters_;
    };
    struct state final {
        explicit state(limits value)
            : limits_(value) {}
        limits limits_;
        std::unordered_map<snapshot_key, std::shared_ptr<entry>, snapshot_key_hash> entries_;
        std::size_t subscribers_{}, flights_{};
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
        friend class snapshot_reads;
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
                if (!entry_->loading_ || entry_->flight_generation_ != flight_generation_) {
                    return;
                }
                entry_->loading_ = false;
                --state_->flights_;
                waiters.swap(entry_->waiters_);
                // A business commit invalidated this read, or everyone
                // disconnected. Never distribute its old result.
                if (snapshot && entry_->version_ == version_ && entry_->subscribers_ != 0) {
                    result = {std::move(snapshot), version_, patch_epoch_};
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
        [[nodiscard]] const snapshot_key& key() const noexcept {
            return entry_->key_;
        }
        ~lease() {
            if (!entry_) {
                return;
            }
            --state_->subscribers_;
            if (--entry_->subscribers_ == 0) {
                state_->entries_.erase(entry_->key_);
                entry_->waiters_.clear();
            }
        }

        service::live_resource::snapshot_reads::read read(const ruvia::worker_handle& worker) const {
            auto [sender, receiver] = ruvia::make_channel<result>(
                worker, {.capacity_ = 1, .resource_ = std::pmr::new_delete_resource()});
            service::live_resource::snapshot_reads::read result{{}, {}, std::move(receiver)};
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
            return entry_->version_ == version;
        }
        std::uint64_t patch_epoch() const {
            return entry_->patch_epoch_;
        }
        bool current(const result& result, std::uint64_t minimum_patch_epoch = 0) const {
            // A subscriber can replay all patches delivered since it joined.
            // A late join cannot use a query begun before that boundary: some
            // intervening patches were never in its mailbox. It joins the next
            // read once, without starving on subsequent high-rate heartbeats.
            return entry_->version_ == result.version_ &&
                   result.patch_epoch_ >= std::max(joined_epoch_, minimum_patch_epoch);
        }

    private:
        friend class snapshot_reads;
        lease(std::shared_ptr<state> state, std::shared_ptr<entry> entry)
            : state_(std::move(state)),
              entry_(std::move(entry)),
              joined_epoch_(entry_->patch_epoch_) {}
        std::shared_ptr<state> state_;
        std::shared_ptr<entry> entry_;
        std::uint64_t joined_epoch_;
    };

    snapshot_reads()
        : snapshot_reads(limits{}) {}
    explicit snapshot_reads(limits limits)
        : state_(std::make_shared<state>(limits)) {}
    lease subscribe(snapshot_key key) {
        if (key.tenant_.empty() || key.query_.empty()) {
            throw std::invalid_argument("snapshot subscription requires tenant and query key");
        }
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
        }
    }
    stats get_stats() const {
        return {state_->entries_.size(), state_->subscribers_, state_->flights_};
    }

private:
    std::shared_ptr<state> state_;
};

}  // namespace service::live_resource
