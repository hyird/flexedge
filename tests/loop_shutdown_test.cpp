#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <asio/steady_timer.hpp>
#include <ruvia/core/WorkerNotification.h>
#include "node/runtime/loop_shutdown.h"

#define REQUIRE(condition) do { if (!(condition)) \
    throw std::runtime_error("requirement failed: " #condition); } while (false)

int main() {
    {
        ruvia::EventLoopPool workers({.loopCount = 1});
        ruvia::EventLoopPool control({.loopCount = 1});
        std::atomic<bool> workersStopped{}, controlStopped{};
        auto first = workers.loop(0).onStop([&]() -> ruvia::Task<void> {
            workersStopped = true;
            co_return;
        });
        auto second = control.loop(0).onStop([&]() -> ruvia::Task<void> {
            controlStopped = true;
            co_return;
        });
        bool originalPreserved = false;
        try {
            flexedge::node::LoopShutdown shutdown(workers, control);
            workers.start();
            control.start();
            throw std::runtime_error("startup failure");
        } catch (const std::runtime_error& error) {
            originalPreserved = std::string_view(error.what()) == "startup failure";
        }
        REQUIRE(originalPreserved);
        REQUIRE(workersStopped && controlStopped);
    }
    {
        ruvia::EventLoopPool workers({.loopCount = 1});
        ruvia::EventLoopPool control({.loopCount = 1});
        std::atomic<bool> controlStopped{};
        auto first = workers.loop(0).onStop([]() -> ruvia::Task<void> {
            throw std::runtime_error("worker cleanup");
            co_return;
        });
        auto second = control.loop(0).onStop([&]() -> ruvia::Task<void> {
            controlStopped = true;
            co_return;
        });
        flexedge::node::LoopShutdown shutdown(workers, control);
        workers.start();
        control.start();
        bool reported = false;
        try { shutdown.stopAndJoin(); }
        catch (const std::runtime_error& error) {
            reported = std::string_view(error.what()) == "worker cleanup";
        }
        REQUIRE(reported);
        REQUIRE(controlStopped);
        shutdown.stopAndJoin();
    }
    {
        ruvia::EventLoopPool workers({.loopCount = 1});
        ruvia::EventLoopPool control({.loopCount = 1});
        auto loop = workers.loop(0);
        auto notification = std::make_shared<ruvia::WorkerNotification>(loop);
        auto timer = std::make_shared<asio::steady_timer>(loop.ioContext());
        std::atomic<bool> operationStarted{};
        std::atomic<bool> operationRetired{};
        std::atomic<bool> operationCancelled{};
        auto cleanup = loop.onStop([notification, timer]() -> ruvia::Task<void> {
            std::error_code ignored;
            timer->cancel(ignored);
            co_await notification->wait();
            notification->close();
        });
        flexedge::node::LoopShutdown shutdown(workers, control);
        workers.start();
        control.start();
        REQUIRE(loop.post([timer, notification, &operationStarted, &operationRetired,
                           &operationCancelled] {
            timer->expires_after(std::chrono::hours(1));
            timer->async_wait([notification, &operationRetired, &operationCancelled](
                                  const std::error_code& error) {
                operationCancelled = error == asio::error::operation_aborted;
                operationRetired = true;
                static_cast<void>(notification->notify());
            });
            operationStarted = true;
        }).accepted());
        while (!operationStarted) std::this_thread::yield();
        shutdown.stopAndJoin();
        REQUIRE(operationRetired);
        REQUIRE(operationCancelled);
    }
    {
        ruvia::EventLoopPool workers({.loopCount = 1});
        ruvia::EventLoopPool control({.loopCount = 1});
        auto notification = std::make_shared<ruvia::WorkerNotification>(workers.loop(0));
        std::atomic<bool> cleanupFinished{};
        auto cleanup = workers.loop(0).onStop(
            [notification, &cleanupFinished]() -> ruvia::Task<void> {
                co_await notification->wait();
                notification->close();
                cleanupFinished = true;
            });
        flexedge::node::LoopShutdown shutdown(workers, control);
        workers.start();
        control.start();
        std::jthread completion([notification] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            static_cast<void>(notification->notify());
        });
        const auto started = std::chrono::steady_clock::now();
        shutdown.stopAndJoin();
        completion.join();
        REQUIRE(cleanupFinished);
        REQUIRE(std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(15));
    }
}
