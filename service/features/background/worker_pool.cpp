#include "service/features/background/worker_pool.h"

#include <atomic>
#include <exception>
#include <future>
#include <stdexcept>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include <ruvia/core/AsioTask.h>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/memory/MemoryPool.h>
#include <ruvia/web/App.h>

#include "service/features/logging/logger.h"
#include "service/utils/token.h"

namespace service::background {

struct WorkerContext::Impl final {
    struct OutboundClient final {
        std::string_view alias;
        std::unique_ptr<ruvia::HttpClient> client;
    };

    Impl(ruvia::EventLoop eventLoop, ruvia::DbConfig database,
         const service::config::OutboundOrigins& origins, std::string workerName,
         std::span<const std::string_view> outboundAliases)
        : loop(std::move(eventLoop)), worker(loop.handle()), database(loop, std::move(database)) {
        clients.reserve(outboundAliases.size());
        for (const auto alias : outboundAliases) {
            for (const auto& client : clients) {
                if (client.alias == alias) {
                    throw std::invalid_argument("background worker origin is declared twice");
                }
            }
            clients.push_back({
                .alias = alias,
                .client = std::make_unique<ruvia::HttpClient>(
                    loop, service::config::outboundOriginConfig(origins, alias)),
            });
        }
        leaseOwner = std::move(workerName) + ":" + service::utils::randomToken().substr(0, 32);
        stopRegistration = loop.onStop([this] { stopSource.requestStop(); });
    }

    ruvia::EventLoop loop;
    ruvia::WorkerHandle worker;
    ruvia::WorkerMemory memory;
    ruvia::StopSource stopSource;
    ruvia::DbClient database;
    std::vector<OutboundClient> clients;
    std::string leaseOwner;
    ruvia::EventLoopStopRegistration stopRegistration;
};

WorkerContext::WorkerContext(ruvia::EventLoop loop, ruvia::DbConfig database,
                             const service::config::OutboundOrigins& origins,
                             std::string workerName,
                             std::span<const std::string_view> outboundAliases)
    : impl_(std::make_unique<Impl>(std::move(loop), std::move(database), origins,
                                   std::move(workerName), outboundAliases)) {}

WorkerContext::~WorkerContext() = default;

ruvia::DbClient& WorkerContext::db() const noexcept { return impl_->database; }

ruvia::HttpClient& WorkerContext::httpClient(std::string_view alias) const {
    for (const auto& client : impl_->clients) {
        if (client.alias == alias) {
            return *client.client;
        }
    }
    throw std::invalid_argument("background HTTP origin is not configured");
}

const ruvia::WorkerHandle& WorkerContext::worker() const noexcept { return impl_->worker; }

std::pmr::memory_resource* WorkerContext::resource() const noexcept {
    return impl_->memory.resource();
}

ruvia::StopToken WorkerContext::stopToken() const noexcept { return impl_->stopSource.token(); }

std::string_view WorkerContext::leaseOwner() const noexcept { return impl_->leaseOwner; }

void WorkerContext::close() noexcept {
    impl_->stopSource.requestStop();
    for (const auto& client : impl_->clients) {
        client.client->close();
    }
    impl_->database.close();
}

struct WorkerPool::Impl final {
    Impl(const ruvia::DbConfig& database, const service::config::OutboundOrigins& origins,
         std::vector<WorkerDefinition> definitions)
        : loops({.loopCount = definitions.size(), .mailboxCapacity = 256}),
          workers(std::move(definitions)) {
        if (workers.empty()) {
            throw std::invalid_argument("background worker pool requires at least one worker");
        }
        contexts.reserve(workers.size());
        for (std::size_t index = 0; index < workers.size(); ++index) {
            contexts.push_back(std::unique_ptr<WorkerContext>(
                new WorkerContext(loops.loop(index), database, origins, workers[index].name,
                                  workers[index].outboundAliases)));
        }
    }

    void startTask(std::size_t index) {
        try {
            auto task = workers[index].run(*contexts[index]);
            asio::co_spawn(
                loops.loop(index).executor(), ruvia::asAwaitable(std::move(task)),
                [this, index](const std::exception_ptr& failure) {
                    if (failure) {
                        try {
                            std::rethrow_exception(failure);
                        } catch (const std::exception& error) {
                            service::logging::error("Background worker " + workers[index].name +
                                                    " failed: " + error.what());
                        } catch (...) {
                            service::logging::error("Background worker " + workers[index].name +
                                                    " failed with an unknown exception");
                        }
                    } else if (!contexts[index]->stopToken().stopRequested()) {
                        service::logging::error("Background worker " + workers[index].name +
                                                " stopped unexpectedly");
                    } else {
                        return;
                    }
                    ruvia::app().stop();
                });
        } catch (const std::exception& error) {
            service::logging::error("Background worker " + workers[index].name +
                                    " could not start: " + error.what());
            ruvia::app().stop();
        } catch (...) {
            service::logging::error("Background worker " + workers[index].name +
                                    " could not start");
            ruvia::app().stop();
        }
    }

    void start() {
        if (started.exchange(true)) {
            throw std::logic_error("background worker pool already started");
        }

        std::vector<std::future<void>> connections;
        connections.reserve(contexts.size());
        for (std::size_t index = 0; index < contexts.size(); ++index) {
            connections.push_back(asio::co_spawn(
                loops.loop(index).executor(), ruvia::asAwaitable(contexts[index]->db().connect()),
                asio::use_future));
        }

        try {
            loops.start();
            for (auto& connection : connections) {
                connection.get();
            }
            for (std::size_t index = 0; index < workers.size(); ++index) {
                const auto status = loops.loop(index).post([this, index] { startTask(index); });
                if (status != ruvia::PostStatus::kAccepted) {
                    throw std::runtime_error("background worker mailbox rejected startup");
                }
            }
            service::logging::info("Background worker pool started with " +
                                   std::to_string(workers.size()) + " workers");
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        if (stopped.exchange(true)) {
            return;
        }
        for (auto& context : contexts) {
            context->close();
        }
        loops.stop();
        try {
            loops.join();
        } catch (const std::exception& error) {
            service::logging::error("Background worker pool shutdown failed: " +
                                    std::string(error.what()));
        } catch (...) {
            service::logging::error("Background worker pool shutdown failed");
        }
        if (started.load()) {
            service::logging::info("Background worker pool stopped");
        }
    }

    ruvia::EventLoopPool loops;
    std::vector<WorkerDefinition> workers;
    std::vector<std::unique_ptr<WorkerContext>> contexts;
    std::atomic_bool started{false};
    std::atomic_bool stopped{false};
};

WorkerPool::WorkerPool(const ruvia::DbConfig& database,
                       const service::config::OutboundOrigins& origins,
                       std::vector<WorkerDefinition> workers)
    : impl_(std::make_unique<Impl>(database, origins, std::move(workers))) {}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::start() { impl_->start(); }

void WorkerPool::stop() noexcept { impl_->stop(); }

std::size_t WorkerPool::workerCount() const noexcept { return impl_->workers.size(); }

} // namespace service::background
