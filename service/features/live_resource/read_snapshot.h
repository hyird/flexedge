#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>

#include "service/common/http.h"
#include "service/features/live_resource/read_scope.h"
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

namespace detail {

template <typename Fetch>
ruvia::Task<Snapshot> fetchSnapshotAndJoinWatchdog(
    ruvia::Context& context, SnapshotCache::Clock::time_point deadline, Fetch& fetch) {
    SnapshotReadScope scope(context, deadline);
    std::optional<Snapshot> candidate;
    std::exception_ptr fetchFailure;
    try {
        try {
            candidate.emplace(Snapshot{co_await fetch(scope), false});
        } catch (const ruvia::HttpError& error) {
            candidate.emplace(snapshotError(error.info()));
        } catch (const std::exception& error) {
            if (!scope.stopRequested())
                service::logging::error("Live resource read failure: " + std::string(error.what()));
            candidate.emplace(snapshotUnavailable());
        } catch (...) {
            candidate.emplace(snapshotUnavailable());
        }
    } catch (...) {
        fetchFailure = std::current_exception();
    }

    std::exception_ptr joinFailure;
    try {
        co_await scope.stopAndJoin();
    } catch (...) {
        joinFailure = std::current_exception();
    }
    if (joinFailure) std::rethrow_exception(joinFailure);
    if (fetchFailure) std::rethrow_exception(fetchFailure);
    if (context.stopToken().stopRequested())
        throw std::runtime_error("live resource read interrupted");
    if (scope.deadlineExceeded())
        throw std::runtime_error("live resource read timed out");
    co_return std::move(*candidate);
}

inline void checkSnapshotReadBudget(
    const ruvia::Context& context, SnapshotCache::Clock::time_point deadline) {
    if (context.stopToken().stopRequested())
        throw std::runtime_error("live resource read interrupted");
    if (SnapshotCache::Clock::now() >= deadline || context.deadlineExceeded())
        throw std::runtime_error("live resource read timed out");
}

}  // namespace detail

template <typename Fetch>
ruvia::Task<std::shared_ptr<const Snapshot>> readSnapshotUntil(
    ruvia::Context& context, const SnapshotCache::Lease& lease,
    SnapshotCache::Clock::time_point deadline, Fetch fetch) {
    std::uint64_t minimumPatchEpoch{};
    std::shared_ptr<const Snapshot> answer;
    std::exception_ptr failure;
    std::optional<SnapshotCache::Ticket> activeTicket;

    try {
        minimumPatchEpoch = lease.patchEpoch();
        for (;;) {
            detail::checkSnapshotReadBudget(context, deadline);
            auto read = lease.read(context.worker());
            if (read.ready.snapshot && lease.current(read.ready, minimumPatchEpoch)) {
                answer = read.ready.snapshot;
                break;
            }

            if (read.ticket) {
                activeTicket.emplace(*read.ticket);
                auto candidate = co_await detail::fetchSnapshotAndJoinWatchdog(context, deadline, fetch);
                detail::checkSnapshotReadBudget(context, deadline);
                activeTicket->complete(std::make_shared<const Snapshot>(std::move(candidate)));
                activeTicket.reset();
            }

            const auto remaining = deadline - SnapshotCache::Clock::now();
            if (remaining <= SnapshotCache::Clock::duration::zero())
                throw std::runtime_error("live resource read timed out");
            if (read.ready.snapshot) continue;
            auto received = co_await read.receiver.receiveFor(remaining, context.stopToken());
            if (!received.hasValue()) {
                detail::checkSnapshotReadBudget(context, deadline);
                throw std::runtime_error("live resource read timed out");
            }
            const auto result = std::move(received).takeValue();
            if (result.snapshot && lease.current(result, minimumPatchEpoch)) {
                answer = result.snapshot;
                break;
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }

    if (activeTicket) {
        try {
            activeTicket->cancel();
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
    detail::checkSnapshotReadBudget(context, deadline);
    co_return answer;
}

template <typename Fetch>
ruvia::Task<std::shared_ptr<const Snapshot>> readSnapshot(
    ruvia::Context& context, const SnapshotCache::Lease& lease, Fetch fetch) {
    const auto deadline = SnapshotCache::Clock::now() + std::chrono::seconds(30);
    co_return co_await readSnapshotUntil(context, lease, deadline, std::move(fetch));
}

}  // namespace service::live_resource
