#pragma once

#include <ruvia/core/timer.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/db/db_transaction.h>

#include <charconv>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/background/work_available.h"
#include "service/features/live_resource/node_presence.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/live_resource/outbox_entity.h"
#include "service/features/live_resource/redis_snapshot_cache.h"
#include "service/features/log_ingest/notifications.h"
#include "service/features/logging/logger.h"
#include "service/features/node_channel/projection.h"
#include "service/utils/token.h"

namespace service::live_resource {

namespace outbox {

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
local has_presence = ARGV[#ARGV-2] ~= ''
if has_presence then
    presence_validate(KEYS[6])
    local tenant, node, seen = ARGV[#ARGV-3], ARGV[#ARGV-2], tonumber(ARGV[#ARGV-1])
    if tenant == '' or string.find(tenant, '|', 1, true) or string.find(node, '|', 1, true) or not seen or seen <= 0 then
        return redis.error_reply('invalid presence metadata')
    end
end
local scopes = {}
for i=1,tonumber(ARGV[2]) do scopes[i] = ARGV[2+i] end
local entry = redis.call('XADD', KEYS[2], 'MAXLEN', '~', ARGV[1], '*', unpack(ARGV, 3+#scopes, #ARGV-4))
snapshot_invalidate(KEYS[3], KEYS[4], KEYS[5], scopes, ARGV[#ARGV])
if has_presence then presence_record(KEYS[6], ARGV[#ARGV-3], ARGV[#ARGV-2], tonumber(ARGV[#ARGV-1])) end
redis.call('SET', KEYS[1], entry, 'EX', 604800)
return entry
)lua"};

inline ruvia::task<void> publish(ruvia::redis_handle redis, std::string_view id, std::string_view kind,
    std::string_view payload) {
    std::string stream;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::string> invalidations;
    std::string presence_tenant, presence_node;
    std::int64_t presence_seen{};
    if (kind == "resource") {
        stream = notifications::stream_key;
        fields = unpack(payload);
        auto notification = notifications::decode(fields);
        if (!notification) {
            throw std::runtime_error("invalid resource outbox notification");
        }
        presence_tenant = notification->tenant_id_;
        presence_node = notification->node_id_;
        presence_seen = notification->node_seen_unix_ms_;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                             .count();
        if (!presence_node.empty() && presence_seen > 0 && now - presence_seen > 15000) {
            // A delayed patch must not replace newer runtime. Preserve its
            // original presence timestamp and request one recovery snapshot.
            notification->node_runtime_.clear();
            notification->origin_runtime_.clear();
            notification->missed_until_ = now;
        }
        if (notification->missed_until_) {
            notification->missed_until_ = now;
            fields = notifications::encode(*notification);
        }
        for (const auto& change : notification->changes_) {
            const auto scopes = redis_snapshot_cache::invalidation_scopes(notification->tenant_id_, change.resource_, change.id_);
            invalidations.insert(invalidations.end(), scopes.begin(), scopes.end());
        }
        if (notification->missed_until_) {
            invalidations.push_back(notification->tenant_id_ == notifications::all_tenants ? "*" : redis_snapshot_cache::tenant_scope(notification->tenant_id_));
        }
    } else if (kind == "log") {
        stream = service::log_ingest::notifications::stream_key;
        fields = unpack(payload);
        std::string_view tenant, type, resource_id;
        for (const auto& [name, value] : fields) {
            if (name == "tenant_id") {
                tenant = value;
            }
            if (name == "resource_type") {
                type = value;
            }
            if (name == "resource_id") {
                resource_id = value;
            }
        }
        if (tenant.empty() || resource_id.empty() || (type != "access" && type != "node")) {
            throw std::runtime_error("invalid log outbox notification");
        }
        if (type == "access") {
            invalidations = redis_snapshot_cache::invalidation_scopes(tenant, resource::access_history, resource_id);
            invalidations.push_back(redis_snapshot_cache::resource_scope(tenant, resource::overview_traffic));
        }
    } else if (kind == "task") {
        if (payload != "provider-verification" && payload != "dns-sync" && payload != "certificate" && payload != "website-dispatch") {
            throw std::invalid_argument("unsupported task wakeup type");
        }
        stream = service::background::work_available::stream_key;
        fields.emplace_back("type", payload);
    } else {
        throw std::runtime_error("invalid outbox event kind");
    }
    fields.emplace_back("event_id", id);
    std::vector<std::string> keys{"flexedge:notification-published:v1:" + std::string(id), stream};
    for (const auto key : redis_snapshot_cache::generation_keys) {
        keys.emplace_back(key);
    }
    keys.emplace_back(node_presence::deadlines_key);
    std::vector<std::string> arguments{std::to_string(kind == "resource" ? notifications::max_entries : kind == "task" ? 1024
                                                                                                                       : service::log_ingest::notifications::max_entries),
        std::to_string(invalidations.size())};
    arguments.insert(arguments.end(), invalidations.begin(), invalidations.end());
    for (const auto& [name, value] : fields) {
        arguments.push_back(name);
        arguments.push_back(value);
    }
    arguments.insert(arguments.end(), {presence_tenant, presence_node, std::to_string(presence_seen), std::string(id)});
    std::vector<std::string_view> key_views, argument_views;
    for (const auto& key : keys) {
        key_views.push_back(key);
    }
    for (const auto& argument : arguments) {
        argument_views.push_back(argument);
    }
    const auto script = std::string(redis_snapshot_cache::invalidation_lua) + std::string(node_presence::record_lua) + std::string(publish_script);
    const auto result = co_await redis.eval(script, key_views, argument_views);
    service::utils::require_redis_success(result, "could not publish committed notification");
    if (result.kind() != ruvia::redis_value::kind_type::string) {
        throw std::runtime_error("invalid outbox publish result");
    }
}

template <typename context_type>
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
    // The alias owns one DB connection. Reuse the claim transaction, and
    // acquire node locks in a stable tenant/node order across all claimants.
    std::set<std::string> projection_tenants;
    std::set<std::string> failed_projections;
    std::vector<std::string> completed_keys;
    for (const auto& event : events) {
        if (event.template get<"kind">() == "resource") {
            const auto notification = notifications::decode(unpack(event.template get<"payload">()));
            if (notification && notification->node_config_) {
                projection_tenants.insert(notification->tenant_id_);
            }
        }
    }
    for (const auto& tenant : projection_tenants) {
        try {
            co_await service::node_channel::refresh(transaction, context.redis(), tenant);
        } catch (const ruvia::db_error&) {
            throw;
        } catch (const std::exception&) {
            failed_projections.insert(tenant);
        }
    }
    for (const auto& event : events) {
        bool failed{};
        try {
            if (event.template get<"kind">() == "resource") {
                const auto notification = notifications::decode(unpack(event.template get<"payload">()));
                if (notification && notification->node_config_ && failed_projections.contains(notification->tenant_id_)) {
                    throw std::runtime_error("node channel projection is not ready");
                }
            }
            co_await publish(context.redis().with_options({.timeout_ = notifications::send_timeout,
                                 .stop_token_ = context.get_stop_token()}),
                event.template get<"id">(),
                event.template get<"kind">(), event.template get<"payload">());
        } catch (const std::exception&) {
            failed = true;
        }
        if (failed) {
            ruvia::db_query retry;
            const auto attempts = std::min<std::int32_t>(event.template get<"attempts">() + 1, 1000000);
            const auto seconds = std::int64_t{1} << std::min(attempts, 6);
            const std::vector<ruvia::db_named_argument> interval{{"secs", retry.value(seconds)}};
            retry.update(outbox_entity::table_name()).set("attempts", retry.value(attempts)).set("available_at", retry.binary(retry.call("now"), ruvia::db_binary_operator::add, retry.call("make_interval", {}, interval))).where(retry.binary(retry.column("id"), ruvia::db_binary_operator::equal, retry.value(std::string_view{event.template get<"id">()})));
            (void)co_await transaction.execute(retry);
            service::logging::error("Notification outbox delivery deferred; event remains durable");
        } else {
            co_await erase(transaction, event.template get<"id">());
            completed_keys.push_back("flexedge:notification-published:v1:" + std::string(event.template get<"id">()));
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

inline ruvia::task<void> wake(ruvia::redis_handle redis) {
    co_await service::background::work_available::publish(redis, "outbox");
}

template <typename context_type>
ruvia::task<void> run_worker(context_type& context) {
    // Bind before catchup so a hint arriving while the durable batch drains
    // remains queued until this task starts waiting.
    auto& signal = service::background::work_available::get_signals(context).for_type("outbox");
    while (!context.get_stop_token().stop_requested()) {
        bool retry{};
        try {
            if (co_await deliver_batch(context)) {
                continue;
            }

            // A wakeup is a latency hint. The database remains authoritative:
            // scan at the next durable deadline and at most 15 seconds after a
            // missed hint, stream reset or Redis recovery, including empty scans.
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
            co_await signal.wait(wait, context.get_stop_token());
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

template <typename transaction_type>
ruvia::task<std::string> enqueue_notification(transaction_type& transaction,
    const notifications::resource_notification& value, std::chrono::seconds delay = {}) {
    if (value.empty()) {
        co_return std::string{};
    }
    const auto durable = notifications::bounded(value);
    const auto fields = notifications::encode(durable);
    const auto id = co_await outbox::insert(transaction, "resource", outbox::pack(fields));
    if (delay > std::chrono::seconds::zero()) {
        ruvia::db_query defer;
        const std::vector<ruvia::db_named_argument> interval{{"secs", defer.value(delay.count())}};
        defer.update(outbox_entity::table_name()).set("available_at", defer.binary(defer.call("now"), ruvia::db_binary_operator::add, defer.call("make_interval", {}, interval))).where(defer.binary(defer.column("id"), ruvia::db_binary_operator::equal, defer.value(std::string_view{id})));
        (void)co_await transaction.execute(defer);
    }
    co_return id;
}

template <typename transaction_type>
ruvia::task<void> enqueue_log(transaction_type& transaction, std::string_view type,
    std::string_view tenant_id, std::string_view resource_id) {
    if (type != "access" && type != "node") {
        throw std::invalid_argument("unsupported outbox log resource");
    }
    const std::vector<std::pair<std::string, std::string>> fields{
        {"tenant_id", std::string(tenant_id)}, {"resource_type", std::string(type)}, {"resource_id", std::string(resource_id)}};
    (void)co_await outbox::insert(transaction, "log", outbox::pack(fields));
}

template <typename transaction_type>
ruvia::task<void> enqueue_worker_wakeup(transaction_type& transaction, std::string_view type) {
    (void)co_await outbox::insert(transaction, "task", type);
}

// Only the durable claim loop publishes. Producers send an availability hint
// after commit; a lost hint is covered by the bounded recovery deadline.
template <typename context_type>
ruvia::task<void> wake_committed_notification(context_type& context, std::string_view id,
    const notifications::resource_notification& value) {
    if (id.empty()) {
        co_return;
    }
    if constexpr (requires { context.record_committed_notification(value); }) {
        co_await context.record_committed_notification(value);
    } else {
        try {
            co_await outbox::wake(context.redis().with_options({.timeout_ = notifications::send_timeout,
                .stop_token_ = context.get_stop_token()}));
        } catch (const std::exception&) {
            // The outbox has a bounded durable-work recovery deadline.
        }
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
