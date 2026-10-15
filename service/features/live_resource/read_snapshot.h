#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>
#include <ruvia/web/context.h>
#include <ruvia/web/error.h>
#include <ruvia/web/model_json.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "service/common/http.h"
#include "service/features/live_resource/read_scope.h"
#include "service/features/live_resource/snapshot_cache.h"
#include "service/features/logging/logger.h"

namespace service::live_resource {

inline snapshot snapshot_error(const ruvia::http_error_info& info) {
    service::common::error_response response(
        ruvia::model_options{.resource_ = std::pmr::new_delete_resource()});
    response.set<"code">(service::common::normalize_business_error_code(info.code(), info.status().value()));
    response.set<"message">(service::common::response_error_message(info));
    return {std::string(ruvia::to_json(response, {.resource_ = std::pmr::new_delete_resource()})), true};
}

inline snapshot snapshot_unavailable() {
    return {R"({"code":10004,"message":"资源读取暂时不可用，请稍后重试"})", true};
}

namespace detail {

template <typename fetch_type>
ruvia::task<snapshot> fetch_snapshot_and_join_watchdog(
    ruvia::context& context, snapshot_cache::clock::time_point deadline, fetch_type& fetch) {
    snapshot_read_scope scope(context, deadline);
    std::optional<snapshot> candidate;
    std::exception_ptr fetch_failure;
    try {
        try {
            candidate.emplace(snapshot{co_await fetch(scope), false});
        } catch (const ruvia::http_error& error) {
            candidate.emplace(snapshot_error(error.info()));
        } catch (const std::exception& error) {
            if (!scope.stop_requested()) {
                service::logging::error("Live resource read failure: " + std::string(error.what()));
            }
            candidate.emplace(snapshot_unavailable());
        } catch (...) {
            candidate.emplace(snapshot_unavailable());
        }
    } catch (...) {
        fetch_failure = std::current_exception();
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
    if (fetch_failure) {
        std::rethrow_exception(fetch_failure);
    }
    if (context.get_stop_token().stop_requested()) {
        throw std::runtime_error("live resource read interrupted");
    }
    if (scope.deadline_exceeded()) {
        throw std::runtime_error("live resource read timed out");
    }
    co_return std::move(*candidate);
}

inline void check_snapshot_read_budget(
    const ruvia::context& context, snapshot_cache::clock::time_point deadline) {
    if (context.get_stop_token().stop_requested()) {
        throw std::runtime_error("live resource read interrupted");
    }
    if (snapshot_cache::clock::now() >= deadline) {
        throw std::runtime_error("live resource read timed out");
    }
}

}  // namespace detail

template <typename fetch_type>
ruvia::task<std::shared_ptr<const snapshot>> read_snapshot_until(
    ruvia::context& context, const snapshot_cache::lease& lease,
    snapshot_cache::clock::time_point deadline, fetch_type fetch) {
    std::uint64_t minimum_patch_epoch{};
    std::shared_ptr<const snapshot> answer;
    std::exception_ptr failure;
    std::optional<snapshot_cache::ticket> active_ticket;

    try {
        minimum_patch_epoch = lease.patch_epoch();
        for (;;) {
            detail::check_snapshot_read_budget(context, deadline);
            auto read = lease.read(context.worker());
            if (read.ready_.snapshot_ && lease.current(read.ready_, minimum_patch_epoch)) {
                answer = read.ready_.snapshot_;
                break;
            }

            if (read.ticket_) {
                active_ticket.emplace(*read.ticket_);
                auto candidate = co_await detail::fetch_snapshot_and_join_watchdog(context, deadline, fetch);
                detail::check_snapshot_read_budget(context, deadline);
                active_ticket->complete(std::make_shared<const snapshot>(std::move(candidate)));
                active_ticket.reset();
            }

            const auto remaining = deadline - snapshot_cache::clock::now();
            if (remaining <= snapshot_cache::clock::duration::zero()) {
                throw std::runtime_error("live resource read timed out");
            }
            if (read.ready_.snapshot_) {
                continue;
            }
            auto received = co_await read.receiver_.receive_for(remaining, context.get_stop_token());
            if (!received.has_value()) {
                detail::check_snapshot_read_budget(context, deadline);
                throw std::runtime_error("live resource read timed out");
            }
            const auto result = std::move(received).take_value();
            if (result.snapshot_ && lease.current(result, minimum_patch_epoch)) {
                answer = result.snapshot_;
                break;
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }

    if (active_ticket) {
        try {
            active_ticket->cancel();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    detail::check_snapshot_read_budget(context, deadline);
    co_return answer;
}

template <typename fetch_type>
ruvia::task<std::shared_ptr<const snapshot>> read_snapshot(
    ruvia::context& context, const snapshot_cache::lease& lease, fetch_type fetch) {
    const auto deadline = snapshot_cache::clock::now() + std::chrono::seconds(30);
    co_return co_await read_snapshot_until(context, lease, deadline, std::move(fetch));
}

}  // namespace service::live_resource
