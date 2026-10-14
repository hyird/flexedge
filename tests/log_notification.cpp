#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "node/runtime/log_buffer.h"
#include "node/runtime/loop_shutdown.h"

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

int main() {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    ruvia::event_loop_pool unused({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    auto [sender, receiver] = ruvia::make_channel<bool>(loop.handle(), {.capacity_ = 1});
    flexedge::node::node_log_buffer logs(0, [sender] { (void)sender.send(true); });
    flexedge::node::loop_shutdown shutdown(loops, unused);
    loops.start();

    // A notification published before the consumer starts waiting is retained.
    REQUIRE(!logs.pending());
    REQUIRE(logs.node("info", "test", "before wait"));
    REQUIRE(loop.start(receiver.receive_for(std::chrono::seconds(1))).get().has_value());
    auto batch = logs.take("node");
    REQUIRE(batch.has_value());
    logs.acknowledge(std::move(*batch));
    REQUIRE(!logs.pending());

    // Cross-thread publication wakes the waiter; repeated signals coalesce,
    // while all queue entries remain available for acknowledgement.
    auto waiting = loop.start(receiver.receive_for(std::chrono::seconds(1)));
    std::jthread producer([&] {
        for (int index = 0; index < 10; ++index) {
            (void)logs.node("info", "test", "concurrent");
        }
    });
    producer.join();
    REQUIRE(waiting.get().has_value());
    REQUIRE(logs.queued_events() == 10);
    batch = logs.take("node");
    REQUIRE(batch.has_value());
    logs.restore(std::move(*batch));
    REQUIRE(logs.pending());
    batch = logs.take("node");
    REQUIRE(batch.has_value());
    logs.acknowledge(std::move(*batch));
    REQUIRE(!logs.pending());

    // Consume any coalesced wake left by the producer, then cancel idle wait.
    (void)loop.start(receiver.receive_for(std::chrono::milliseconds(10))).get();
    ruvia::stop_source stop;
    auto cancelled = loop.start(receiver.receive(stop.token()));
    stop.request_stop();
    REQUIRE(!cancelled.get().has_value());
    shutdown.stop_and_join();
}
