#include "service/features/background/worker_pool.h"

#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/memory/memory_pool.h>
#include <ruvia/core/root_task.h>
#include <ruvia/web/app.h>
#include <ruvia/web/db/db_client.h>

#include <atomic>
#include <exception>
#include <stdexcept>
#include <string_view>

#include "service/features/logging/logger.h"
#include "service/utils/token.h"

namespace service::background {

struct worker_context::impl final {
    struct outbound_client final {
        std::string_view alias_;
        std::unique_ptr<ruvia::http_client> client_;
    };

    impl(ruvia::event_loop event_loop, ruvia::db_config database,
        const service::config::outbound_origins& origins, std::string worker_name,
        std::span<const std::string_view> outbound_aliases)
        : loop_(std::move(event_loop)),
          worker_(loop_.handle()),
          database_(loop_, std::move(database)) {
        clients_.reserve(outbound_aliases.size());
        for (const auto alias : outbound_aliases) {
            for (const auto& client : clients_) {
                if (client.alias_ == alias) {
                    throw std::invalid_argument("background worker origin is declared twice");
                }
            }
            clients_.push_back({
                .alias_ = alias,
                .client_ = std::make_unique<ruvia::http_client>(
                    loop_, service::config::outbound_origin_config(origins, alias)),
            });
        }
        lease_owner_ = std::move(worker_name) + ":" + service::utils::random_token().substr(0, 32);
        stop_registration_ = loop_.on_stop([this]() -> ruvia::task<void> {
            stop_source_.request_stop();
            for (const auto& client : clients_) {
                client.client_->close();
            }
            this->database_.close();
            for (const auto& client : clients_) {
                co_await client.client_->shutdown();
            }
            co_await this->database_.shutdown();
        });
    }

    ruvia::event_loop loop_;
    ruvia::worker_handle worker_;
    ruvia::worker_memory memory_;
    ruvia::stop_source stop_source_;
    ruvia::db_client database_;
    std::vector<outbound_client> clients_;
    std::string lease_owner_;
    ruvia::event_loop_stop_registration stop_registration_;
};

worker_context::worker_context(ruvia::event_loop loop, ruvia::db_config database,
    const service::config::outbound_origins& origins,
    std::string worker_name,
    std::span<const std::string_view> outbound_aliases)
    : impl_(std::make_unique<impl>(std::move(loop), std::move(database), origins,
          std::move(worker_name), outbound_aliases)) {}

worker_context::~worker_context() = default;

ruvia::db_handle worker_context::db() const {
    return impl_->database_.handle();
}

ruvia::http_client& worker_context::get_http_client(std::string_view alias) const {
    for (const auto& client : impl_->clients_) {
        if (client.alias_ == alias) {
            return *client.client_;
        }
    }
    throw std::invalid_argument("background HTTP origin is not configured");
}

const ruvia::worker_handle& worker_context::worker() const noexcept {
    return impl_->worker_;
}

std::pmr::memory_resource* worker_context::pool() const noexcept {
    return impl_->memory_.resource();
}

ruvia::stop_token worker_context::get_stop_token() const noexcept {
    return impl_->stop_source_.token();
}

std::string_view worker_context::lease_owner() const noexcept {
    return impl_->lease_owner_;
}

void worker_context::close() noexcept {
    impl_->stop_source_.request_stop();
    for (const auto& client : impl_->clients_) {
        client.client_->close();
    }
    impl_->database_.close();
}

struct worker_pool::impl final {
    impl(const ruvia::db_config& database, const service::config::outbound_origins& origins,
        std::vector<worker_definition> definitions)
        : loops_({.loop_count_ = definitions.size(), .queue_capacity_ = 256}),
          workers_(std::move(definitions)) {
        if (workers_.empty()) {
            throw std::invalid_argument("background worker pool requires at least one worker");
        }
        contexts_.reserve(workers_.size());
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            contexts_.push_back(std::unique_ptr<worker_context>(
                new worker_context(loops_.loop(index), database, origins, workers_[index].name_,
                    workers_[index].outbound_aliases_)));
        }
    }

    ruvia::task<void> run_task(std::size_t index) {
        try {
            co_await workers_[index].run_(*contexts_[index]);
            if (!contexts_[index]->get_stop_token().stop_requested()) {
                service::logging::error("Background worker " + workers_[index].name_ +
                                        " stopped unexpectedly");
                ruvia::app().stop();
            }
        } catch (const std::exception& error) {
            if (contexts_[index]->get_stop_token().stop_requested()) {
                co_return;
            }
            service::logging::error("Background worker " + workers_[index].name_ +
                                    " failed: " + error.what());
            ruvia::app().stop();
            throw;
        } catch (...) {
            if (contexts_[index]->get_stop_token().stop_requested()) {
                co_return;
            }
            service::logging::error("Background worker " + workers_[index].name_ +
                                    " failed with an unknown exception");
            ruvia::app().stop();
            throw;
        }
    }

    void start_task(std::size_t index) {
        tasks_.push_back(loops_.loop(index).start(run_task(index)));
    }

    void wait_tasks() noexcept {
        for (std::size_t index = 0; index < tasks_.size(); ++index) {
            try {
                tasks_[index].get();
                if (!contexts_[index]->get_stop_token().stop_requested()) {
                    service::logging::error("Background worker " + workers_[index].name_ +
                                            " stopped unexpectedly");
                    ruvia::app().stop();
                }
            } catch (...) {
                // run_task() has already reported the failure and requested shutdown.
                ruvia::app().stop();
            }
        }
        tasks_.clear();
    }

    void wait_connections() noexcept {
        for (auto& connection : connection_tasks_) {
            if (!connection.valid()) {
                continue;
            }
            try {
                connection.get();
            } catch (const std::exception& error) {
                service::logging::error("Background database connection shutdown failed: " +
                                        std::string(error.what()));
            } catch (...) {
                service::logging::error("Background database connection shutdown failed");
            }
        }
        connection_tasks_.clear();
    }

    void start() {
        if (started_.exchange(true)) {
            throw std::logic_error("background worker pool already started");
        }

        try {
            loops_.start();
            connection_tasks_.reserve(contexts_.size());
            for (std::size_t index = 0; index < contexts_.size(); ++index) {
                connection_tasks_.push_back(
                    loops_.loop(index).start(contexts_[index]->impl_->database_.connect()));
            }
            for (auto& connection : connection_tasks_) {
                connection.get();
            }
            tasks_.reserve(workers_.size());
            for (std::size_t index = 0; index < workers_.size(); ++index) {
                start_task(index);
            }
            service::logging::info("Background worker pool started with " +
                                   std::to_string(workers_.size()) + " workers");
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        if (stopped_.exchange(true)) {
            return;
        }
        for (auto& context : contexts_) {
            context->close();
        }
        loops_.stop();
        wait_tasks();
        wait_connections();
        try {
            loops_.join();
        } catch (const std::exception& error) {
            service::logging::error("Background worker pool shutdown failed: " +
                                    std::string(error.what()));
        } catch (...) {
            service::logging::error("Background worker pool shutdown failed");
        }
        if (started_.load()) {
            service::logging::info("Background worker pool stopped");
        }
    }

    ruvia::event_loop_pool loops_;
    std::vector<worker_definition> workers_;
    std::vector<std::unique_ptr<worker_context>> contexts_;
    std::vector<ruvia::root_task<void>> connection_tasks_;
    std::vector<ruvia::root_task<void>> tasks_;
    std::atomic_bool started_{false};
    std::atomic_bool stopped_{false};
};

worker_pool::worker_pool(const ruvia::db_config& database,
    const service::config::outbound_origins& origins,
    std::vector<worker_definition> workers)
    : impl_(std::make_unique<impl>(database, origins, std::move(workers))) {}

worker_pool::~worker_pool() {
    stop();
}

void worker_pool::start() {
    impl_->start();
}

void worker_pool::stop() noexcept {
    impl_->stop();
}

std::size_t worker_pool::worker_count() const noexcept {
    return impl_->workers_.size();
}

}  // namespace service::background
