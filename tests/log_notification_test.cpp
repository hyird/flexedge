#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <ruvia/core/Channel.h>
#include "node/runtime/log_buffer.h"
#include "node/runtime/loop_shutdown.h"

#define REQUIRE(condition) do { if (!(condition)) \
    throw std::runtime_error("requirement failed: " #condition); } while (false)

int main() {
    ruvia::EventLoopPool loops({.loopCount = 1});
    ruvia::EventLoopPool unused({.loopCount = 1});
    const auto loop = loops.loop(0);
    auto [sender, receiver] = ruvia::makeChannel<bool>(loop.handle(), {.capacity = 1});
    flexedge::node::NodeLogBuffer logs(0, [sender] { (void)sender.send(true); });
    flexedge::node::LoopShutdown shutdown(loops, unused);
    loops.start();

    // A notification published before the consumer starts waiting is retained.
    REQUIRE(!logs.pending());
    REQUIRE(logs.node("info", "test", "before wait"));
    REQUIRE(loop.start(receiver.receiveFor(std::chrono::seconds(1))).get().hasValue());
    auto batch = logs.take("node");
    REQUIRE(batch.has_value());
    logs.acknowledge(std::move(*batch));
    REQUIRE(!logs.pending());

    // Cross-thread publication wakes the waiter; repeated signals coalesce,
    // while all queue entries remain available for acknowledgement.
    auto waiting = loop.start(receiver.receiveFor(std::chrono::seconds(1)));
    std::jthread producer([&] {
        for (int index = 0; index < 10; ++index)
            (void)logs.node("info", "test", "concurrent");
    });
    producer.join();
    REQUIRE(waiting.get().hasValue());
    REQUIRE(logs.queuedEvents() == 10);
    batch = logs.take("node");
    REQUIRE(batch.has_value());
    logs.restore(std::move(*batch));
    REQUIRE(logs.pending());
    batch = logs.take("node");
    REQUIRE(batch.has_value());
    logs.acknowledge(std::move(*batch));
    REQUIRE(!logs.pending());

    // Consume any coalesced wake left by the producer, then cancel idle wait.
    (void)loop.start(receiver.receiveFor(std::chrono::milliseconds(10))).get();
    ruvia::StopSource stop;
    auto cancelled = loop.start(receiver.receive(stop.token()));
    stop.requestStop();
    REQUIRE(!cancelled.get().hasValue());
    shutdown.stopAndJoin();
}
