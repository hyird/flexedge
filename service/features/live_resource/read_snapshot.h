#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <ruvia/web/App.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include "service/common/http.h"
#include "service/features/live_resource/snapshot_cache.h"
#include "service/features/logging/logger.h"

namespace service::live_resource {

inline Snapshot snapshotError(const ruvia::HttpErrorInfo& info) {
    service::common::ErrorResponse response(
        ruvia::ModelOptions{.resource = std::pmr::new_delete_resource()});
    response.set<"code">(service::common::normalizeBusinessErrorCode(info.code(), info.status().value()));
    response.set<"message">(service::common::responseErrorMessage(info));
    return {std::string(ruvia::toJson(response, {.resource = std::pmr::new_delete_resource()})), true};
}

inline Snapshot snapshotUnavailable() {
    return {R"({"code":10004,"message":"资源读取暂时不可用，请稍后重试"})", true};
}

template <typename Fetch>
ruvia::Task<std::shared_ptr<const Snapshot>> readSnapshot(
    ruvia::Context& c, const SnapshotCache::Lease& lease, Fetch fetch) {
    const auto deadline = SnapshotCache::Clock::now() + std::chrono::seconds(30);
    // A returning reader may already have emitted runtime patches. Its next
    // baseline must include those bytes in the DB read, rather than undo them.
    // Keep this boundary fixed across retries so newer heartbeats cannot starve
    // the read; patches after it remain queued for replay after the baseline.
    const auto minimumPatchEpoch = lease.patchEpoch();
    while (!c.stopToken().stopRequested()) {
        auto read = lease.read(c.worker());
        if (read.ready.snapshot && lease.current(read.ready, minimumPatchEpoch))
            co_return read.ready.snapshot;
        if (read.ticket) {
            bool posted{};
            try {
                for (const auto& worker : ruvia::app().workers()) {
                    if (worker.id() != c.worker().id())
                        continue;
                    // The query outlives any one subscriber. All inputs are
                    // owned, and its DTOs are reclaimed on this owning worker.
                    posted = worker.post([ticket = *read.ticket, fetch](ruvia::WebWorkerContext& context)
                                             -> ruvia::Task<void> {
                        Snapshot snapshot;
                        try {
                            snapshot.data = co_await fetch(context);
                        } catch (const ruvia::HttpError& error) {
                            snapshot = snapshotError(error.info());
                        } catch (const std::exception& error) {
                            service::logging::error("Live resource read failure: " + std::string(error.what()));
                            snapshot = snapshotUnavailable();
                        } catch (...) {
                            snapshot = snapshotUnavailable();
                        }
                        ticket.complete(std::make_shared<const Snapshot>(std::move(snapshot)));
                        co_return;
                    }).accepted();
                    break;
                }
            } catch (...) {
                read.ticket->complete(std::make_shared<const Snapshot>(snapshotUnavailable()));
                throw;
            }
            if (!posted)
                read.ticket->complete(std::make_shared<const Snapshot>(snapshotUnavailable()));
        }
        const auto remaining = deadline - SnapshotCache::Clock::now();
        if (remaining <= SnapshotCache::Clock::duration::zero())
            throw std::runtime_error("live resource read timed out");
        // A ready value invalidated before the check has no waiter. Rejoin the
        // new generation directly instead of waiting on an empty channel.
        if (read.ready.snapshot)
            continue;
        auto received = co_await read.receiver.receiveFor(remaining, c.stopToken());
        if (!received.hasValue())
            throw std::runtime_error("live resource read interrupted or timed out");
        const auto result = std::move(received).takeValue();
        if (result.snapshot && lease.current(result, minimumPatchEpoch))
            co_return result.snapshot;
    }
    throw std::runtime_error("live resource read interrupted");
}

} // namespace service::live_resource
