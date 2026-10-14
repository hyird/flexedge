#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/http/http_status.h>
#include <ruvia/web/error.h>
#include <ruvia/web/web_worker.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/features/live_resource/snapshot_cache.h"

namespace service::live_resource {

inline constexpr auto heartbeat_interval = std::chrono::seconds(15);
// Access logs can commit several batches per second. Their tenant-wide
// aggregate is recomputed at most once per interval, and only after an event.
inline constexpr auto traffic_refresh_interval = std::chrono::seconds(30);

struct pending_changes final {
    bool snapshot_{};
    std::unordered_map<std::string, std::string> runtime_;
    std::unordered_map<std::string, std::string> origins_;
    // A coalesced change is waiting; the subscriber releases it at this time.
    std::optional<std::chrono::steady_clock::time_point> refresh_at_;
};

// One subscribed projection. A subscription owns one or more parts that share
// a single wakeup channel, so one connection can carry several projections.
struct subscription_part final {
    resource resource_;
    std::string id_;
    std::string query_;
};

// Web workers and background workers are threads of the same server process.
// The mailbox, rather than the channel, owns pending changes: a full wakeup
// channel cannot lose a committed change or the latest runtime value.
class hub final {
    using clock = std::chrono::steady_clock;
    struct mailbox final {
        std::mutex mutex_;
        std::vector<pending_changes> pending_;
        ruvia::channel_sender<std::uint64_t> sender_;
        mailbox(ruvia::channel_sender<std::uint64_t> value, std::size_t parts)
            : pending_(parts),
              sender_(std::move(value)) {}
    };
    struct entry final {
        std::string tenant_;
        resource resource_;
        std::string id_;
        ruvia::worker_id_type worker_;
        std::weak_ptr<mailbox> mailbox_;
        std::size_t part_;
    };
    // Process-wide gate of one coalesced projection: at most one delivery per
    // interval; later events inside the interval defer a single delivery.
    struct coalesced_gate final {
        std::string tenant_;
        resource resource_;
        std::optional<clock::time_point> delivered_at_;
        std::optional<clock::time_point> deferred_until_;
    };
    struct target final {
        std::shared_ptr<mailbox> mailbox_;
        std::size_t part_;
    };
    struct state final {
        std::mutex mutex_;
        std::vector<entry> entries_;
        std::vector<coalesced_gate> gates_;
        snapshot_cache snapshots_;

        std::vector<target> targets(std::string_view tenant, resource resource) const {
            std::vector<target> result;
            for (const auto& entry : entries_) {
                if (entry.tenant_ == tenant && entry.resource_ == resource) {
                    if (auto mailbox = entry.mailbox_.lock()) {
                        result.push_back({std::move(mailbox), entry.part_});
                    }
                }
            }
            return result;
        }
        std::vector<coalesced_gate>::iterator gate(std::string_view tenant, resource resource) {
            return std::find_if(gates_.begin(), gates_.end(), [&](const coalesced_gate& value) {
                return value.tenant_ == tenant && value.resource_ == resource;
            });
        }
    };

public:
    class subscription final {
    public:
        subscription(std::shared_ptr<state> state, std::shared_ptr<mailbox> mailbox,
            ruvia::channel_receiver<std::uint64_t> receiver, std::string tenant,
            std::vector<resource> resources, std::vector<snapshot_cache::lease> snapshots)
            : state_(std::move(state)),
              mailbox_(std::move(mailbox)),
              receiver_(std::move(receiver)),
              tenant_(std::move(tenant)),
              resources_(std::move(resources)),
              snapshots_(std::move(snapshots)) {}
        subscription(const subscription&) = delete;
        subscription& operator=(const subscription&) = delete;
        subscription(subscription&&) = default;
        ~subscription() {
            if (mailbox_) {
                receiver_.close();
                const std::lock_guard lock(state_->mutex_);
                std::erase_if(state_->entries_, [this](const entry& entry) {
                    const auto mailbox = entry.mailbox_.lock();
                    return !mailbox || mailbox == mailbox_;
                });
                // Gates exist only while the projection has subscribers.
                std::erase_if(state_->gates_, [this](const coalesced_gate& gate) {
                    return std::none_of(state_->entries_.begin(), state_->entries_.end(),
                        [&](const entry& entry) {
                            return entry.tenant_ == gate.tenant_ &&
                                   entry.resource_ == gate.resource_;
                        });
                });
            }
        }
        auto receive_for(std::chrono::steady_clock::duration timeout, ruvia::stop_token stop) const {
            return receiver_.receive_for(timeout, std::move(stop));
        }
        std::size_t parts() const {
            return snapshots_.size();
        }
        pending_changes drain(std::size_t part = 0) {
            const std::lock_guard lock(mailbox_->mutex_);
            return std::exchange(mailbox_->pending_.at(part), {});
        }
        const snapshot_cache::lease& snapshots(std::size_t part = 0) const {
            return snapshots_.at(part);
        }
        // Delivers a coalesced change whose deferral has elapsed. Every
        // subscriber of the projection may call this; only the first call
        // after the deadline delivers, and it wakes all of them once.
        bool release_coalesced(std::size_t part, clock::time_point now = clock::now()) {
            return hub::release_coalesced(*state_, tenant_, resources_.at(part), now);
        }

    private:
        std::shared_ptr<state> state_;
        std::shared_ptr<mailbox> mailbox_;
        ruvia::channel_receiver<std::uint64_t> receiver_;
        std::string tenant_;
        std::vector<resource> resources_;
        std::vector<snapshot_cache::lease> snapshots_;
    };

    subscription subscribe(const ruvia::worker_handle& worker, std::string_view tenant,
        std::vector<subscription_part> parts) {
        if (tenant.empty()) {
            throw std::invalid_argument("live resource subscription requires tenant");
        }
        if (parts.empty()) {
            throw std::invalid_argument("live resource subscription requires a part");
        }
        auto [sender, receiver] = ruvia::make_channel<std::uint64_t>(
            worker, {.capacity_ = 1, .resource_ = std::pmr::new_delete_resource()});
        auto mailbox_value = std::make_shared<mailbox>(std::move(sender), parts.size());
        std::vector<snapshot_cache::lease> snapshots;
        std::vector<resource> resources;
        snapshots.reserve(parts.size());
        resources.reserve(parts.size());
        bool deferred = false;
        const std::lock_guard lock(state_->mutex_);
        try {
            for (auto& part : parts) {
                snapshots.push_back(state_->snapshots_.subscribe(
                    {std::string(tenant), part.resource_, part.id_, std::move(part.query_)}));
            }
        } catch (const snapshot_capacity_error&) {
            throw ruvia::http_error({.status_ = ruvia::http_status::service_unavailable,
                .code_ = "10004",
                .message_ = "资源订阅容量已满，请稍后重试"});
        }
        for (std::size_t index = 0; index < parts.size(); ++index) {
            state_->entries_.push_back({std::string(tenant), parts[index].resource_,
                std::move(parts[index].id_), worker.id(), mailbox_value, index});
            resources.push_back(parts[index].resource_);
            // A late subscriber must also release a change deferred before it
            // joined: its first read may reuse the snapshot cached before it.
            const auto gate = state_->gate(tenant, parts[index].resource_);
            if (gate != state_->gates_.end() && gate->deferred_until_) {
                mailbox_value->pending_[index].refresh_at_ = gate->deferred_until_;
                deferred = true;
            }
        }
        if (deferred) {
            (void)mailbox_value->sender_.send(1);
        }
        return subscription(state_, std::move(mailbox_value), std::move(receiver), std::string(tenant),
            std::move(resources), std::move(snapshots));
    }
    subscription subscribe(const ruvia::worker_handle& worker, std::string_view tenant,
        resource resource, std::string_view id = {},
        std::string query = query_key("default")) {
        std::vector<subscription_part> parts;
        parts.push_back({resource, std::string(id), std::move(query)});
        return subscribe(worker, tenant, std::move(parts));
    }
    snapshot_cache::stats cache_stats() const {
        return state_->snapshots_.get_stats();
    }

    void publish(std::string_view tenant, resource resource, std::string_view id = {}) {
        publish_impl(std::nullopt, tenant, resource, id);
    }
    void publish_for_worker(ruvia::worker_id_type worker, std::string_view tenant,
        resource resource, std::string_view id = {}) {
        publish_impl(worker, tenant, resource, id);
    }
    void publish_runtime(std::string_view tenant, std::string_view id, std::string_view payload) {
        deliver(*state_, tenant, resource::nodes, id, payload, true);
    }
    void publish_origins(std::string_view tenant, std::string_view website_id,
        std::string_view node_id, std::string_view payload) {
        deliver(*state_, tenant, resource::websites, website_id, payload, false, node_id);
    }
    // High-rate change of a tenant-wide projection. Without subscribers this
    // is a no-op, so nothing is ever read for an unobserved projection. The
    // first change after a quiet interval is delivered at once; later changes
    // inside the interval schedule one deferred delivery, which subscribers
    // release at the deadline. Every web worker reads every notification;
    // the process-wide gate turns those duplicates into one delivery.
    void publish_coalesced(std::string_view tenant, resource resource, clock::duration interval,
        clock::time_point now = clock::now()) {
        std::vector<target> deferred;
        clock::time_point refresh_at;
        {
            const std::lock_guard lock(state_->mutex_);
            auto targets = state_->targets(tenant, resource);
            auto gate = state_->gate(tenant, resource);
            if (targets.empty()) {
                if (gate != state_->gates_.end()) {
                    state_->gates_.erase(gate);
                }
                return;
            }
            if (gate == state_->gates_.end()) {
                gate = state_->gates_.insert(state_->gates_.end(),
                    {std::string(tenant), resource, std::nullopt, std::nullopt});
            }
            if (gate->deferred_until_) {
                return;
            }
            if (gate->delivered_at_ && now < *gate->delivered_at_ + interval) {
                refresh_at = *gate->delivered_at_ + interval;
                gate->deferred_until_ = refresh_at;
                deferred = std::move(targets);
            } else {
                gate->delivered_at_ = now;
            }
        }
        if (deferred.empty()) {
            deliver(*state_, tenant, resource, {}, {}, false);
            return;
        }
        for (const auto& target : deferred) {
            {
                const std::lock_guard lock(target.mailbox_->mutex_);
                auto& pending = target.mailbox_->pending_[target.part_];
                if (!pending.refresh_at_ || refresh_at < *pending.refresh_at_) {
                    pending.refresh_at_ = refresh_at;
                }
            }
            (void)target.mailbox_->sender_.send(1);
        }
    }

private:
    static bool release_coalesced(state& state, std::string_view tenant, resource resource,
        clock::time_point now) {
        {
            const std::lock_guard lock(state.mutex_);
            const auto gate = state.gate(tenant, resource);
            if (gate == state.gates_.end() || !gate->deferred_until_ || now < *gate->deferred_until_) {
                return false;
            }
            gate->delivered_at_ = now;
            gate->deferred_until_.reset();
        }
        deliver(state, tenant, resource, {}, {}, false);
        return true;
    }

    void publish_impl(std::optional<ruvia::worker_id_type> worker, std::string_view tenant,
        resource resource_value, std::string_view id) {
        deliver(*state_, tenant, resource_value, id, {}, false, {}, worker);
        // The overview projects resource totals and task marker state. Runtime
        // reports bypass publish(), so ordinary heartbeats never reload it.
        if (resource_value != resource::overview && resource_value != resource::access_history &&
            resource_value != resource::overview_traffic) {
            deliver(*state_, tenant, resource::overview, {}, {}, false, {}, worker);
        }
        // Read projections join these resources. Dependent projections receive
        // a tenant-scoped wakeup because their IDs differ from the changed row.
        // Deliver directly rather than recursively publishing: reciprocal joins
        // (zone counts, website bindings) must not form notification loops.
        const auto dependent = [&](resource projection) {
            deliver(*state_, tenant, projection, {}, {}, false, {}, worker);
        };
        switch (resource_value) {
            case resource::nodes:
                dependent(resource::clusters);
                dependent(resource::websites);
                break;
            case resource::clusters:
                dependent(resource::nodes);
                dependent(resource::websites);
                break;
            case resource::dns_zones:
                dependent(resource::providers);
                dependent(resource::clusters);
                dependent(resource::websites);
                dependent(resource::certificates);
                break;
            case resource::providers:
                dependent(resource::dns_zones);
                dependent(resource::clusters);
                dependent(resource::websites);
                break;
            case resource::certificates:
                dependent(resource::websites);
                break;
            case resource::websites:
                dependent(resource::dns_zones);
                dependent(resource::certificates);
                // Website names, deletion and logging switches shape the traffic
                // aggregate; it shares the access-log refresh gate.
                publish_coalesced(tenant, resource::overview_traffic, traffic_refresh_interval);
                break;
            case resource::tasks:
            case resource::overview:
            case resource::access_history:
            case resource::overview_traffic:
                break;
        }
    }
    static void deliver(state& state, std::string_view tenant, resource resource,
        std::string_view id, std::string_view payload, bool runtime,
        std::string_view origin_node = {},
        std::optional<ruvia::worker_id_type> worker = std::nullopt) {
        // Invalidate once per query group BEFORE waking any subscriber. Runtime
        // patches also invalidate cached rows for future joins, but do not turn
        // ordinary heartbeats into snapshot reads for existing subscribers.
        state.snapshots_.invalidate(tenant, resource, id, !origin_node.empty(),
            runtime || !origin_node.empty());
        std::vector<target> targets;
        {
            const std::lock_guard lock(state.mutex_);
            for (const auto& entry : state.entries_) {
                if (entry.tenant_ != tenant || entry.resource_ != resource ||
                    (worker && entry.worker_ != *worker) ||
                    (!entry.id_.empty() && !id.empty() && entry.id_ != id)) {
                    continue;
                }
                // Origin health is a detail-only projection; list rows do not
                // contain per-origin diagnostics and must not wake on reports.
                if (!origin_node.empty() && entry.id_.empty()) {
                    continue;
                }
                if (auto mailbox = entry.mailbox_.lock()) {
                    targets.push_back({std::move(mailbox), entry.part_});
                }
            }
        }
        for (const auto& target : targets) {
            {
                const std::lock_guard lock(target.mailbox_->mutex_);
                auto& pending = target.mailbox_->pending_[target.part_];
                if (!origin_node.empty()) {
                    pending.origins_[std::string(origin_node)] = std::string(payload);
                } else if (runtime) {
                    pending.runtime_[std::string(id)] = std::string(payload);
                } else {
                    pending.snapshot_ = true;
                }
            }
            (void)target.mailbox_->sender_.send(1);
        }
    }
    std::shared_ptr<state> state_{std::make_shared<state>()};
};

inline hub& get_hub() {
    static hub value;
    return value;
}

}  // namespace service::live_resource
