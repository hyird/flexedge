#include <chrono>
#include <cstdint>
#include <optional>
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
using LogResourceType = service::log_ingest::notifications::LogResourceType;

ruvia::Task<ruvia::WorkerWaitResult<std::uint64_t>> receive(Subscription& subscription,
                                                            std::chrono::milliseconds timeout) {
    co_return co_await subscription.receiveFor(timeout, {});
}

} // namespace

int main() {
    try {
        ruvia::EventLoopPool loops({.loopCount = 2});
        const auto accessLoop = loops.loop(0);
        const auto peerLoop = loops.loop(1);
        const auto accessWorker = accessLoop.handle().id();
        const auto peerWorker = peerLoop.handle().id();
        service::log_ingest::fanout::Hub fanout;
        REQUIRE(service::log_ingest::notifications::resourceTypeName(LogResourceType::access) ==
                "access");
        REQUIRE(service::log_ingest::notifications::resourceTypeName(LogResourceType::node) ==
                "node");
        REQUIRE(service::log_ingest::notifications::parseResourceType("invalid") == std::nullopt);

        // Same-type workers subscribe to the same topic; each reader owns its own signal.
        auto access = fanout.subscribe(accessLoop.handle(), LogResourceType::access,
                                       "tenant-a", "website-a");
        auto peerAccess = fanout.subscribe(peerLoop.handle(), LogResourceType::access,
                                           "tenant-a", "website-a");
        auto otherTenant = fanout.subscribe(peerLoop.handle(), LogResourceType::access,
                                            "tenant-b", "website-a");
        auto otherTopic = fanout.subscribe(accessLoop.handle(), LogResourceType::access,
                                           "tenant-a", "website-b");
        auto node = fanout.subscribe(peerLoop.handle(), LogResourceType::node,
                                     "tenant-a", "node-a");
        loops.start();

        fanout.markReady();
        REQUIRE(accessLoop.start(receive(access, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(peerLoop.start(receive(peerAccess, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(peerLoop.start(receive(otherTenant, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(accessLoop.start(receive(otherTopic, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(peerLoop.start(receive(node, std::chrono::seconds(1))).get().hasValue());

        const Notification wrongTopic{
            .id = "1-0",
            .tenantId = "tenant-a",
            .resourceType = LogResourceType::access,
            .resourceId = "website-b",
        };
        fanout.publish(wrongTopic, accessWorker);
        REQUIRE(accessLoop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(peerLoop.start(receive(peerAccess, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(accessLoop.start(receive(otherTopic, std::chrono::seconds(1))).get().hasValue());

        const Notification wrongTenant{
            .id = "1-1",
            .tenantId = "tenant-b",
            .resourceType = LogResourceType::access,
            .resourceId = "website-a",
        };
        fanout.publish(wrongTenant, peerWorker);
        REQUIRE(accessLoop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(peerLoop.start(receive(peerAccess, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(peerLoop.start(receive(otherTenant, std::chrono::seconds(1))).get().hasValue());

        const Notification accessNotification{
            .id = "2-0",
            .tenantId = "tenant-a",
            .resourceType = LogResourceType::access,
            .resourceId = "website-a",
        };
        fanout.publish(accessNotification, accessWorker);
        fanout.publish(accessNotification, accessWorker);
        fanout.publish(accessNotification, accessWorker);
        REQUIRE(accessLoop.start(receive(access, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(peerLoop.start(receive(peerAccess, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(accessLoop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);

        // The peer reader independently sees its own publication of the same event once.
        fanout.publish(accessNotification, peerWorker);
        fanout.publish(accessNotification, peerWorker);
        fanout.publish(accessNotification, peerWorker);
        REQUIRE(peerLoop.start(receive(peerAccess, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(accessLoop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(peerLoop.start(receive(peerAccess, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);

        const Notification nodeNotification{
            .id = "3-0",
            .tenantId = "tenant-a",
            .resourceType = LogResourceType::node,
            .resourceId = "node-a",
        };
        fanout.publish(nodeNotification, accessWorker);
        REQUIRE(peerLoop.start(receive(node, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        fanout.publish(nodeNotification, peerWorker);
        REQUIRE(peerLoop.start(receive(node, std::chrono::seconds(1))).get().hasValue());

        // Closing a subscriber removes its receiver; later events reach only a new lease.
        const Notification closingNotification{
            .id = "4-0",
            .tenantId = "tenant-a",
            .resourceType = LogResourceType::access,
            .resourceId = "website-close",
        };
        {
            auto closing = fanout.subscribe(accessLoop.handle(), LogResourceType::access,
                                             "tenant-a", "website-close");
            fanout.publish(closingNotification, accessWorker);
            REQUIRE(accessLoop.start(receive(closing, std::chrono::seconds(1))).get().hasValue());
        }
        fanout.publish(closingNotification, accessWorker);
        auto reopened = fanout.subscribe(accessLoop.handle(), LogResourceType::access,
                                         "tenant-a", "website-close");
        REQUIRE(accessLoop.start(receive(reopened, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        fanout.publish(closingNotification, accessWorker);
        REQUIRE(accessLoop.start(receive(reopened, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(accessLoop.start(receive(reopened, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);

        // A publisher racing pool shutdown must retire worker-stopping receivers.
        loops.stop();
        fanout.publish(accessNotification, accessWorker);
        fanout.publish(accessNotification, peerWorker);
        fanout.publish(nodeNotification, peerWorker);
        loops.join();
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
