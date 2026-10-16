#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/background/stream_consumers.h"
#include "service/features/log_ingest/enqueue.h"
#include "service/features/log_ingest/ingest.h"
#include "service/features/logging/logger.h"

namespace service::log_ingest {

inline bool is_permanent_database_failure(const ruvia::db_error& error) {
    const auto state = error.sql_state();
    return state && (state->starts_with("22") || state->starts_with("23"));
}

template <typename context_type>
inline ruvia::task<std::size_t> persist_deliveries(context_type& context, std::string_view consumer,
    std::vector<queue::entry> entries) {
    std::size_t processed{};
    for (const auto& entry : entries) {
        const auto envelope = entry.valid_ ? parse_envelope(entry.payload_) : std::nullopt;
        bool insert_failed{};
        std::string permanent_failure;
        if (!envelope) {
            permanent_failure = "invalid_log_envelope";
        } else {
            try {
                const auto separator = entry.id_.find('-');
                const auto received_unix_ms = queue::parse_integer(std::string_view(entry.id_).substr(0, separator));
                co_await insert_envelope(context, *envelope, received_unix_ms);
            } catch (const invalid_delivery& error) {
                permanent_failure = error.what();
            } catch (const ruvia::db_error& error) {
                if (is_permanent_database_failure(error)) {
                    permanent_failure = "database_invalid_log_data:" + std::string(*error.sql_state());
                } else {
                    insert_failed = true;
                }
            } catch (...) {
                insert_failed = true;
            }
        }
        if (!permanent_failure.empty()) {
            const auto result = co_await queue::fail(context.redis(), consumer, entry, permanent_failure);
            if (result != queue::failure_result::full) {
                if (result == queue::failure_result::archived) {
                    service::logging::error("Log delivery archived for replay: " + permanent_failure);
                }
                continue;
            }
            insert_failed = true;
            service::logging::error("Log failure archive is full; original delivery retained");
        }
        if (insert_failed) {
            const auto delay = co_await queue::schedule_retry(context.redis(), consumer, entry);
            if (delay.count() > 0) {
                service::logging::error("Log delivery retained; retry scheduled in " +
                                        std::to_string(delay.count()) + "ms");
            }
            continue;
        }
        co_await queue::finish(context.redis(), consumer, entry);
        ++processed;
    }
    co_return processed;
}

template <typename context_type>
inline ruvia::task<void> recover_stale(context_type& context, std::string_view consumer) {
    auto claimed = co_await queue::reclaim_stale(context.redis(), consumer);
    if (!claimed.empty()) {
        (void)co_await persist_deliveries(context, consumer, std::move(claimed));
    }
}

template <typename context_type>
inline ruvia::task<void> run_worker(context_type& context, std::string consumer) {
    if (consumer.empty()) {
        throw std::invalid_argument("log ingest consumer name cannot be empty");
    }
    // Subscribe before the first claim: deliveries this worker enqueues later
    // wake it at once.
    const auto own = service::background::work_available::subscribe(context, work_type);
    bool initialized{};
    auto next_recovery = std::chrono::steady_clock::now();
    auto next_consumer_cleanup = next_recovery;
    while (!context.get_stop_token().stop_requested()) {
        bool failed{};
        try {
            if (!initialized) {
                co_await queue::initialize(context.redis());
                initialized = true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_consumer_cleanup) {
                (void)co_await service::background::stream_consumers::remove_idle(
                    context.redis(), queue::stream_key, queue::consumer_group, consumer);
                next_consumer_cleanup = now + service::background::stream_consumers::cleanup_interval;
            }
            if (now >= next_recovery) {
                co_await recover_stale(context, consumer);
                next_recovery = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            auto retries = co_await queue::claim_retries(context.redis(), consumer);
            if (!retries.empty()) {
                (void)co_await persist_deliveries(context, consumer, std::move(retries));
            }
            auto entries = co_await queue::read(context.redis(), consumer);
            if (!entries.empty()) {
                (void)co_await persist_deliveries(context, consumer, std::move(entries));
                continue;
            }
            (void)co_await service::background::stream_consumers::remove_empty(
                context.redis(), queue::stream_key, queue::consumer_group, consumer);
            // Only a wait deadline is local; Redis retains every retry record.
            // New deliveries wake immediately, retries use their real deadline,
            // and abandoned claims retain the existing recovery cadence.
            const auto recovery_wait = std::chrono::duration_cast<std::chrono::milliseconds>(
                next_recovery - std::chrono::steady_clock::now());
            const auto wait = co_await queue::retry_wait(context.redis(), recovery_wait);
            (void)co_await own.receive_for(wait, context.get_stop_token());
        } catch (const std::exception& error) {
            if (context.get_stop_token().stop_requested()) {
                co_return;
            }
            initialized = false;
            failed = true;
            service::logging::error("Log ingest worker failure: " + std::string(error.what()));
        } catch (...) {
            if (context.get_stop_token().stop_requested()) {
                co_return;
            }
            initialized = false;
            failed = true;
            service::logging::error("Log ingest worker failure with unknown exception");
        }
        if (failed && co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(1),
                          context.get_stop_token()) ==
                          ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
    }
}

}  // namespace service::log_ingest
