#pragma once

#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/http/http_status.h>
#include <ruvia/web/error.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/features/background/keyed_signal_hub.h"
#include "service/features/live_resource/snapshot_reads.h"

namespace service::live_resource {

// Access logs can commit several batches per second. Their tenant-wide
// aggregate is recomputed at most once per interval, and only after an event.
inline constexpr auto traffic_refresh_interval = std::chrono::seconds(30);

// One attempt recorded for a task (a resource's task, keyed by its resource
// id). Only the identifier travels; the subscriber reads the attempt itself.
struct task_attempt_notice final {
    std::int64_t event_id_{};
};
// Notices a mailbox part keeps before it falls back to one recovery snapshot.
inline constexpr std::size_t max_pending_task_attempts{32};

struct pending_changes final {
    bool snapshot_{};
    std::unordered_map<std::string, std::string> runtime_;
    std::unordered_map<std::string, std::string> origins_;
    // Attempts recorded since the last drain, in delivery order; only parts
    // subscribed with `task_attempts_` receive them.
    std::vector<task_attempt_notice> task_attempts_;
    // The retained history changed as a whole (retention, notice overflow,
    // missed notifications): an incremental part resends its snapshot once.
    bool history_reset_{};
    // Notifications may have been missed: the part writes its next snapshot
    // even when its bytes equal the last one written.
    bool recovery_{};
    // A coalesced change is waiting; the subscriber releases it at this time.
    std::optional<std::chrono::steady_clock::time_point> refresh_at_;
};

// One subscribed projection. A subscription owns one or more parts that share
// a single wakeup channel, so one connection can carry several projections.
struct subscription_part final {
    resource resource_;
    std::string id_;
    std::string query_;
    // Receives task attempt notices of exactly this task id.
    bool task_attempts_{};
};

// Worker-local fan-out: every Web Worker owns one hub (Ruvia worker state)
// and delivers only to the subscriptions of the connections it holds.
// Changes reach it from the worker's own resource notification reader, never
// from another worker. The mailbox, rather than the channel, owns pending
// changes: a full wakeup channel cannot lose a change or the latest runtime
// value.
class hub final {
    using clock = std::chrono::steady_clock;
    // The projection a part follows, owned or borrowed for a lookup.
    struct projection_key final {
        std::string tenant_;
        resource resource_;
        friend bool operator==(const projection_key&, const projection_key&) = default;
    };
    struct projection_view final {
        std::string_view tenant_;
        resource resource_;
        friend bool operator==(const projection_key& left, const projection_view& right) {
            return left.tenant_ == right.tenant_ && left.resource_ == right.resource_;
        }
    };
    struct projection_hash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(const projection_view& value) const noexcept {
            return std::hash<std::string_view>{}(value.tenant_) ^
                   (std::hash<int>{}(static_cast<int>(value.resource_)) << 1U);
        }
        [[nodiscard]] std::size_t operator()(const projection_key& value) const noexcept {
            return (*this)(projection_view{value.tenant_, value.resource_});
        }
    };
    // One registered part: its id filter and the mailbox slot owning its
    // pending changes.
    struct part_registration final {
        std::string id_;
        bool task_attempts_{};
        pending_changes* pending_{};
    };
    using signals = service::background::keyed_signal_hub<projection_key, part_registration, projection_hash>;
    // Gate of one coalesced projection: at most one delivery per interval;
    // later events inside the interval defer a single delivery.
    struct coalesced_gate final {
        std::string tenant_;
        resource resource_;
        std::optional<clock::time_point> delivered_at_;
        std::optional<clock::time_point> deferred_until_;
    };
    struct state final {
        signals signals_;
        std::vector<coalesced_gate> gates_;
        snapshot_reads snapshots_;

        std::vector<coalesced_gate>::iterator gate(std::string_view tenant, resource resource) {
            return std::find_if(gates_.begin(), gates_.end(), [&](const coalesced_gate& value) {
                return value.tenant_ == tenant && value.resource_ == resource;
            });
        }
    };

public:
    class subscription final {
    public:
        subscription(std::shared_ptr<state> state, std::unique_ptr<std::vector<pending_changes>> pending,
            signals::subscription signal, std::string tenant, std::vector<resource> resources,
            std::vector<snapshot_reads::lease> snapshots)
            : state_(std::move(state)),
              pending_(std::move(pending)),
              signal_(std::move(signal)),
              tenant_(std::move(tenant)),
              resources_(std::move(resources)),
              snapshots_(std::move(snapshots)) {}
        subscription(const subscription&) = delete;
        subscription& operator=(const subscription&) = delete;
        subscription(subscription&&) = default;
        ~subscription() {
            if (!state_) {
                return;
            }
            signal_.close();
            // Gates exist only while the projection has subscribers.
            std::erase_if(state_->gates_, [this](const coalesced_gate& gate) {
                return !state_->signals_.contains(projection_view{gate.tenant_, gate.resource_});
            });
        }
        auto receive_for(std::chrono::steady_clock::duration timeout, ruvia::stop_token stop) const {
            return signal_.receive_for(timeout, std::move(stop));
        }
        std::size_t parts() const {
            return snapshots_.size();
        }
        pending_changes drain(std::size_t part = 0) {
            return std::exchange(pending_->at(part), {});
        }
        const snapshot_reads::lease& snapshots(std::size_t part = 0) const {
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
        std::unique_ptr<std::vector<pending_changes>> pending_;
        signals::subscription signal_;
        std::string tenant_;
        std::vector<resource> resources_;
        std::vector<snapshot_reads::lease> snapshots_;
    };

    subscription subscribe(const ruvia::worker_handle& worker, std::string_view tenant,
        std::vector<subscription_part> parts) {
        if (tenant.empty()) {
            throw std::invalid_argument("live resource subscription requires tenant");
        }
        if (parts.empty()) {
            throw std::invalid_argument("live resource subscription requires a part");
        }
        auto pending = std::make_unique<std::vector<pending_changes>>(parts.size());
        std::vector<snapshot_reads::lease> snapshots;
        std::vector<resource> resources;
        snapshots.reserve(parts.size());
        resources.reserve(parts.size());
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
        std::vector<std::pair<projection_key, part_registration>> registrations;
        registrations.reserve(parts.size());
        std::vector<std::size_t> deferred;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            registrations.push_back({projection_key{std::string(tenant), parts[index].resource_},
                part_registration{std::move(parts[index].id_), parts[index].task_attempts_, &(*pending)[index]}});
            resources.push_back(parts[index].resource_);
            // A late subscriber must also release a change deferred before it
            // joined: its first read may reuse the snapshot cached before it.
            const auto gate = state_->gate(tenant, parts[index].resource_);
            if (gate != state_->gates_.end() && gate->deferred_until_) {
                (*pending)[index].refresh_at_ = gate->deferred_until_;
                deferred.push_back(index);
            }
        }
        auto signal = state_->signals_.subscribe(worker, std::move(registrations));
        for (const auto index : deferred) {
            const auto* slot = &(*pending)[index];
            state_->signals_.publish(projection_view{tenant, resources[index]},
                [slot](part_registration& part) { return part.pending_ == slot; });
        }
        return subscription(state_, std::move(pending), std::move(signal), std::string(tenant),
            std::move(resources), std::move(snapshots));
    }
    subscription subscribe(const ruvia::worker_handle& worker, std::string_view tenant,
        resource resource, std::string_view id = {},
        std::string query = query_key("default")) {
        std::vector<subscription_part> parts;
        parts.push_back({resource, std::string(id), std::move(query)});
        return subscribe(worker, tenant, std::move(parts));
    }
    snapshot_reads::stats read_stats() const {
        return state_->snapshots_.get_stats();
    }

    void publish(std::string_view tenant, resource resource_value, std::string_view id = {}) {
        deliver(*state_, tenant, resource_value, id, {}, false);
        // The overview projects resource totals. Task state is not part of
        // it, and runtime reports bypass publish(), so neither task changes
        // nor ordinary heartbeats reload it.
        if (resource_value != resource::overview && resource_value != resource::access_history &&
            resource_value != resource::overview_traffic && resource_value != resource::tasks) {
            deliver(*state_, tenant, resource::overview, {}, {}, false);
        }
        // Read projections join these resources. Dependent projections receive
        // a tenant-scoped wakeup because their IDs differ from the changed row.
        // Deliver directly rather than recursively publishing: reciprocal joins
        // (zone counts, website bindings) must not form notification loops.
        const auto dependent = [&](resource projection) {
            deliver(*state_, tenant, projection, {}, {}, false);
        };
        switch (resource_value) {
            case resource::nodes:
                dependent(resource::clusters);
                dependent(resource::websites);
                // Task views show whether nodes with pending work are online.
                dependent(resource::tasks);
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
    void publish_runtime(std::string_view tenant, std::string_view id, std::string_view payload) {
        deliver(*state_, tenant, resource::nodes, id, payload, true);
    }
    void publish_origins(std::string_view tenant, std::string_view website_id,
        std::string_view node_id, std::string_view payload) {
        deliver(*state_, tenant, resource::websites, website_id, payload, false, node_id);
    }
    // A recorded attempt of task `task_id`. It wakes task projections as
    // publish() does; parts subscribed with `task_attempts_` for exactly this
    // task also receive the notice, so they read only that attempt instead of
    // re-reading their snapshot.
    void publish_task_attempt(std::string_view tenant, std::string_view task_id,
        const task_attempt_notice& notice) {
        queue_task_history(tenant, task_id, &notice);
        publish(tenant, resource::tasks, task_id);
    }
    // The retained attempts of the task changed as a whole (retention):
    // incremental history parts resend one snapshot.
    void reset_task_history(std::string_view tenant, std::string_view task_id) {
        queue_task_history(tenant, task_id, nullptr);
        publish(tenant, resource::tasks, task_id);
    }
    // Notifications may have been missed (the reader lost its stream, or a
    // producer could not send): every subscription of the tenant, or of all
    // tenants without one, reads one recovery snapshot. Byte-identical
    // snapshot parts still write nothing; incremental parts always resend.
    void recover(std::optional<std::string_view> tenant = std::nullopt) {
        // Registrations of one projection are visited together, so each
        // projection is invalidated once, before any of its parts wakes.
        const projection_key* invalidated{};
        state_->signals_.publish_if([&](const projection_key& key, part_registration& part) {
            if (tenant && key.tenant_ != *tenant) {
                return false;
            }
            if (invalidated != &key) {
                state_->snapshots_.invalidate(key.tenant_, key.resource_);
                invalidated = &key;
            }
            auto& pending = *part.pending_;
            pending.snapshot_ = true;
            pending.recovery_ = true;
            if (part.task_attempts_) {
                pending.task_attempts_.clear();
                pending.history_reset_ = true;
            }
            return true;
        });
    }
    // High-rate change of a tenant-wide projection. Without subscribers this
    // is a no-op, so nothing is ever read for an unobserved projection. The
    // first change after a quiet interval is delivered at once; later changes
    // inside the interval schedule one deferred delivery, which subscribers
    // release at the deadline.
    void publish_coalesced(std::string_view tenant, resource resource, clock::duration interval,
        clock::time_point now = clock::now()) {
        const projection_view projection{tenant, resource};
        auto gate = state_->gate(tenant, resource);
        if (!state_->signals_.contains(projection)) {
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
        if (!gate->delivered_at_ || now >= *gate->delivered_at_ + interval) {
            gate->delivered_at_ = now;
            deliver(*state_, tenant, resource, {}, {}, false);
            return;
        }
        const auto refresh_at = *gate->delivered_at_ + interval;
        gate->deferred_until_ = refresh_at;
        state_->signals_.publish(projection, [refresh_at](part_registration& part) {
            auto& pending = *part.pending_;
            if (!pending.refresh_at_ || refresh_at < *pending.refresh_at_) {
                pending.refresh_at_ = refresh_at;
            }
            return true;
        });
    }

private:
    static bool release_coalesced(state& state, std::string_view tenant, resource resource,
        clock::time_point now) {
        const auto gate = state.gate(tenant, resource);
        if (gate == state.gates_.end() || !gate->deferred_until_ || now < *gate->deferred_until_) {
            return false;
        }
        gate->delivered_at_ = now;
        gate->deferred_until_.reset();
        deliver(state, tenant, resource, {}, {}, false);
        return true;
    }
    // Queues the notice (or, without one, a history reset) for incremental
    // parts of exactly this task; the caller's publish wakes them. A part
    // that falls behind by more than max_pending_task_attempts drops its
    // notices for one recovery snapshot, so no mailbox grows unbounded.
    void queue_task_history(std::string_view tenant, std::string_view task_id,
        const task_attempt_notice* notice) {
        state_->signals_.publish(projection_view{tenant, resource::tasks}, [&](part_registration& part) {
            if (!part.task_attempts_ || part.id_ != task_id) {
                return false;
            }
            auto& pending = *part.pending_;
            if (pending.history_reset_) {
                return false;
            }
            if (notice == nullptr || pending.task_attempts_.size() >= max_pending_task_attempts) {
                pending.task_attempts_.clear();
                pending.history_reset_ = true;
            } else {
                pending.task_attempts_.push_back(*notice);
            }
            return false;
        });
    }
    static void deliver(state& state, std::string_view tenant, resource resource,
        std::string_view id, std::string_view payload, bool runtime,
        std::string_view origin_node = {}) {
        // Invalidate once per query group BEFORE waking any subscriber. Runtime
        // patches also invalidate cached rows for future joins, but do not turn
        // ordinary heartbeats into snapshot reads for existing subscribers.
        state.snapshots_.invalidate(tenant, resource, id, !origin_node.empty(),
            runtime || !origin_node.empty());
        state.signals_.publish(projection_view{tenant, resource}, [&](part_registration& part) {
            if (!part.id_.empty() && !id.empty() && part.id_ != id) {
                return false;
            }
            // Origin health is a detail-only projection; list rows do not
            // contain per-origin diagnostics and must not wake on reports.
            if (!origin_node.empty() && part.id_.empty()) {
                return false;
            }
            auto& pending = *part.pending_;
            if (!origin_node.empty()) {
                pending.origins_[std::string(origin_node)] = std::string(payload);
            } else if (runtime) {
                pending.runtime_[std::string(id)] = std::string(payload);
            } else {
                pending.snapshot_ = true;
            }
            return true;
        });
    }
    std::shared_ptr<state> state_{std::make_shared<state>()};
};

// The calling worker's hub. Request contexts and posted worker jobs both
// borrow their own worker's instance (registered with use_worker_state).
template <typename context_type>
hub& get_hub(context_type& context) {
    return context.template worker_state<hub>();
}

}  // namespace service::live_resource
