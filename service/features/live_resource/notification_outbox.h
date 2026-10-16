#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/db/db_transaction.h>

#include <array>
#include <charconv>
#include <chrono>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/background/reliable_stream.h"
#include "service/features/background/work_available.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/live_resource/outbox_entity.h"
#include "service/features/live_resource/redis_snapshot_cache.h"
#include "service/features/logging/logger.h"
#include "service/utils/token.h"

// The transactional outbox: a business transaction inserts one row with its
// committed notification; a claim loop relays each row to its Redis stream
// and deletes it. Its participants are a compile-time list of types, each
// optionally providing
//   static constexpr std::string_view kind;
//   static ruvia::task<void> publish(ruvia::redis_handle, std::string_view id, std::string_view payload);
//       atomically publishes one row of `kind`, including owned business effects;
//   template <typename context_type, typename rows_type>
//   static ruvia::task<std::set<std::string>> prepare(ruvia::db_transaction&, context_type&, const rows_type&);
//       work inside the claim transaction before any row of the batch is
//       published; returns the ids of rows that stay durable for a retry.
namespace service::live_resource {

namespace outbox {

inline constexpr std::string_view work_type{"outbox"};
inline constexpr std::string_view resource_kind{"resource"};

inline std::string pack(std::span<const std::pair<std::string, std::string>> fields) {
    std::string payload;
    for (const auto& [name, value] : fields) {
        for (const std::string_view part : {std::string_view{name}, std::string_view{value}}) {
            payload += std::to_string(part.size()) + ":";
            payload += part;
        }
    }
    return payload;
}

inline std::vector<std::pair<std::string, std::string>> unpack(std::string_view payload) {
    std::vector<std::string> parts;
    while (!payload.empty()) {
        const auto colon = payload.find(':');
        std::size_t length{};
        if (colon == std::string_view::npos) {
            throw std::runtime_error("invalid notification outbox payload");
        }
        const auto [end, error] = std::from_chars(payload.data(), payload.data() + colon, length);
        if (error != std::errc{} || end != payload.data() + colon || length > payload.size() - colon - 1) {
            throw std::runtime_error("invalid notification outbox length");
        }
        parts.emplace_back(payload.substr(colon + 1, length));
        payload.remove_prefix(colon + 1 + length);
    }
    if (parts.size() % 2 != 0) {
        throw std::runtime_error("invalid notification outbox fields");
    }
    std::vector<std::pair<std::string, std::string>> fields;
    for (std::size_t index = 0; index < parts.size(); index += 2) {
        fields.emplace_back(std::move(parts[index]), std::move(parts[index + 1]));
    }
    return fields;
}

template <typename database_type>
ruvia::task<std::string> insert(database_type& database, std::string_view kind, std::string_view payload) {
    const auto id = service::utils::random_token();
    outbox_entity event;
    event.template set<"id">(id);
    event.template set<"kind">(kind);
    event.template set<"payload">(payload);
    (void)co_await database.template get_repository<outbox_entity>().insert(event);
    co_return id;
}

template <typename database_type>
ruvia::task<void> erase(database_type& database, std::string_view id) {
    ruvia::db_query candidate;
    candidate.select(candidate.column("id")).from(outbox_entity::table_name()).where(candidate.binary(candidate.column("id"), ruvia::db_binary_operator::equal, candidate.value(id))).lock({.mode_ = ruvia::db_row_lock::update, .skip_locked_ = true});
    ruvia::db_query query;
    query.delete_from(outbox_entity::table_name()).where(query.binary(query.column("id"), ruvia::db_binary_operator::in, query.subquery(candidate)));
    (void)co_await database.execute(query);
}

// One committed row as its relay publishes it.
struct publication final {
    std::string_view stream_;
    std::int64_t max_entries_{};
    std::vector<std::pair<std::string, std::string>> fields_{};
    // Snapshot cache scopes invalidated atomically with the append.
    std::vector<std::string> invalidations_{};
};

// Atomically deduplicates a committed event and publishes it. The same event
// may be retried after the sender dies between Redis and the database commit.
inline constexpr std::string_view publish_script{R"lua(
local duplicate = redis.call('GET', KEYS[1])
if duplicate then return duplicate end
local stream_type = redis.call('TYPE', KEYS[2]).ok
if stream_type ~= 'none' and stream_type ~= 'stream' then
    return redis.error_reply('notification target must be a stream')
end
snapshot_validate_generations(KEYS[3], KEYS[4], KEYS[5])
local scopes = {}
for i=1,tonumber(ARGV[2]) do scopes[i] = ARGV[2+i] end
local entry = redis.call('XADD', KEYS[2], 'MAXLEN', '~', ARGV[1], '*', unpack(ARGV, 3+#scopes, #ARGV-1))
snapshot_invalidate(KEYS[3], KEYS[4], KEYS[5], scopes, ARGV[#ARGV])
redis.call('SET', KEYS[1], entry, 'EX', 604800)
return entry
)lua"};

inline ruvia::task<void> publish(ruvia::redis_handle redis, std::string_view id, publication value) {
    value.fields_.emplace_back("event_id", id);
    std::vector<std::string> keys{"flexedge:notification-published:v1:" + std::string(id), std::string(value.stream_)};
    for (const auto key : redis_snapshot_cache::generation_keys) {
        keys.emplace_back(key);
    }
    std::vector<std::string> arguments{std::to_string(value.max_entries_), std::to_string(value.invalidations_.size())};
    arguments.insert(arguments.end(), value.invalidations_.begin(), value.invalidations_.end());
    for (const auto& [name, field] : value.fields_) {
        arguments.push_back(name);
        arguments.push_back(field);
    }
    arguments.emplace_back(id);
    std::vector<std::string_view> key_views, argument_views;
    for (const auto& key : keys) {
        key_views.push_back(key);
    }
    for (const auto& argument : arguments) {
        argument_views.push_back(argument);
    }
    const auto script = std::string(redis_snapshot_cache::invalidation_lua) + std::string(publish_script);
    const auto result = co_await redis.eval(script, key_views, argument_views);
    service::utils::require_redis_success(result, "could not publish committed notification");
    if (result.kind() != ruvia::redis_value::kind_type::string) {
        throw std::runtime_error("invalid outbox publish result");
    }
}

// Dispatches to the owner of the durable event kind. The owner publishes its
// Redis effects as one atomic operation; the outbox only claims and retries.
template <typename... participants>
ruvia::task<void> publish_event(ruvia::redis_handle redis, std::string_view id,
    std::string_view kind, std::string_view payload) {
    bool published{};
    const auto publish_with = [&]<typename participant>() -> ruvia::task<void> {
        if constexpr (requires { participant::publish(redis, id, payload); }) {
            if (!published && participant::kind == kind) {
                co_await participant::publish(redis, id, payload);
                published = true;
            }
        }
    };
    (co_await publish_with.template operator()<participants>(), ...);
    if (!published) {
        throw std::runtime_error("invalid outbox event kind");
    }
}

// Delay before durable retry `attempts` of an undeliverable row.
[[nodiscard]] constexpr std::chrono::seconds retry_delay(std::int32_t attempts) noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        service::background::reliable_stream::backoff(attempts, std::chrono::seconds(2), std::chrono::seconds(64)));
}

template <typename... participants, typename context_type>
ruvia::task<bool> deliver_batch(context_type& context) {
    auto transaction = co_await context.db().begin_transaction();
    ruvia::db_query expressions;
    auto pending = transaction.template get_repository<outbox_entity>().create_query_builder();
    pending.where(expressions.binary(expressions.column("available_at"), ruvia::db_binary_operator::less_equal,
                      expressions.call("now")))
        .order_by("created_at")
        .add_order_by("id")
        .take(32)
        .set_lock({.mode_ = ruvia::db_row_lock::update, .skip_locked_ = true});
    const auto events = co_await pending.get_many();
    // The alias owns one DB connection: participants reuse this claim
    // transaction, including its row locks, instead of a second lease.
    std::vector<ruvia::task<std::set<std::string>>> preparations;
    const auto prepare_with = [&]<typename participant>() {
        if constexpr (requires { participant::prepare(transaction, context, events); }) {
            preparations.push_back(participant::prepare(transaction, context, events));
        }
    };
    (prepare_with.template operator()<participants>(), ...);
    std::set<std::string> deferred;
    for (auto& preparation : preparations) {
        deferred.merge(co_await std::move(preparation));
    }
    std::vector<std::string> completed_keys;
    for (const auto& event : events) {
        const std::string_view id{event.template get<"id">()};
        bool failed{};
        try {
            if (deferred.contains(std::string(id))) {
                throw std::runtime_error("outbox event is not ready for publication");
            }
            co_await publish_event<participants...>(context.redis().with_options({.timeout_ = notifications::send_timeout,
                                                        .stop_token_ = context.get_stop_token()}),
                id, event.template get<"kind">(), event.template get<"payload">());
        } catch (const std::exception&) {
            failed = true;
        }
        if (failed) {
            ruvia::db_query retry;
            const auto attempts = std::min<std::int32_t>(event.template get<"attempts">() + 1, 1000000);
            const std::vector<ruvia::db_named_argument> interval{{"secs", retry.value(retry_delay(attempts).count())}};
            retry.update(outbox_entity::table_name()).set("attempts", retry.value(attempts)).set("available_at", retry.binary(retry.call("now"), ruvia::db_binary_operator::add, retry.call("make_interval", {}, interval))).where(retry.binary(retry.column("id"), ruvia::db_binary_operator::equal, retry.value(id)));
            (void)co_await transaction.execute(retry);
            service::logging::error("Notification outbox delivery deferred; event remains durable");
        } else {
            co_await erase(transaction, id);
            completed_keys.push_back("flexedge:notification-published:v1:" + std::string(id));
        }
        if (context.get_stop_token().stop_requested()) {
            break;
        }
    }
    co_await transaction.commit();
    // All publishers claim this row under its DB lock. After a known commit
    // there can be no late connection-side publisher, so dedup metadata can
    // be released; an uncertain commit retains the bounded-time fence.
    for (const auto& key : completed_keys) {
        try {
            (void)co_await context.redis().with_options({.timeout_ = notifications::send_timeout,
                                                            .stop_token_ = context.get_stop_token()})
                .del(key);
        } catch (const std::exception&) {
            break;
        }
    }
    co_return !events.empty();
}

inline constexpr auto maximum_wait = std::chrono::seconds(15);

// Each Worker claims committed rows. Its own commits wake it at once; rows of
// a busy or exited committer are found at the next bounded scan.
template <typename... participants, typename context_type>
ruvia::task<void> run_worker(context_type& context) {
    // Subscribe before the first claim so a commit of this worker during the
    // drain remains queued until this task starts waiting.
    const auto own = service::background::work_available::subscribe(context, work_type);
    while (!context.get_stop_token().stop_requested()) {
        bool retry{};
        try {
            if (co_await deliver_batch<participants...>(context)) {
                continue;
            }

            // A wakeup is a latency hint. The database remains authoritative:
            // scan at the next durable deadline and at most 15 seconds after
            // another worker committed or Redis recovered, including empty scans.
            ruvia::db_query available;
            const auto delay_ms = available.binary(
                available.extract(ruvia::db_date_part::epoch,
                    available.binary(available.aggregate("min", {available.column("available_at")}),
                        ruvia::db_binary_operator::subtract, available.call("clock_timestamp"))),
                ruvia::db_binary_operator::multiply, available.value(1000));
            available.select(available.cast(available.call("ceil", {delay_ms}), ruvia::db_data_type::big_int))
                .from(outbox_entity::table_name());
            const auto rows = co_await context.db().query(available);
            auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(maximum_wait);
            if (!rows.empty()) {
                if (const auto delay = rows[0][0].template as<std::int64_t>()) {
                    wait = std::min(wait, std::chrono::milliseconds(std::max<std::int64_t>(100, *delay)));
                }
            }
            (void)co_await own.receive_for(wait, context.get_stop_token());
        } catch (const std::exception&) {
            if (context.get_stop_token().stop_requested()) {
                break;
            }
            retry = true;
            service::logging::error("Notification outbox will retry durable work after Redis recovery");
        }
        if (retry) {
            if (co_await ruvia::sleep_for(context.worker(), maximum_wait, context.get_stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
                break;
            }
        }
    }
}

}  // namespace outbox

// Wakes the calling worker's own outbox claim loop for rows it committed.
template <typename context_type>
void wake_own_outbox(context_type& context) {
    static constexpr std::array work{outbox::work_type};
    service::background::work_available::wake_own_claims(context, work);
}

template <typename transaction_type>
ruvia::task<std::string> enqueue_notification(transaction_type& transaction,
    const notifications::resource_notification& value, std::chrono::seconds delay = {}) {
    if (value.empty()) {
        co_return std::string{};
    }
    const auto durable = notifications::bounded(value);
    const auto fields = notifications::encode(durable);
    const auto id = co_await outbox::insert(transaction, outbox::resource_kind, outbox::pack(fields));
    if (delay > std::chrono::seconds::zero()) {
        ruvia::db_query defer;
        const std::vector<ruvia::db_named_argument> interval{{"secs", defer.value(delay.count())}};
        defer.update(outbox_entity::table_name()).set("available_at", defer.binary(defer.call("now"), ruvia::db_binary_operator::add, defer.call("make_interval", {}, interval))).where(defer.binary(defer.column("id"), ruvia::db_binary_operator::equal, defer.value(std::string_view{id})));
        (void)co_await transaction.execute(defer);
    }
    co_return id;
}

// Only the durable claim loop publishes. The committing worker wakes its own
// loop after commit; other workers' bounded scans cover a busy committer.
template <typename context_type>
ruvia::task<void> wake_committed_notification(context_type& context, std::string_view id,
    const notifications::resource_notification& value) {
    if (id.empty()) {
        co_return;
    }
    if constexpr (requires { context.record_committed_notification(value); }) {
        co_await context.record_committed_notification(value);
    } else {
        wake_own_outbox(context);
    }
}

// Trace persistence is deliberately outside the business transaction. A
// finite availability guard prevents its notification overtaking the trace;
// if the producer exits, the durable event becomes claimable automatically.
inline constexpr auto trace_delivery_guard = std::chrono::seconds(120);

template <typename context_type>
ruvia::task<void> release_notification(context_type& context, std::string_view id,
    const notifications::resource_notification& value) {
    if (id.empty()) {
        co_return;
    }
    try {
        ruvia::db_query release;
        release.update(outbox_entity::table_name()).set("available_at", release.call("now")).where(release.binary(release.column("id"), ruvia::db_binary_operator::equal, release.value(id)));
        (void)co_await context.db().execute(release);
        co_await wake_committed_notification(context, id, value);
    } catch (const std::exception&) {
        service::logging::error("Trace notification remains durable until its delivery guard expires");
    }
}

template <typename transaction_type, typename context_type>
ruvia::task<void> commit_and_notify(transaction_type& transaction, context_type& context,
    const notifications::resource_notification& value) {
    const auto id = co_await enqueue_notification(transaction, value);
    co_await transaction.commit();
    co_await wake_committed_notification(context, id, value);
}

}  // namespace service::live_resource
