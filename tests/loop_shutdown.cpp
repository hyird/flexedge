#include "node/runtime/loop_shutdown.h"

#include <ruvia/core/event_loop.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/core/worker_signal.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <asio/steady_timer.hpp>

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

int main() {
    {
        ruvia::event_loop_pool workers({.loop_count_ = 1});
        ruvia::event_loop_pool control({.loop_count_ = 1});
        std::atomic<bool> workers_stopped{}, control_stopped{};
        auto first = workers.loop(0).on_stop([&]() -> ruvia::task<void> {
            workers_stopped = true;
            co_return;
        });
        auto second = control.loop(0).on_stop([&]() -> ruvia::task<void> {
            control_stopped = true;
            co_return;
        });
        bool original_preserved = false;
        try {
            flexedge::node::loop_shutdown shutdown(workers, control);
            workers.start();
            control.start();
            throw std::runtime_error("startup failure");
        } catch (const std::runtime_error& error) {
            original_preserved = std::string_view(error.what()) == "startup failure";
        }
        REQUIRE(original_preserved);
        REQUIRE(workers_stopped && control_stopped);
    }
    {
        ruvia::event_loop_pool workers({.loop_count_ = 1});
        ruvia::event_loop_pool control({.loop_count_ = 1});
        std::atomic<bool> control_stopped{};
        auto first = workers.loop(0).on_stop([]() -> ruvia::task<void> {
            throw std::runtime_error("worker cleanup");
            co_return;
        });
        auto second = control.loop(0).on_stop([&]() -> ruvia::task<void> {
            control_stopped = true;
            co_return;
        });
        flexedge::node::loop_shutdown shutdown(workers, control);
        workers.start();
        control.start();
        bool reported = false;
        try {
            shutdown.stop_and_join();
        } catch (const std::runtime_error& error) {
            reported = std::string_view(error.what()) == "worker cleanup";
        }
        REQUIRE(reported);
        REQUIRE(control_stopped);
        shutdown.stop_and_join();
    }
    {
        ruvia::event_loop_pool workers({.loop_count_ = 1});
        ruvia::event_loop_pool control({.loop_count_ = 1});
        auto loop = workers.loop(0);
        const auto worker = loop.handle();
        // Declared before loop_shutdown so the signal outlives the joined loops.
        ruvia::worker_signal signal(worker);
        auto timer = std::make_shared<asio::steady_timer>(loop.io_context());
        std::atomic<bool> operation_started{};
        std::atomic<bool> operation_retired{};
        std::atomic<bool> operation_cancelled{};
        auto cleanup = loop.on_stop([&signal, timer]() -> ruvia::task<void> {
            std::error_code ignored;
            timer->cancel(ignored);
            co_await signal.wait();
        });
        flexedge::node::loop_shutdown shutdown(workers, control);
        workers.start();
        control.start();
        REQUIRE(loop.post([timer, &signal, &operation_started, &operation_retired,
                              &operation_cancelled] {
                        timer->expires_after(std::chrono::hours(1));
                        // The timer completes on the loop thread, which owns the signal.
                        timer->async_wait([&signal, &operation_retired, &operation_cancelled](
                                              const std::error_code& error) {
                            operation_cancelled = error == asio::error::operation_aborted;
                            operation_retired = true;
                            signal.notify();
                        });
                        operation_started = true;
                    })
                .accepted());
        while (!operation_started) {
            std::this_thread::yield();
        }
        shutdown.stop_and_join();
        REQUIRE(operation_retired);
        REQUIRE(operation_cancelled);
    }
    {
        ruvia::event_loop_pool workers({.loop_count_ = 1});
        ruvia::event_loop_pool control({.loop_count_ = 1});
        const auto loop = workers.loop(0);
        const auto worker = loop.handle();
        ruvia::worker_signal signal(worker);
        std::atomic<bool> cleanup_finished{};
        auto cleanup = loop.on_stop([&signal, &cleanup_finished]() -> ruvia::task<void> {
            co_await signal.wait();
            cleanup_finished = true;
        });
        flexedge::node::loop_shutdown shutdown(workers, control);
        workers.start();
        control.start();
        std::atomic<bool> deferred{};
        // A foreign thread wakes the stopping loop through its cleanup admission.
        std::jthread completion([loop, &signal, &deferred] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            deferred = loop.defer_cleanup([&signal] { signal.notify(); });
        });
        const auto started = std::chrono::steady_clock::now();
        shutdown.stop_and_join();
        completion.join();
        REQUIRE(deferred);
        REQUIRE(cleanup_finished);
        REQUIRE(std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(15));
    }
}
