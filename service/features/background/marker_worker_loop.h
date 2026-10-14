#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

#include "service/features/background/worker_pool.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/state.h"

namespace service::background {

template <typename maintenance_type, typename process_available_type, typename format_error_type>
ruvia::task<void> run_marker_worker_loop(worker_context& context, std::chrono::seconds idle_poll_interval,
    std::string_view failure_log_prefix,
    std::string_view unknown_failure, maintenance_type maintenance,
    process_available_type process_available, format_error_type format_error) {
    auto next_lease_recovery = std::chrono::steady_clock::now();
    auto next_reconciliation = std::chrono::steady_clock::now();
    auto next_event_prune = std::chrono::steady_clock::now();
    while (!context.get_stop_token().stop_requested()) {
        std::string worker_error;
        std::size_t processed = 0;
        try {
            co_await maintenance(context, next_lease_recovery, next_reconciliation);
            if (context.get_stop_token().stop_requested()) {
                break;
            }
            if (std::chrono::steady_clock::now() >= next_event_prune) {
                auto db = context.db();
                co_await service::sync_runtime::prune_result_events(db);
                next_event_prune = std::chrono::steady_clock::now() + std::chrono::hours{1};
            }
            if (context.get_stop_token().stop_requested()) {
                break;
            }
            co_await process_available(context, processed);
        } catch (const std::exception& error) {
            worker_error = format_error(error.what());
        } catch (...) {
            worker_error = std::string(unknown_failure);
        }
        if (context.get_stop_token().stop_requested()) {
            break;
        }
        if (!worker_error.empty()) {
            service::logging::error(std::string(failure_log_prefix) + worker_error);
        }
        if ((processed == 0 || !worker_error.empty()) &&
            co_await ruvia::sleep_for(context.worker(), idle_poll_interval,
                context.get_stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
            break;
        }
    }
    co_return;
}

}  // namespace service::background
