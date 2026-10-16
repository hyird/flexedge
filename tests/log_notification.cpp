#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "node/runtime/log_producer.h"
#include "node/runtime/loop_shutdown.h"

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

namespace {

ruvia::task<void> publish(flexedge::node::log_producer& producer,
    std::string_view message, int events = 1) {
    for (int index = 0; index < events; ++index) {
        REQUIRE(producer.node("info", "test", message));
    }
    REQUIRE(producer.flush());
    REQUIRE(producer.queued_events() == 0);
    co_return;
}

ruvia::task<void> saturate(flexedge::node::log_producer& producer) {
    const auto dropped_before = producer.dropped_events();
    for (int index = 0; index < 32; ++index) {
        (void)producer.node("info", "test", "queue full");
        (void)producer.flush();
    }
    REQUIRE(producer.dropped_events() > dropped_before);
    co_return;
}

void require_batch(const flexedge::node::owned_log_batch& batch,
    std::string_view message, int events) {
    REQUIRE(batch != nullptr);
    REQUIRE(batch->value().events_size() == events);
    for (const auto& event : batch->value().events()) {
        REQUIRE(event.has_node_log());
        REQUIRE(event.node_log().message() == message);
        REQUIRE(!event.node_log().id().empty());
    }
}

}  // namespace

int main() {
    ruvia::event_loop_pool workers({.loop_count_ = 2});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    const auto control_loop = control.loop(0);
    auto [sender, receiver] = ruvia::make_channel<flexedge::node::owned_log_batch>(
        control_loop.handle(), {.capacity_ = 1});
    flexedge::node::log_producer first(workers.loop(0), sender);
    flexedge::node::log_producer second(workers.loop(1), sender);
    flexedge::node::loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();

    // Publication transfers the complete batch before the receiver starts waiting.
    workers.loop(0).start(publish(first, "before wait")).get();
    auto received = control_loop.start(receiver.receive_for(std::chrono::seconds(1))).get();
    REQUIRE(received.has_value());
    require_batch(received.value(), "before wait", 1);

    // An idle receiver wakes for another worker's immutable batch without a
    // callback into that producer or a second notification queue.
    auto waiting = control_loop.start(receiver.receive_for(std::chrono::seconds(1)));
    workers.loop(1).start(publish(second, "other worker", 10)).get();
    received = waiting.get();
    REQUIRE(received.has_value());
    require_batch(received.value(), "other worker", 10);
    REQUIRE(!control_loop.start(receiver.receive_for(std::chrono::milliseconds(30))).get().has_value());

    // A saturated receiver keeps the accepted batch intact. Rejected events
    // affect only their producer's local bounded accounting.
    workers.loop(0).start(publish(first, "retained batch")).get();
    workers.loop(1).start(saturate(second)).get();
    received = control_loop.start(receiver.receive_for(std::chrono::seconds(1))).get();
    REQUIRE(received.has_value());
    require_batch(received.value(), "retained batch", 1);
    workers.loop(1).start(publish(second, "after drain")).get();
    received = control_loop.start(receiver.receive_for(std::chrono::seconds(1))).get();
    REQUIRE(received.has_value());
    require_batch(received.value(), "after drain", 1);

    // Idle cancellation releases the waiter and does not leave a second receiver.
    ruvia::stop_source stop;
    auto cancelled = control_loop.start(receiver.receive(stop.token()));
    const auto cancelled_at = std::chrono::steady_clock::now();
    stop.request_stop();
    REQUIRE(cancelled.get().status() == ruvia::worker_wait_status::cancelled);
    REQUIRE(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(1));
    workers.loop(0).start(publish(first, "after cancellation")).get();
    received = control_loop.start(receiver.receive_for(std::chrono::seconds(1))).get();
    REQUIRE(received.has_value());
    require_batch(received.value(), "after cancellation", 1);

    // Closing admission allows accepted batches to drain and ends the idle wait.
    workers.loop(0).start(publish(first, "before close")).get();
    sender.close();
    received = control_loop.start(receiver.receive_for(std::chrono::seconds(1))).get();
    REQUIRE(received.has_value());
    require_batch(received.value(), "before close", 1);
    REQUIRE(control_loop.start(receiver.receive()).get().status() == ruvia::worker_wait_status::closed);
    shutdown.stop_and_join();
}
