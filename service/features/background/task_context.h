#pragma once

#include <ruvia/core/stop_token.h>
#include <ruvia/web/web_worker.h>

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "service/utils/token.h"

namespace service::background {

// One task invocation borrows its accepting Web Worker's capabilities. The
// consumer identity only fences durable leases; it never selects an executor.
class task_context final {
public:
    explicit task_context(ruvia::web_worker_context& context, ruvia::stop_token stop = {})
        : context_(context),
          stop_token_(ruvia::combine_stop_tokens(context.get_stop_token(), std::move(stop))),
          lease_owner_(service::utils::random_token()) {}

    task_context(const task_context&) = delete;
    task_context& operator=(const task_context&) = delete;
    task_context(task_context&&) = delete;
    task_context& operator=(task_context&&) = delete;

    [[nodiscard]] ruvia::db_handle db() const {
        return context_.db().with_options({.stop_token_ = stop_token_});
    }

    [[nodiscard]] ruvia::redis_handle redis() const {
        return context_.redis().with_options({.stop_token_ = stop_token_});
    }

    [[nodiscard]] ruvia::http_client_handle get_http_client(std::string_view alias) const {
        return context_.get_http_client(alias).with_options({.stop_token_ = stop_token_});
    }

    [[nodiscard]] const ruvia::worker_handle& worker() const noexcept {
        return context_.worker();
    }

    [[nodiscard]] std::pmr::memory_resource* pool() const noexcept {
        return context_.pool();
    }

    [[nodiscard]] ruvia::stop_token get_stop_token() const noexcept {
        return stop_token_;
    }

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return context_.template worker_state<state_type>();
    }

    [[nodiscard]] std::string_view lease_owner() const noexcept {
        return lease_owner_;
    }

private:
    ruvia::web_worker_context& context_;
    const ruvia::stop_token stop_token_;
    const std::string lease_owner_;
};

}  // namespace service::background
