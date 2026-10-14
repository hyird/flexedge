#pragma once

#include <ruvia/core/operation_options.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db_handle.h>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace service::live_resource {

class snapshot_read_scope final {
public:
    using clock = std::chrono::steady_clock;

    snapshot_read_scope(ruvia::context& context, clock::time_point deadline)
        : context_(context),
          deadline_(deadline),
          operation_stop_token_(ruvia::combine_stop_tokens(context.get_stop_token(), budget_stop_.token())),
          watchdog_(context.worker()) {
        watchdog_.spawn(watch_deadline());
    }
    ~snapshot_read_scope() = default;

    snapshot_read_scope(const snapshot_read_scope&) = delete;
    snapshot_read_scope& operator=(const snapshot_read_scope&) = delete;
    snapshot_read_scope(snapshot_read_scope&&) = delete;
    snapshot_read_scope& operator=(snapshot_read_scope&&) = delete;

    [[nodiscard]] std::pmr::memory_resource* pool() const noexcept {
        return context_.pool();
    }
    [[nodiscard]] const ruvia::worker_handle& worker() const noexcept {
        return context_.worker();
    }
    [[nodiscard]] bool deadline_exceeded() const noexcept {
        return clock::now() >= deadline_ || context_.deadline_exceeded() || budget_stop_.stop_requested();
    }
    [[nodiscard]] bool stop_requested() const noexcept {
        return context_.get_stop_token().stop_requested() || budget_stop_.stop_requested();
    }
    [[nodiscard]] ruvia::stop_token stop_token() const noexcept {
        return operation_stop_token_;
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] ruvia::db_handle db() const {
        return context_.db().with_options({.stop_token_ = stop_token()});
    }
#endif

    ruvia::task<void> stop_and_join() {
        watchdog_.request_stop();
        co_await watchdog_.join();
    }

private:
    ruvia::task<void> watch_deadline() {
        try {
            const auto now = clock::now();
            if (now < deadline_) {
                const auto result = co_await ruvia::sleep_for(
                    context_.worker(), deadline_ - now, watchdog_.get_stop_token());
                if (result == ruvia::timer_sleep_result::elapsed) {
                    budget_stop_.request_stop();
                }
            } else {
                budget_stop_.request_stop();
            }
        } catch (...) {
            budget_stop_.request_stop();
            throw;
        }
    }

    ruvia::context& context_;
    const clock::time_point deadline_;
    ruvia::stop_source budget_stop_;
    const ruvia::stop_token operation_stop_token_;
    ruvia::task_scope watchdog_;
};

// SSE tail reads retain the original aggregate wait budget while binding all
// dependency operations to the request and budget stop tokens.
template <typename t_type, typename fetch_type>
ruvia::task<t_type> read_once(ruvia::context& context, fetch_type fetch) {
    using clock = snapshot_read_scope::clock;
    const auto deadline = clock::now() + std::chrono::seconds(30);
    snapshot_read_scope scope(context, deadline);
    std::optional<t_type> result;
    std::exception_ptr read_failure;
    try {
        result.emplace(co_await fetch(scope));
    } catch (...) {
        read_failure = std::current_exception();
    }

    std::exception_ptr join_failure;
    try {
        co_await scope.stop_and_join();
    } catch (...) {
        join_failure = std::current_exception();
    }
    if (join_failure) {
        std::rethrow_exception(join_failure);
    }
    if (context.get_stop_token().stop_requested()) {
        throw std::runtime_error("live resource read interrupted");
    }
    if (scope.deadline_exceeded()) {
        throw std::runtime_error("live resource read timed out");
    }
    if (read_failure) {
        std::rethrow_exception(read_failure);
    }
    co_return std::move(*result);
}

}  // namespace service::live_resource
