#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "service/features/log_ingest/fanout.h"

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

namespace {

using subscription = service::log_ingest::fanout::hub::subscription;
using notification = service::log_ingest::notifications::notification;
using log_resource_type = service::log_ingest::notifications::log_resource_type;

ruvia::task<ruvia::worker_wait_result<std::uint64_t>> receive(subscription& subscription,
    std::chrono::milliseconds timeout) {
    co_return co_await subscription.receive_for(timeout, {});
}

}  // namespace

int main() {
    try {
        ruvia::event_loop_pool loops({.loop_count_ = 2});
        const auto access_loop = loops.loop(0);
        const auto peer_loop = loops.loop(1);
        const auto access_worker = access_loop.handle().id();
        const auto peer_worker = peer_loop.handle().id();
        service::log_ingest::fanout::hub fanout;
        REQUIRE(service::log_ingest::notifications::resource_type_name(log_resource_type::access) ==
                "access");
        REQUIRE(service::log_ingest::notifications::resource_type_name(log_resource_type::node) ==
                "node");
        REQUIRE(service::log_ingest::notifications::parse_resource_type("invalid") == std::nullopt);

        // Same-type workers subscribe to the same topic; each reader owns its own signal.
        auto access = fanout.subscribe(access_loop.handle(), log_resource_type::access,
            "tenant-a", "website-a");
        auto peer_access = fanout.subscribe(peer_loop.handle(), log_resource_type::access,
            "tenant-a", "website-a");
        auto other_tenant = fanout.subscribe(peer_loop.handle(), log_resource_type::access,
            "tenant-b", "website-a");
        auto other_topic = fanout.subscribe(access_loop.handle(), log_resource_type::access,
            "tenant-a", "website-b");
        auto node = fanout.subscribe(peer_loop.handle(), log_resource_type::node,
            "tenant-a", "node-a");
        loops.start();

        fanout.mark_ready();
        REQUIRE(access_loop.start(receive(access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(peer_loop.start(receive(other_tenant, std::chrono::seconds(1))).get().has_value());
        REQUIRE(access_loop.start(receive(other_topic, std::chrono::seconds(1))).get().has_value());
        REQUIRE(peer_loop.start(receive(node, std::chrono::seconds(1))).get().has_value());

        const notification wrong_topic{
            .id_ = "1-0",
            .tenant_id_ = "tenant-a",
            .resource_type_ = log_resource_type::access,
            .resource_id_ = "website-b",
        };
        fanout.publish(wrong_topic, access_worker);
        REQUIRE(access_loop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(access_loop.start(receive(other_topic, std::chrono::seconds(1))).get().has_value());

        const notification wrong_tenant{
            .id_ = "1-1",
            .tenant_id_ = "tenant-b",
            .resource_type_ = log_resource_type::access,
            .resource_id_ = "website-a",
        };
        fanout.publish(wrong_tenant, peer_worker);
        REQUIRE(access_loop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(peer_loop.start(receive(other_tenant, std::chrono::seconds(1))).get().has_value());

        const notification access_notification{
            .id_ = "2-0",
            .tenant_id_ = "tenant-a",
            .resource_type_ = log_resource_type::access,
            .resource_id_ = "website-a",
        };
        fanout.publish(access_notification, access_worker);
        fanout.publish(access_notification, access_worker);
        fanout.publish(access_notification, access_worker);
        REQUIRE(access_loop.start(receive(access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(access_loop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);

        // The peer reader independently sees its own publication of the same event once.
        fanout.publish(access_notification, peer_worker);
        fanout.publish(access_notification, peer_worker);
        fanout.publish(access_notification, peer_worker);
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(access_loop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);

        const notification node_notification{
            .id_ = "3-0",
            .tenant_id_ = "tenant-a",
            .resource_type_ = log_resource_type::node,
            .resource_id_ = "node-a",
        };
        fanout.publish(node_notification, access_worker);
        REQUIRE(peer_loop.start(receive(node, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        fanout.publish(node_notification, peer_worker);
        REQUIRE(peer_loop.start(receive(node, std::chrono::seconds(1))).get().has_value());

        // Closing a subscriber removes its receiver; later events reach only a new lease.
        const notification closing_notification{
            .id_ = "4-0",
            .tenant_id_ = "tenant-a",
            .resource_type_ = log_resource_type::access,
            .resource_id_ = "website-close",
        };
        {
            auto closing = fanout.subscribe(access_loop.handle(), log_resource_type::access,
                "tenant-a", "website-close");
            fanout.publish(closing_notification, access_worker);
            REQUIRE(access_loop.start(receive(closing, std::chrono::seconds(1))).get().has_value());
        }
        fanout.publish(closing_notification, access_worker);
        auto reopened = fanout.subscribe(access_loop.handle(), log_resource_type::access,
            "tenant-a", "website-close");
        REQUIRE(access_loop.start(receive(reopened, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        fanout.publish(closing_notification, access_worker);
        REQUIRE(access_loop.start(receive(reopened, std::chrono::seconds(1))).get().has_value());
        REQUIRE(access_loop.start(receive(reopened, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);

        // A publisher racing pool shutdown must retire worker-stopping receivers.
        loops.stop();
        fanout.publish(access_notification, access_worker);
        fanout.publish(access_notification, peer_worker);
        fanout.publish(node_notification, peer_worker);
        loops.join();
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
