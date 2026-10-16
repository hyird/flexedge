#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis_client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/log_ingest/queue.h"
#include "support/redis_test.h"

namespace {

namespace queue = service::log_ingest::queue;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string("log ingest queue: ") + message);
    }
}

ruvia::task<void> clean(ruvia::redis_handle redis) {
    (void)co_await redis.command("DEL", queue::stream_key, queue::retained_bytes_key,
        queue::retry_schedule_key, queue::retry_attempts_key, queue::failed_stream_key,
        queue::failed_bytes_key, queue::statistics_key);
}

ruvia::task<queue::entry> append_and_claim(ruvia::redis_handle redis, std::string_view consumer,
    std::string_view payload) {
    require(co_await queue::push(redis, payload), "append fits the live budget");
    auto batch = co_await queue::read(redis, consumer);
    require(batch.size() == 1 && batch.front().payload_ == payload && batch.front().valid_,
        "binary payload round trips through the consumer group");
    co_return std::move(batch.front());
}

ruvia::task<void> check_retry_and_ownership(ruvia::redis_handle redis) {
    const std::string payload{"binary\0log-delivery", 19};
    auto entry = co_await append_and_claim(redis, "first", payload);
    bool rejected{};
    try {
        co_await queue::finish(redis, "other", entry);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "another consumer cannot acknowledge a pending delivery");
    require((co_await queue::schedule_retry(redis, "other", entry)).count() == 0,
        "another consumer cannot schedule a retry");
    require(co_await queue::fail(redis, "other", entry, "invalid_log_envelope") ==
                queue::failure_result::ownership_lost,
        "another consumer cannot archive a delivery");

    std::string owner{"first"};
    for (std::int64_t attempt = 0; attempt < 9; ++attempt) {
        const auto delay = co_await queue::schedule_retry(redis, owner, entry);
        require(delay.count() == std::min(queue::retry_maximum_ms, queue::retry_initial_ms << attempt),
            "exponential retry delay has a fixed upper bound");
        const auto statistics = co_await queue::inspect_statistics(redis);
        require(statistics.retained_entries_ == 1 && statistics.pending_entries_ == 1 &&
                    statistics.retained_bytes_ == static_cast<std::int64_t>(payload.size()) &&
                    statistics.scheduled_retries_ == 1 && statistics.retries_total_ == attempt + 1 &&
                    statistics.failed_entries_ == 0,
            "more than four temporary failures preserve the original delivery and byte budget");
        require((co_await queue::claim_retries(redis, "early")).empty(),
            "a retry is unavailable before its persisted deadline");
        (void)co_await redis.command("ZADD", queue::retry_schedule_key, "0", entry.id_);
        const std::string next_owner = owner == "first" ? "second" : "first";
        auto claimed = co_await queue::claim_retries(redis, next_owner);
        require(claimed.size() == 1 && claimed.front().id_ == entry.id_ && claimed.front().payload_ == payload,
            "an available consumer atomically claims the due retry");
        require((co_await queue::claim_retries(redis, "competitor")).empty(),
            "the same due retry cannot be claimed twice");
        require((co_await queue::schedule_retry(redis, owner, entry)).count() == 0,
            "the previous owner loses mutation rights after recovery");
        owner = next_owner;
    }
    co_await queue::finish(redis, owner, entry);
    co_await queue::finish(redis, owner, entry);
    const auto statistics = co_await queue::inspect_statistics(redis);
    require(statistics.retained_entries_ == 0 && statistics.pending_entries_ == 0 &&
                statistics.retained_bytes_ == 0 && statistics.scheduled_retries_ == 0 &&
                statistics.finished_total_ == 1,
        "successful persistence acknowledgement removes the input and retry state exactly once");
    require((co_await redis.command("HLEN", queue::retry_attempts_key)).integer() == 0,
        "retry attempt metadata is removed after completion");
}

ruvia::task<void> check_failure_archive_and_replay(ruvia::redis_handle redis) {
    const auto entry = co_await append_and_claim(redis, "first", "invalid protobuf payload");
    const auto maximum_failed_bytes = std::to_string(queue::max_failed_bytes);
    (void)co_await redis.command("SET", queue::failed_bytes_key, maximum_failed_bytes);
    require(co_await queue::fail(redis, "first", entry, "invalid_log_envelope") == queue::failure_result::full,
        "a byte-full failure archive rejects an atomic move");
    auto statistics = co_await queue::inspect_statistics(redis);
    require(statistics.retained_entries_ == 1 && statistics.pending_entries_ == 1 &&
                statistics.failed_entries_ == 0 && statistics.failures_total_ == 0,
        "failure archive pressure retains the original pending record");
    require((co_await queue::schedule_retry(redis, "first", entry)).count() > 0,
        "a full failure archive permits bounded retry instead of dropping data");
    (void)co_await redis.command("DEL", queue::failed_bytes_key);
    require(co_await queue::fail(redis, "first", entry, "invalid_log_envelope") ==
                queue::failure_result::archived,
        "permanent invalid data moves to the archive");
    statistics = co_await queue::inspect_statistics(redis);
    require(statistics.retained_entries_ == 0 && statistics.retained_bytes_ == 0 &&
                statistics.pending_entries_ == 0 && statistics.scheduled_retries_ == 0 &&
                statistics.failed_entries_ == 1 && statistics.failed_bytes_ > 0 &&
                statistics.failures_total_ == 1,
        "archival atomically removes the pending input and accounts for archive bytes");
    const auto archived = co_await queue::read_failed(redis);
    require(archived.size() == 1 && archived.front().source_id_ == entry.id_ &&
                archived.front().payload_ == entry.payload_ && archived.front().reason_ == "invalid_log_envelope" &&
                archived.front().attempts_ == 1 && archived.front().failed_unix_ms_ > 0,
        "the archive preserves payload, origin, reason, retry count, and UTC failure time");
    require((co_await queue::read_failed(redis, archived.front().id_)).empty(),
        "failure inspection has an exclusive pagination cursor");

    const auto maximum_live_bytes = std::to_string(queue::max_retained_bytes);
    (void)co_await redis.command("SET", queue::retained_bytes_key, maximum_live_bytes);
    require(!(co_await queue::replay_failed(redis, archived.front().id_)),
        "replay observes the live byte budget");
    require((co_await queue::read_failed(redis)).size() == 1,
        "replay cannot remove the archive record before a successful live append");
    (void)co_await redis.command("DEL", queue::retained_bytes_key);
    const auto replayed_id = co_await queue::replay_failed(redis, archived.front().id_);
    require(replayed_id.has_value(), "archive replay appends to the regular consumer group stream");
    require(!(co_await queue::replay_failed(redis, archived.front().id_)),
        "replaying the same archive identifier is idempotent");
    auto replayed = co_await queue::read(redis, "replayer");
    require(replayed.size() == 1 && replayed.front().id_ == *replayed_id &&
                replayed.front().payload_ == entry.payload_,
        "replay retains the exact payload for the regular validation and persistence path");
    co_await queue::finish(redis, "replayer", replayed.front());
    statistics = co_await queue::inspect_statistics(redis);
    require(statistics.failed_entries_ == 0 && statistics.failed_bytes_ == 0 &&
                statistics.retained_entries_ == 0 && statistics.replays_total_ == 1,
        "replay removes archived bytes and exposes its counter");
}

ruvia::task<void> check_entry_budget_and_crash_recovery(ruvia::redis_handle redis) {
    const auto entry = co_await append_and_claim(redis, "crashed", "recoverable delivery");
    const auto maximum_entries = std::to_string(queue::max_failed_entries);
    const std::array<std::string_view, 1> keys{queue::failed_stream_key};
    const std::array<std::string_view, 1> arguments{maximum_entries};
    constexpr std::string_view fill_failure_archive{R"lua(
for index = 1, tonumber(ARGV[1]) do redis.call('XADD', KEYS[1], '*', 'fixture', 'full') end
return 1
)lua"};
    (void)co_await redis.eval(fill_failure_archive, keys, arguments);
    require(co_await queue::fail(redis, "crashed", entry, "invalid_log_envelope") == queue::failure_result::full,
        "the failure archive enforces its entry budget independently of bytes");
    require((co_await queue::inspect_statistics(redis)).retained_entries_ == 1,
        "entry budget pressure retains live data");
    (void)co_await redis.command("DEL", queue::failed_stream_key);

    require((co_await queue::reclaim_stale(redis, "replacement")).empty(),
        "a newly assigned delivery is protected from premature reclaim");
    const auto stale_idle = std::to_string(queue::reclaim_idle_ms + 1);
    (void)co_await redis.command("XCLAIM", queue::stream_key, queue::consumer_group, "crashed",
        "0", entry.id_, "IDLE", stale_idle, "JUSTID");
    const auto recovered = co_await queue::reclaim_stale(redis, "replacement");
    require(recovered.size() == 1 && recovered.front().id_ == entry.id_,
        "a replacement consumer recovers an abandoned pending delivery");
    require(co_await queue::fail(redis, "crashed", entry, "invalid_log_envelope") ==
                queue::failure_result::ownership_lost,
        "a resumed former process cannot mutate the replacement's task");
    co_await queue::finish(redis, "replacement", recovered.front());
}

ruvia::task<void> run(ruvia::redis_client& client) {
    const auto redis = client.handle();
    co_await clean(redis);
    co_await queue::initialize(redis);
    co_await queue::initialize(redis);
    co_await check_retry_and_ownership(redis);
    co_await check_failure_archive_and_replay(redis);
    co_await check_entry_budget_and_crash_recovery(redis);
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
        ruvia::redis_client redis(loop, *config);
        loops.start();
        loop.start(redis.connect()).get();
        loop.start(run(redis)).get();
        loop.start(redis.shutdown()).get();
        loops.stop();
        loops.join();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Log ingestion queue: durable retry, bounded failure archive, replay, ownership, recovery passed\n";
    return 0;
}
