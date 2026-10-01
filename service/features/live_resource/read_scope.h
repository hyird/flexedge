#pragma once

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <ruvia/core/OperationOptions.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/Context.h>

namespace service::live_resource {

class SnapshotReadScope final {
public:
    using Clock = std::chrono::steady_clock;

    SnapshotReadScope(ruvia::Context& context, Clock::time_point deadline)
        : context_(context), deadline_(deadline),
          operationStopToken_(ruvia::combineStopTokens(context.stopToken(), budgetStop_.token())),
          watchdog_(context.worker()) {
        watchdog_.spawn(watchDeadline());
    }
    ~SnapshotReadScope() = default;

    SnapshotReadScope(const SnapshotReadScope&) = delete;
    SnapshotReadScope& operator=(const SnapshotReadScope&) = delete;
    SnapshotReadScope(SnapshotReadScope&&) = delete;
    SnapshotReadScope& operator=(SnapshotReadScope&&) = delete;

    [[nodiscard]] std::pmr::memory_resource* pool() const noexcept { return context_.pool(); }
    [[nodiscard]] const ruvia::WorkerHandle& worker() const noexcept { return context_.worker(); }
    [[nodiscard]] bool deadlineExceeded() const noexcept {
        return Clock::now() >= deadline_ || context_.deadlineExceeded() || budgetStop_.stopRequested();
    }
    [[nodiscard]] bool stopRequested() const noexcept {
        return context_.stopToken().stopRequested() || budgetStop_.stopRequested();
    }
    [[nodiscard]] ruvia::StopToken stopToken() const noexcept { return operationStopToken_; }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] ruvia::DbHandle db() const {
        return context_.db().withOptions({.stopToken = stopToken()});
    }
#endif

    ruvia::Task<void> stopAndJoin() {
        watchdog_.requestStop();
        co_await watchdog_.join();
    }

private:
    ruvia::Task<void> watchDeadline() {
        try {
            const auto now = Clock::now();
            if (now < deadline_) {
                const auto result = co_await ruvia::sleepFor(
                    context_.worker(), deadline_ - now, watchdog_.stopToken());
                if (result == ruvia::TimerSleepResult::kElapsed) budgetStop_.requestStop();
            } else {
                budgetStop_.requestStop();
            }
        } catch (...) {
            budgetStop_.requestStop();
            throw;
        }
    }

    ruvia::Context& context_;
    const Clock::time_point deadline_;
    ruvia::StopSource budgetStop_;
    const ruvia::StopToken operationStopToken_;
    ruvia::TaskScope watchdog_;
};

// SSE tail reads retain the original aggregate wait budget while binding all
// dependency operations to the request and budget stop tokens.
template <typename T, typename Fetch>
ruvia::Task<T> readOnce(ruvia::Context& context, Fetch fetch) {
    using Clock = SnapshotReadScope::Clock;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    SnapshotReadScope scope(context, deadline);
    std::optional<T> result;
    std::exception_ptr readFailure;
    try {
        result.emplace(co_await fetch(scope));
    } catch (...) {
        readFailure = std::current_exception();
    }

    std::exception_ptr joinFailure;
    try {
        co_await scope.stopAndJoin();
    } catch (...) {
        joinFailure = std::current_exception();
    }
    if (joinFailure) std::rethrow_exception(joinFailure);
    if (context.stopToken().stopRequested())
        throw std::runtime_error("live resource read interrupted");
    if (scope.deadlineExceeded())
        throw std::runtime_error("live resource read timed out");
    if (readFailure) std::rethrow_exception(readFailure);
    co_return std::move(*result);
}

}  // namespace service::live_resource
