#pragma once

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <ruvia/core/Channel.h>
#include <ruvia/core/Task.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/Error.h>
#include "service/features/live_resource/snapshot_cache.h"

namespace service::live_resource {

inline constexpr auto kHeartbeatInterval = std::chrono::seconds(15);

struct PendingChanges final {
    bool snapshot{};
    std::unordered_map<std::string, std::string> runtime;
    std::unordered_map<std::string, std::string> origins;
};

// Web workers and background workers are threads of the same server process.
// The mailbox, rather than the channel, owns pending changes: a full wakeup
// channel cannot lose a committed change or the latest runtime value.
class Hub final {
    struct Mailbox final {
        std::mutex mutex;
        PendingChanges pending;
        ruvia::ChannelSender<std::uint64_t> sender;
        explicit Mailbox(ruvia::ChannelSender<std::uint64_t> value) : sender(std::move(value)) {}
    };
    struct Entry final {
        std::string tenant;
        Resource resource;
        std::string id;
        std::weak_ptr<Mailbox> mailbox;
    };
    struct State final {
        std::mutex mutex;
        std::vector<Entry> entries;
        SnapshotCache snapshots;
    };

  public:
    class Subscription final {
      public:
        Subscription(std::shared_ptr<State> state, std::shared_ptr<Mailbox> mailbox,
                     ruvia::ChannelReceiver<std::uint64_t> receiver, SnapshotCache::Lease snapshots)
            : state_(std::move(state)), mailbox_(std::move(mailbox)),
              receiver_(std::move(receiver)), snapshots_(std::move(snapshots)) {}
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&&) = default;
        ~Subscription() {
            if (mailbox_) {
                receiver_.close();
                const std::lock_guard lock(state_->mutex);
                std::erase_if(state_->entries, [this](const Entry& entry) {
                    const auto mailbox = entry.mailbox.lock();
                    return !mailbox || mailbox == mailbox_;
                });
            }
        }
        auto receiveFor(std::chrono::steady_clock::duration timeout, ruvia::StopToken stop) const {
            return receiver_.receiveFor(timeout, std::move(stop));
        }
        PendingChanges drain() {
            const std::lock_guard lock(mailbox_->mutex);
            return std::exchange(mailbox_->pending, {});
        }
        const SnapshotCache::Lease& snapshots() const { return snapshots_; }

      private:
        std::shared_ptr<State> state_;
        std::shared_ptr<Mailbox> mailbox_;
        ruvia::ChannelReceiver<std::uint64_t> receiver_;
        SnapshotCache::Lease snapshots_;
    };

    Subscription subscribe(const ruvia::WorkerHandle& worker, std::string_view tenant,
                           Resource resource, std::string_view id = {},
                           std::string query = queryKey("default")) {
        if (tenant.empty())
            throw std::invalid_argument("live resource subscription requires tenant");
        auto [sender, receiver] = ruvia::makeChannel<std::uint64_t>(
            worker, {.capacity = 1, .resource = std::pmr::new_delete_resource()});
        auto mailbox = std::make_shared<Mailbox>(std::move(sender));
        const std::lock_guard lock(state_->mutex);
        try {
            auto snapshots = state_->snapshots.subscribe(
                {std::string(tenant), resource, std::string(id), std::move(query)});
            state_->entries.push_back({std::string(tenant), resource, std::string(id), mailbox});
            return Subscription(state_, std::move(mailbox), std::move(receiver), std::move(snapshots));
        } catch (const SnapshotCapacityError&) {
            throw ruvia::HttpError({.status = ruvia::http_status::kServiceUnavailable,
                                    .code = "10004", .message = "资源订阅容量已满，请稍后重试"});
        }
    }
    SnapshotCache::Stats cacheStats() const { return state_->snapshots.stats(); }

    void publish(std::string_view tenant, Resource resource, std::string_view id = {}) {
        deliver(tenant, resource, id, {}, false);
        // The overview projects resource totals and task marker state. Runtime
        // reports bypass publish(), so ordinary heartbeats never reload it.
        if (resource != Resource::overview && resource != Resource::accessHistory)
            deliver(tenant, Resource::overview, {}, {}, false);
        // Read projections join these resources. Dependent projections receive
        // a tenant-scoped wakeup because their IDs differ from the changed row.
        // Deliver directly rather than recursively publishing: reciprocal joins
        // (zone counts, website bindings) must not form notification loops.
        const auto dependent = [&](Resource projection) {
            deliver(tenant, projection, {}, {}, false);
        };
        switch (resource) {
        case Resource::nodes:
            dependent(Resource::clusters);
            dependent(Resource::websites);
            break;
        case Resource::clusters:
            dependent(Resource::nodes);
            dependent(Resource::websites);
            break;
        case Resource::dnsZones:
            dependent(Resource::providers);
            dependent(Resource::clusters);
            dependent(Resource::websites);
            dependent(Resource::certificates);
            break;
        case Resource::providers:
            dependent(Resource::dnsZones);
            dependent(Resource::clusters);
            dependent(Resource::websites);
            break;
        case Resource::certificates:
            dependent(Resource::websites);
            break;
        case Resource::websites:
            dependent(Resource::dnsZones);
            dependent(Resource::certificates);
            break;
        case Resource::tasks:
        case Resource::overview:
        case Resource::accessHistory:
            break;
        }
    }
    void publishRuntime(std::string_view tenant, std::string_view id, std::string_view payload) {
        deliver(tenant, Resource::nodes, id, payload, true);
    }
    void publishOrigins(std::string_view tenant, std::string_view websiteId,
                        std::string_view nodeId, std::string_view payload) {
        deliver(tenant, Resource::websites, websiteId, payload, false, nodeId);
    }

  private:
    void deliver(std::string_view tenant, Resource resource, std::string_view id,
                 std::string_view payload, bool runtime, std::string_view originNode = {}) {
        // Invalidate once per query group BEFORE waking any subscriber. Runtime
        // patches also invalidate cached rows for future joins, but do not turn
        // ordinary heartbeats into snapshot reads for existing subscribers.
        state_->snapshots.invalidate(tenant, resource, id, !originNode.empty(),
                                     runtime || !originNode.empty());
        std::vector<std::shared_ptr<Mailbox>> targets;
        {
            const std::lock_guard lock(state_->mutex);
            for (const auto& entry : state_->entries) {
                if (entry.tenant != tenant || entry.resource != resource ||
                    (!entry.id.empty() && !id.empty() && entry.id != id))
                    continue;
                // Origin health is a detail-only projection; list rows do not
                // contain per-origin diagnostics and must not wake on reports.
                if (!originNode.empty() && entry.id.empty())
                    continue;
                if (auto mailbox = entry.mailbox.lock())
                    targets.push_back(std::move(mailbox));
            }
        }
        for (const auto& mailbox : targets) {
            {
                const std::lock_guard lock(mailbox->mutex);
                if (!originNode.empty())
                    mailbox->pending.origins[std::string(originNode)] = std::string(payload);
                else if (runtime)
                    mailbox->pending.runtime[std::string(id)] = std::string(payload);
                else
                    mailbox->pending.snapshot = true;
            }
            (void)mailbox->sender.send(1);
        }
    }
    std::shared_ptr<State> state_{std::make_shared<State>()};
};

inline Hub& hub() {
    static Hub value;
    return value;
}

} // namespace service::live_resource
