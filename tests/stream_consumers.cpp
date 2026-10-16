#include "service/features/background/stream_consumers.h"

#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/web/redis/redis_client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/utils/redis_reply.h"
#include "support/redis_test.h"

namespace {

using namespace std::chrono_literals;
namespace consumers = service::background::stream_consumers;
using service::utils::require_redis_success;

constexpr std::string_view group{"consumer-cleanup"};
constexpr std::string_view active_stream{"flexedge:test:stream-consumers:active"};
constexpr std::string_view pending_stream{"flexedge:test:stream-consumers:pending"};
constexpr std::string_view racing_stream{"flexedge:test:stream-consumers:racing"};
constexpr std::string_view batch_stream{"flexedge:test:stream-consumers:batch"};
constexpr std::string_view missing_stream{"flexedge:test:stream-consumers:missing"};
constexpr std::string_view groupless_stream{"flexedge:test:stream-consumers:groupless"};
constexpr std::string_view wrong_type_key{"flexedge:test:stream-consumers:wrong-type"};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string("stream consumer cleanup: ") + message);
    }
}

ruvia::task<void> clean(ruvia::redis_handle redis) {
    const auto reply = co_await redis.command("DEL", active_stream, pending_stream, racing_stream, batch_stream,
        missing_stream, groupless_stream, wrong_type_key);
    require_redis_success(reply, "remove isolated consumer fixtures");
}

ruvia::task<void> create_group(ruvia::redis_handle redis, std::string_view stream) {
    const auto reply = co_await redis.command("XGROUP", "CREATE", stream, group, "0", "MKSTREAM");
    require_redis_success(reply, "create isolated consumer group");
}

ruvia::task<void> create_consumer(ruvia::redis_handle redis, std::string_view stream,
    std::string_view consumer) {
    const auto reply = co_await redis.command("XGROUP", "CREATECONSUMER", stream, group, consumer);
    require_redis_success(reply, "create isolated consumer");
    require(reply.integer() == 1, "fixture consumer must be newly created");
}

ruvia::task<std::vector<std::string>> names(ruvia::redis_handle redis, std::string_view stream) {
    const auto reply = co_await redis.command("XINFO", "CONSUMERS", stream, group);
    require_redis_success(reply, "inspect isolated consumers");
    std::vector<std::string> result;
    for (const auto& row : reply.array()) {
        const auto fields = row.array();
        bool found{};
        for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
            if (fields[index].string() == "name") {
                result.emplace_back(fields[index + 1].string());
                found = true;
                break;
            }
        }
        require(found, "XINFO consumer row includes its name");
    }
    std::ranges::sort(result);
    co_return result;
}

ruvia::task<void> verify_active_and_empty(ruvia::redis_handle redis) {
    co_await create_group(redis, active_stream);
    co_await create_consumer(redis, active_stream, "current");
    co_await create_consumer(redis, active_stream, "new-active");
    require(co_await consumers::remove_idle(redis, active_stream, group, "current") == 0,
        "the default idle threshold preserves newly active consumers");
    const auto active = co_await names(redis, active_stream);
    require(active.size() == 2 && active[0] == "current" && active[1] == "new-active",
        "current and newly active consumers remain registered");

    // A zero threshold makes the empty fixture eligible without a five-minute sleep.
    require(co_await consumers::remove_idle(redis, active_stream, group, "current", 0ms) == 1,
        "an eligible empty consumer is removed");
    const auto retained = co_await names(redis, active_stream);
    require(retained.size() == 1 && retained[0] == "current",
        "idle cleanup never removes its current consumer");
    require(co_await consumers::remove_idle(redis, active_stream, group, "current", 0ms) == 0,
        "repeating idle cleanup is idempotent");
    require(!(co_await consumers::remove_empty(redis, active_stream, group, "absent")),
        "a missing consumer is an idempotent no-op");
    require(co_await consumers::remove_empty(redis, active_stream, group, "current"),
        "drained cleanup removes an existing empty current consumer");
    require(!(co_await consumers::remove_empty(redis, active_stream, group, "current")),
        "repeating drained cleanup reports no deletion");
    require((co_await names(redis, active_stream)).empty(), "empty group retains no consumers");
}

ruvia::task<void> verify_pending_delivery(ruvia::redis_handle redis) {
    co_await create_group(redis, pending_stream);
    co_await create_consumer(redis, pending_stream, "current");
    const auto appended = co_await redis.command("XADD", pending_stream, "*", "payload", "durable");
    require_redis_success(appended, "append pending fixture");
    const std::string id(appended.string());
    const auto delivery = co_await redis.command("XREADGROUP", "GROUP", group, "abandoned",
        "COUNT", "1", "STREAMS", pending_stream, ">");
    require_redis_success(delivery, "read pending fixture");
    require(delivery.array().size() == 1, "fixture enters the pending list");
    const auto no_new_delivery = co_await redis.command("XREADGROUP", "GROUP", group, "abandoned",
        "COUNT", "1", "STREAMS", pending_stream, ">");
    require_redis_success(no_new_delivery, "drain available fixture messages");
    require(no_new_delivery.null(), "no new delivery is available while a retry remains pending");
    require(!(co_await consumers::remove_empty(redis, pending_stream, group, "abandoned")),
        "draining new deliveries does not discard a consumer's pending retry");
    require(co_await consumers::remove_idle(redis, pending_stream, group, "current", 0ms) == 0,
        "idle cleanup preserves pending ownership regardless of age");
    const auto retained = co_await names(redis, pending_stream);
    require(retained.size() == 2 && retained[0] == "abandoned" && retained[1] == "current",
        "pending consumer remains registered");

    const auto claimed = co_await redis.command("XCLAIM", pending_stream, group, "current", "0", id);
    require_redis_success(claimed, "recover retained pending delivery");
    const auto rows = claimed.array();
    require(rows.size() == 1 && rows.front().array().size() == 2,
        "pending message remains claimable after cleanup");
    const auto row = rows.front().array();
    require(row[0].string() == id && row[1].array().size() == 2 &&
                row[1].array()[0].string() == "payload" && row[1].array()[1].string() == "durable",
        "recovery retains the original message identifier and payload");
    const auto pending = co_await redis.command("XPENDING", pending_stream, group, id, id, "1");
    require_redis_success(pending, "inspect recovered ownership");
    require(pending.array().size() == 1 && pending.array()[0].array()[1].string() == "current",
        "recovery transfers the pending entry to the surviving consumer");
    require(co_await consumers::remove_empty(redis, pending_stream, group, "abandoned"),
        "the former owner becomes removable only after recovery");
    require(co_await consumers::remove_idle(redis, pending_stream, group, "other", 0ms) == 0,
        "a pending current owner remains protected even when not the cleanup caller");
    const auto acknowledged = co_await redis.command("XACK", pending_stream, group, id);
    require_redis_success(acknowledged, "acknowledge recovered delivery");
    require(acknowledged.integer() == 1, "the recovered message can still be acknowledged");
    require(co_await consumers::remove_empty(redis, pending_stream, group, "current"),
        "acknowledgement makes the final consumer removable");
    const auto after_ack = co_await redis.command("XPENDING", pending_stream, group);
    require_redis_success(after_ack, "inspect acknowledged group");
    require(after_ack.array()[0].integer() == 0, "cleanup leaves no orphan pending entries");
    require((co_await redis.command("XLEN", pending_stream)).integer() == 1,
        "consumer cleanup never deletes stream payloads");
}

ruvia::task<void> verify_batch_limit(ruvia::redis_handle redis) {
    co_await create_group(redis, batch_stream);
    co_await create_consumer(redis, batch_stream, "current");
    for (std::size_t index = 0; index < 130; ++index) {
        co_await create_consumer(redis, batch_stream, "empty-" + std::to_string(index));
    }
    for (const auto expected : std::array<std::size_t, 3>{64, 64, 2}) {
        const auto before = co_await names(redis, batch_stream);
        require(co_await consumers::remove_idle(redis, batch_stream, group, "current", 0ms) == expected,
            "one idle-cleanup invocation deletes at most 64 consumers");
        const auto after = co_await names(redis, batch_stream);
        require(before.size() - after.size() == expected &&
                    std::ranges::find(after, "current") != after.end(),
            "batch accounting is exact and preserves the cleanup caller");
    }
    require(co_await consumers::remove_idle(redis, batch_stream, group, "current", 0ms) == 0,
        "draining all eligible batches is idempotent");
    const auto remaining = co_await names(redis, batch_stream);
    require(remaining.size() == 1 && remaining[0] == "current",
        "only the excluded current consumer remains after all batches");
}

ruvia::task<void> verify_missing_and_wrong_types(ruvia::redis_handle redis) {
    require(co_await consumers::remove_idle(redis, missing_stream, group, "current", 0ms) == 0,
        "a missing stream needs no idle cleanup");
    require(!(co_await consumers::remove_empty(redis, missing_stream, group, "current")),
        "a missing stream needs no drained cleanup");
    require((co_await redis.command("EXISTS", missing_stream)).integer() == 0,
        "cleanup does not create a missing stream");
    const auto appended = co_await redis.command("XADD", groupless_stream, "*", "payload", "retained");
    require_redis_success(appended, "create groupless stream fixture");
    require(co_await consumers::remove_idle(redis, groupless_stream, group, "current", 0ms) == 0,
        "a missing group needs no idle cleanup");
    require(!(co_await consumers::remove_empty(redis, groupless_stream, group, "current")),
        "a missing group needs no drained cleanup");
    const auto groups = co_await redis.command("XINFO", "GROUPS", groupless_stream);
    require_redis_success(groups, "inspect groupless fixture");
    require(groups.array().empty() && (co_await redis.command("XLEN", groupless_stream)).integer() == 1,
        "cleanup neither creates groups nor changes an existing stream");

    const auto stored = co_await redis.command("SET", wrong_type_key, "unchanged");
    require_redis_success(stored, "create wrong-type fixture");
    bool idle_failed{};
    try {
        (void)co_await consumers::remove_idle(redis, wrong_type_key, group, "current", 0ms);
    } catch (const std::exception&) {
        idle_failed = true;
    }
    require(idle_failed, "idle cleanup propagates a wrong key type instead of treating it as missing");
    bool empty_failed{};
    try {
        (void)co_await consumers::remove_empty(redis, wrong_type_key, group, "current");
    } catch (const std::exception&) {
        empty_failed = true;
    }
    require(empty_failed, "drained cleanup propagates a wrong key type");
    const auto value = co_await redis.command("GET", wrong_type_key);
    require_redis_success(value, "inspect wrong-type fixture");
    require(value.string() == "unchanged", "failed cleanup preserves the original wrong-type key");
}

ruvia::task<void> read_during_cleanup(ruvia::redis_handle redis, std::string_view id) {
    const auto result = co_await redis.command("XREADGROUP", "GROUP", group, "racing",
        "COUNT", "1", "STREAMS", racing_stream, ">");
    require_redis_success(result, "read concurrently with consumer cleanup");
    require(result.array().size() == 1, "concurrent cleanup cannot discard a new delivery");
    const auto stream = result.array()[0].array();
    require(stream.size() == 2 && stream[1].array().size() == 1,
        "concurrent read returns the one available entry");
    const auto entry = stream[1].array()[0].array();
    require(entry.size() == 2 && entry[0].string() == id && entry[1].array().size() == 2 &&
                entry[1].array()[1].string() == "race-payload",
        "concurrent cleanup preserves the identifier and payload");
}

ruvia::task<void> remove_during_read(ruvia::redis_handle redis) {
    (void)co_await consumers::remove_empty(redis, racing_stream, group, "racing");
}

ruvia::task<void> verify_concurrent_delivery(ruvia::redis_handle redis, ruvia::redis_handle peer,
    const ruvia::worker_handle& worker) {
    co_await create_group(redis, racing_stream);
    for (std::size_t iteration = 0; iteration < 32; ++iteration) {
        co_await create_consumer(redis, racing_stream, "racing");
        const auto appended = co_await redis.command("XADD", racing_stream, "*", "payload", "race-payload");
        require_redis_success(appended, "append concurrent fixture");
        const std::string id(appended.string());
        ruvia::task_scope tasks(worker);
        if (iteration % 2 == 0) {
            tasks.spawn(remove_during_read(redis));
            tasks.spawn(read_during_cleanup(peer, id));
        } else {
            tasks.spawn(read_during_cleanup(peer, id));
            tasks.spawn(remove_during_read(redis));
        }
        co_await tasks.join();
        const auto pending = co_await redis.command("XPENDING", racing_stream, group, id, id, "1");
        require_redis_success(pending, "inspect concurrent delivery");
        require(pending.array().size() == 1 && pending.array()[0].array()[1].string() == "racing",
            "concurrent cleanup retains the pending entry and its consumer");
        const auto claimed = co_await redis.command("XCLAIM", racing_stream, group, "recovery", "0", id);
        require_redis_success(claimed, "recover concurrent delivery");
        require(claimed.array().size() == 1 && claimed.array()[0].array()[0].string() == id,
            "a concurrently retained pending entry remains claimable");
        const auto ack = co_await redis.command("XACK", racing_stream, group, id);
        require_redis_success(ack, "acknowledge concurrent delivery");
        require(ack.integer() == 1, "a concurrently retained pending entry remains acknowledgeable");
        require(co_await consumers::remove_empty(redis, racing_stream, group, "racing"),
            "the racing consumer becomes removable after recovery");
        require(co_await consumers::remove_empty(redis, racing_stream, group, "recovery"),
            "the recovery consumer becomes removable after acknowledgement");
    }
    const auto length = co_await redis.command("XLEN", racing_stream);
    require_redis_success(length, "inspect concurrent stream");
    require(length.integer() == 32, "concurrent consumer cleanup never deletes stream entries");
}

ruvia::task<void> verify(ruvia::redis_client& client, ruvia::redis_client& peer,
    const ruvia::worker_handle& worker) {
    const auto redis = client.handle();
    co_await clean(redis);
    co_await verify_active_and_empty(redis);
    co_await verify_pending_delivery(redis);
    co_await verify_concurrent_delivery(redis, peer.handle(), worker);
    co_await verify_batch_limit(redis);
    co_await verify_missing_and_wrong_types(redis);
    co_await clean(redis);
}

}  // namespace

int main() {
    try {
        const auto config = test_support::isolated_redis_config();
        if (!config) {
            return 77;
        }
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        const auto loop = loops.loop(0);
        const auto worker = loop.handle();
        ruvia::redis_client client(loop, *config);
        ruvia::redis_client peer(loop, *config);
        loops.start();
        int result{};
        try {
            loop.start(client.connect()).get();
            loop.start(peer.connect()).get();
            loop.start(verify(client, peer, worker)).get();
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            result = 1;
        }
        loop.start(client.shutdown()).get();
        loop.start(peer.shutdown()).get();
        loops.stop();
        loops.join();
        if (result != 0) {
            return result;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Stream consumers: idle and drained cleanup, pending recovery, concurrent delivery, batches, and errors passed\n";
}
