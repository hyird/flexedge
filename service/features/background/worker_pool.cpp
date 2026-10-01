#include "service/features/background/worker_pool.h"

#include <atomic>
#include <exception>
#include <stdexcept>
#include <string_view>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/RootTask.h>
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
        stopRegistration = loop.onStop([this]() -> ruvia::Task<void> {
            stopSource.requestStop();
            for (const auto& client : clients) {
                client.client->close();
            }
            this->database.close();
            for (const auto& client : clients)
                co_await client.client->shutdown();
            co_await this->database.shutdown();
        });
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

std::pmr::memory_resource* WorkerContext::pool() const noexcept {
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

    ruvia::Task<void> runTask(std::size_t index) {
        try {
            co_await workers[index].run(*contexts[index]);
            if (!contexts[index]->stopToken().stopRequested()) {
                service::logging::error("Background worker " + workers[index].name +
                                        " stopped unexpectedly");
                ruvia::app().stop();
            }
        } catch (const std::exception& error) {
            if (contexts[index]->stopToken().stopRequested())
                co_return;
            service::logging::error("Background worker " + workers[index].name +
                                    " failed: " + error.what());
            ruvia::app().stop();
            throw;
        } catch (...) {
            if (contexts[index]->stopToken().stopRequested())
                co_return;
            service::logging::error("Background worker " + workers[index].name +
                                    " failed with an unknown exception");
            ruvia::app().stop();
            throw;
        }
    }

    void startTask(std::size_t index) {
        tasks.push_back(loops.loop(index).start(runTask(index)));
    }

    void waitTasks() noexcept {
        for (std::size_t index = 0; index < tasks.size(); ++index) {
            try {
                tasks[index].get();
                if (!contexts[index]->stopToken().stopRequested()) {
                    service::logging::error("Background worker " + workers[index].name +
                                            " stopped unexpectedly");
                    ruvia::app().stop();
                }
            } catch (...) {
                // runTask() has already reported the failure and requested shutdown.
                ruvia::app().stop();
            }
        }
        tasks.clear();
    }

    void waitConnections() noexcept {
        for (auto& connection : connectionTasks) {
            if (!connection.valid())
                continue;
            try {
                connection.get();
            } catch (const std::exception& error) {
                service::logging::error("Background database connection shutdown failed: " +
                                        std::string(error.what()));
            } catch (...) {
                service::logging::error("Background database connection shutdown failed");
            }
        }
        connectionTasks.clear();
    }

    void start() {
        if (started.exchange(true)) {
            throw std::logic_error("background worker pool already started");
        }

        try {
            loops.start();
            connectionTasks.reserve(contexts.size());
            for (std::size_t index = 0; index < contexts.size(); ++index)
                connectionTasks.push_back(loops.loop(index).start(contexts[index]->db().connect()));
            for (auto& connection : connectionTasks)
                connection.get();
            tasks.reserve(workers.size());
            for (std::size_t index = 0; index < workers.size(); ++index) {
                startTask(index);
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
        waitTasks();
        waitConnections();
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
    std::vector<ruvia::RootTask<void>> connectionTasks;
    std::vector<ruvia::RootTask<void>> tasks;
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
