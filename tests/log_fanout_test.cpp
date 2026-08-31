#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <ruvia/core/EventLoopPool.h>

#include "service/features/log_ingest/fanout.h"

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error("requirement failed: " #condition);                           \
    } while (false)

namespace {

using Subscription = service::log_ingest::fanout::Hub::Subscription;
using Notification = service::log_ingest::notifications::Notification;

ruvia::Task<ruvia::WorkerWaitResult<std::uint64_t>> receive(Subscription& subscription,
                                                            std::chrono::milliseconds timeout) {
    co_return co_await subscription.receiveFor(timeout, {});
}

} // namespace

int main() {
    try {
        ruvia::EventLoopPool loops({.loopCount = 2});
        const auto accessLoop = loops.loop(0);
        const auto nodeLoop = loops.loop(1);
        service::log_ingest::fanout::Hub fanout;
        auto access = fanout.subscribeAccess(accessLoop.handle(), "tenant-a", "website-a");
        auto node = fanout.subscribeNode(nodeLoop.handle(), "tenant-a", "node-a");
        loops.start();

        fanout.markReady();
        const auto accessReady = accessLoop.start(receive(access, std::chrono::seconds(1))).get();
        REQUIRE(accessReady.hasValue());
        const auto nodeReady = nodeLoop.start(receive(node, std::chrono::seconds(1))).get();
        REQUIRE(nodeReady.hasValue());

        fanout.publish({.id = "1-0",
                        .tenantId = "tenant-a",
                        .kind = std::string(service::log_ingest::notifications::kKindAccess),
                        .websiteId = "website-b",
                        .nodeId = {}});
        const auto unrelated =
            accessLoop.start(receive(access, std::chrono::milliseconds(20))).get();
        REQUIRE(unrelated.status() == ruvia::WorkerWaitStatus::kTimedOut);

        const Notification accessNotification{
            .id = "2-0",
            .tenantId = "tenant-a",
            .kind = std::string(service::log_ingest::notifications::kKindAccess),
            .websiteId = "website-a",
            .nodeId = {},
        };
        fanout.publish(accessNotification);
        fanout.publish(accessNotification);
        fanout.publish(accessNotification);
        const auto accessSignal = accessLoop.start(receive(access, std::chrono::seconds(1))).get();
        REQUIRE(accessSignal.hasValue());
        const auto coalesced =
            accessLoop.start(receive(access, std::chrono::milliseconds(20))).get();
        REQUIRE(coalesced.status() == ruvia::WorkerWaitStatus::kTimedOut);

        fanout.publish({.id = "3-0",
                        .tenantId = "tenant-a",
                        .kind = std::string(service::log_ingest::notifications::kKindNode),
                        .websiteId = {},
                        .nodeId = "node-a"});
        const auto nodeSignal = nodeLoop.start(receive(node, std::chrono::seconds(1))).get();
        REQUIRE(nodeSignal.hasValue());

        loops.stop();
        loops.join();
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
