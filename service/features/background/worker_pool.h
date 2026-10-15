#pragma once

#include <ruvia/core/event_loop.h>
#include <ruvia/core/move_only_function.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/web/db/db_handle.h>
#include <ruvia/web/db/db_types.h>
#include <ruvia/web/http_client.h>
#include <ruvia/web/redis/redis_handle.h>
#include <ruvia/web/redis/redis_types.h>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/config/outbound.h"
#include "service/features/live_resource/notifications.h"

namespace service::background {

class worker_pool;

class worker_context final {
public:
    ~worker_context();

    worker_context(const worker_context&) = delete;
    worker_context& operator=(const worker_context&) = delete;
    worker_context(worker_context&&) = delete;
    worker_context& operator=(worker_context&&) = delete;

    [[nodiscard]] ruvia::db_handle db() const;
    // This worker's own Redis connection; it carries the worker's resource
    // notifications, sent after each commit.
    [[nodiscard]] ruvia::redis_handle redis() const;
    [[nodiscard]] service::live_resource::notifications::sender& resource_notifications() const noexcept;
    [[nodiscard]] ruvia::http_client& get_http_client(std::string_view alias) const;
    [[nodiscard]] const ruvia::worker_handle& worker() const noexcept;
    [[nodiscard]] std::pmr::memory_resource* pool() const noexcept;
    [[nodiscard]] ruvia::stop_token get_stop_token() const noexcept;
    [[nodiscard]] std::string_view lease_owner() const noexcept;

private:
    struct impl;

    worker_context(ruvia::event_loop loop, ruvia::db_config database, const ruvia::redis_config& redis,
        const service::config::outbound_origins& origins, std::string worker_name,
        std::span<const std::string_view> outbound_aliases);

    void close() noexcept;

    std::unique_ptr<impl> impl_;
    friend class worker_pool;
};

struct worker_definition final {
    std::string name_;
    std::vector<std::string_view> outbound_aliases_;
    ruvia::move_only_function<ruvia::task<void>(worker_context&)> run_;
};

class worker_pool final {
public:
    worker_pool(const ruvia::db_config& database, const ruvia::redis_config& redis,
        const service::config::outbound_origins& origins, std::vector<worker_definition> workers);
    ~worker_pool();

    worker_pool(const worker_pool&) = delete;
    worker_pool& operator=(const worker_pool&) = delete;
    worker_pool(worker_pool&&) = delete;
    worker_pool& operator=(worker_pool&&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] std::size_t worker_count() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace service::background
