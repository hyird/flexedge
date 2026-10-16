#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

#include "service/features/background/task_context.h"
#include "service/features/background/work_available.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/state.h"

namespace service::background {

template <typename maintenance_type, typename process_available_type, typename format_error_type>
ruvia::task<void> run_marker_worker_loop(task_context& context, std::string_view task_type,
    std::string_view failure_log_prefix,
    std::string_view unknown_failure, maintenance_type maintenance,
    process_available_type process_available, format_error_type format_error) {
    // Register before the first durable claim. A racing broadcast stays in
    // this worker-local capacity-one channel until the loop becomes idle.
    auto& signal = work_available::get_signals(context).for_type(task_type);
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
                co_await service::sync_runtime::prune_result_events(context);
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
        if (processed == 0 && worker_error.empty()) {
            try {
                co_await signal.wait(std::chrono::seconds(15), context.get_stop_token());
            } catch (const std::exception&) {
                if (context.get_stop_token().stop_requested()) {
                    break;
                }
                worker_error = "task wakeup unavailable";
            }
        }
        if (!worker_error.empty() && co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(2),
                                         context.get_stop_token()) == ruvia::timer_sleep_result::stop_requested) {
            break;
        }
    }
    co_return;
}

}  // namespace service::background
