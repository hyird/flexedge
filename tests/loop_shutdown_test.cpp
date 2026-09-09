#include <atomic>
#include <stdexcept>
#include <string_view>
#include "node/runtime/loop_shutdown.h"

#define REQUIRE(condition) do { if (!(condition)) \
    throw std::runtime_error("requirement failed: " #condition); } while (false)

int main() {
    {
        ruvia::EventLoopPool workers({.loopCount = 1});
        ruvia::EventLoopPool control({.loopCount = 1});
        std::atomic<bool> workersStopped{}, controlStopped{};
        auto first = workers.loop(0).onStop([&] { workersStopped = true; });
        auto second = control.loop(0).onStop([&] { controlStopped = true; });
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
        auto first = workers.loop(0).onStop([] { throw std::runtime_error("worker cleanup"); });
        auto second = control.loop(0).onStop([&] { controlStopped = true; });
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
}
