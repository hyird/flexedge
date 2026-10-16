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
        // Every Web Worker owns its hub; its reader delivers only to it.
        service::log_ingest::fanout::hub access_fanout;
        service::log_ingest::fanout::hub peer_fanout;
        REQUIRE(service::log_ingest::notifications::resource_type_name(log_resource_type::access) == "access");
        REQUIRE(service::log_ingest::notifications::resource_type_name(log_resource_type::node) == "node");
        REQUIRE(service::log_ingest::notifications::parse_resource_type("invalid") == std::nullopt);

        auto access = access_fanout.subscribe(access_loop.handle(), log_resource_type::access, "tenant-a", "website-a");
        auto peer_access = peer_fanout.subscribe(peer_loop.handle(), log_resource_type::access, "tenant-a", "website-a");
        auto other_tenant = peer_fanout.subscribe(peer_loop.handle(), log_resource_type::access, "tenant-b", "website-a");
        auto other_topic = access_fanout.subscribe(access_loop.handle(), log_resource_type::access, "tenant-a", "website-b");
        auto node = peer_fanout.subscribe(peer_loop.handle(), log_resource_type::node, "tenant-a", "node-a");
        loops.start();

        access_fanout.mark_ready();
        peer_fanout.mark_ready();
        REQUIRE(access_loop.start(receive(access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(peer_loop.start(receive(other_tenant, std::chrono::seconds(1))).get().has_value());
        REQUIRE(access_loop.start(receive(other_topic, std::chrono::seconds(1))).get().has_value());
        REQUIRE(peer_loop.start(receive(node, std::chrono::seconds(1))).get().has_value());

        const notification wrong_topic{
            .id_ = "1-0", .tenant_id_ = "tenant-a", .resource_type_ = log_resource_type::access, .resource_id_ = "website-b"};
        access_fanout.publish(wrong_topic);
        REQUIRE(access_loop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(access_loop.start(receive(other_topic, std::chrono::seconds(1))).get().has_value());

        const notification wrong_tenant{
            .id_ = "1-1", .tenant_id_ = "tenant-b", .resource_type_ = log_resource_type::access, .resource_id_ = "website-a"};
        peer_fanout.publish(wrong_tenant);
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(peer_loop.start(receive(other_tenant, std::chrono::seconds(1))).get().has_value());

        // A worker's delivery reaches only its own subscriptions, once per burst.
        const notification access_notification{
            .id_ = "2-0", .tenant_id_ = "tenant-a", .resource_type_ = log_resource_type::access, .resource_id_ = "website-a"};
        access_fanout.publish(access_notification);
        access_fanout.publish(access_notification);
        access_fanout.publish(access_notification);
        REQUIRE(access_loop.start(receive(access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(access_loop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        peer_fanout.publish(access_notification);
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(access_loop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);

        const notification node_notification{
            .id_ = "3-0", .tenant_id_ = "tenant-a", .resource_type_ = log_resource_type::node, .resource_id_ = "node-a"};
        access_fanout.publish(node_notification);
        REQUIRE(peer_loop.start(receive(node, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        peer_fanout.publish(node_notification);
        REQUIRE(peer_loop.start(receive(node, std::chrono::seconds(1))).get().has_value());

        // Reconnection wakes every tail owned by this reader once, allowing
        // each tail to refill missing rows from its own database cursor.
        access_fanout.recover();
        access_fanout.recover();
        REQUIRE(access_loop.start(receive(access, std::chrono::seconds(1))).get().has_value());
        REQUIRE(access_loop.start(receive(other_topic, std::chrono::seconds(1))).get().has_value());
        REQUIRE(access_loop.start(receive(access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(peer_loop.start(receive(peer_access, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);

        // Closing a subscriber removes its receiver; later events reach only a new lease.
        const notification closing_notification{.id_ = "4-0",
            .tenant_id_ = "tenant-a",
            .resource_type_ = log_resource_type::access,
            .resource_id_ = "website-close"};
        {
            auto closing =
                access_fanout.subscribe(access_loop.handle(), log_resource_type::access, "tenant-a", "website-close");
            access_fanout.publish(closing_notification);
            REQUIRE(access_loop.start(receive(closing, std::chrono::seconds(1))).get().has_value());
        }
        access_fanout.publish(closing_notification);
        auto reopened =
            access_fanout.subscribe(access_loop.handle(), log_resource_type::access, "tenant-a", "website-close");
        REQUIRE(access_loop.start(receive(reopened, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        access_fanout.publish(closing_notification);
        REQUIRE(access_loop.start(receive(reopened, std::chrono::seconds(1))).get().has_value());

        // A delivery racing pool shutdown must retire worker-stopping receivers.
        loops.stop();
        access_fanout.publish(access_notification);
        peer_fanout.publish(node_notification);
        loops.join();
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
