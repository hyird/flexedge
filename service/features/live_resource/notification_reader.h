#pragma once

#include <ruvia/core/operation_options.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/redis/redis.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "service/features/background/work_available.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/live_resource/redis_snapshot_cache.h"
#include "service/features/logging/logger.h"

// Every Web Worker follows the committed notification streams on its own
// Redis connection and delivers them to its own subscriptions only. The
// streams and what consumes them come from a compile-time list of handler
// types; each handler names the stream it follows and provides
//   using stream = ...;   // static constexpr std::string_view key; using value_type;
//                         // static std::optional<value_type> parse(const ruvia::redis_value& entry);
//   template <typename context_type>
//   static void deliver(context_type&, const stream::value_type&, std::optional<std::int64_t> recovered_at);
// and optionally
//   static void ready(context_type&);  // the reader (re)captured the stream heads
//   static ruvia::task<void> recover(context_type&, ruvia::redis_handle);
//       // unread entries of its stream may have been lost
// `recovered_at`: when this reader last recovered from losing the streams;
// an owed recovery of a producer from before that instant is already covered.
// Each entry is parsed once per stream and given to every handler of it.
namespace service::live_resource {

inline constexpr auto notification_retry_delay = std::chrono::seconds(1);
inline constexpr auto notification_read_timeout = std::chrono::seconds(20);

struct notification_cursor final {
    std::string id_{"0-0"};
    std::int64_t seen_entries_{};
};

// A single atomic metadata/read batch closes the race between observing a
// retained prefix and accepting its entries. XREAD below is only the blocking
// wakeup; this batch accepts those same entries, or recovers if they were trimmed
// before this Worker resumed. No event bodies survive the current iteration.
inline constexpr std::string_view notification_batch_script = R"lua(
local function less(a,b)
 local am,as=string.match(a,'^(%d+)%-(%d+)$')
 local bm,bs=string.match(b,'^(%d+)%-(%d+)$')
 if not am or not bm then error('invalid notification cursor') end
 if #am ~= #bm then return #am < #bm end
 if am ~= bm then return am < bm end
 if #as ~= #bs then return #as < #bs end
 return as < bs
end
local result={redis.call('GET',KEYS[#KEYS]) or '',{}}
for i=1,#KEYS-1 do
 local cursor=ARGV[2*i]
 local seen=tonumber(ARGV[2*i+1])
 local kind=redis.call('TYPE',KEYS[i]).ok
 if kind ~= 'none' and kind ~= 'stream' then error('invalid notification stream type') end
 local last,first,added,length,deleted='0-0',nil,0,0,'0-0'
 if kind == 'stream' then
  local info=redis.call('XINFO','STREAM',KEYS[i])
  added=-1
  for n=1,#info,2 do
   if info[n]=='last-generated-id' then last=info[n+1]
   elseif info[n]=='first-entry' and info[n+1] then first=info[n+1][1]
   elseif info[n]=='entries-added' then added=tonumber(info[n+1])
   elseif info[n]=='length' then length=tonumber(info[n+1])
   elseif info[n]=='max-deleted-entry-id' then deleted=info[n+1] end
  end
 end
 local gap=false
 if ARGV[1] ~= 'head' then
  gap=less(last,cursor) or less(cursor,deleted)
  if added >= 0 and seen >= 0 then
   gap=gap or added < seen or added-length > seen
  else
   -- Redis before entries-added cannot distinguish an unread trimmed prefix
   -- from an exactly consumed prefix. Recover this stream conservatively once.
   gap=gap or (first ~= nil and less(cursor,first)) or (length==0 and less(cursor,last)) or (kind=='none' and cursor~='0-0')
  end
 end
 local entries={}
 if ARGV[1]=='head' or gap then
  cursor=last
  seen=added
 elseif kind=='stream' then
  entries=redis.call('XRANGE',KEYS[i],'('..cursor,'+','COUNT',128)
  if #entries>0 then cursor=entries[#entries][1] end
  if added >= 0 and seen >= 0 then seen=seen+#entries else seen=-1 end
 end
 result[2][i]={KEYS[i],entries,cursor,seen,gap and 1 or 0}
end
return result
)lua";

inline ruvia::task<ruvia::redis_value> read_notification_batch(ruvia::redis_handle redis,
    std::span<const std::string_view> streams, std::span<const notification_cursor> cursors, bool head) {
    std::vector<std::string_view> keys(streams.begin(), streams.end());
    keys.push_back(service::background::work_available::generation_key);
    std::vector<std::string> counts(cursors.size());
    std::vector<std::string_view> arguments{head ? "head" : "read"};
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        counts[index] = std::to_string(cursors[index].seen_entries_);
        arguments.push_back(cursors[index].id_);
        arguments.push_back(counts[index]);
    }
    co_return co_await redis.eval(notification_batch_script, keys, arguments);
}

inline bool matching_notification_generation(const ruvia::redis_value& batch, std::string_view generation,
    std::size_t streams) {
    service::utils::require_redis_success(batch, "could not read notification batch");
    if (batch.kind() != ruvia::redis_value::kind_type::array || batch.array().size() != 2 ||
        batch.array()[0].kind() != ruvia::redis_value::kind_type::string ||
        batch.array()[1].kind() != ruvia::redis_value::kind_type::array ||
        batch.array()[1].array().size() != streams) {
        throw std::runtime_error("invalid notification batch");
    }
    return batch.array()[0].string() == generation;
}

inline void accept_notification_cursor(notification_cursor& cursor, const ruvia::redis_value& stream) {
    const auto fields = stream.array();
    if (fields.size() != 5 || fields[2].kind() != ruvia::redis_value::kind_type::string ||
        fields[3].kind() != ruvia::redis_value::kind_type::integer || fields[3].integer() < -1 ||
        fields[4].kind() != ruvia::redis_value::kind_type::integer ||
        (fields[4].integer() != 0 && fields[4].integer() != 1)) {
        throw std::runtime_error("invalid notification cursor metadata");
    }
    cursor.id_ = std::string(fields[2].string());
    cursor.seen_entries_ = fields[3].integer();
}

[[nodiscard]] inline std::int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Delivers committed resource notifications to the calling worker's own
// resource subscriptions.
struct resource_subscriptions final {
    using stream = notifications::stream;

    template <typename context_type>
    static void deliver(context_type& context, const notifications::resource_notification& value,
        std::optional<std::int64_t> recovered_at) {
        auto& hub_value = get_hub(context);
        if (value.missed_until_ && (!recovered_at || *recovered_at < *value.missed_until_)) {
            if (value.tenant_id_ == notifications::all_tenants) {
                hub_value.recover();
            } else {
                hub_value.recover(std::string_view(value.tenant_id_));
            }
        }
        if (value.tenant_id_ == notifications::all_tenants) {
            return;
        }
        const auto& tenant = value.tenant_id_;
        for (const auto& change : value.changes_) {
            hub_value.publish(tenant, change.resource_, change.id_);
        }
        for (const auto& attempt : value.task_attempts_) {
            hub_value.publish_task_attempt(tenant, attempt.task_id_, attempt.notice_);
        }
        for (const auto& task_id : value.task_history_resets_) {
            hub_value.reset_task_history(tenant, task_id);
        }
        if (!value.node_runtime_.empty()) {
            hub_value.publish_runtime(tenant, value.node_id_, value.node_runtime_);
        }
        for (const auto& origin : value.origin_runtime_) {
            hub_value.publish_origins(tenant, origin.website_id_, value.node_id_, origin.payload_);
        }
    }

    // Unread resource changes may be lost: reusable snapshots are invalid and
    // every subscription of this worker reads one recovery snapshot.
    template <typename context_type>
    static ruvia::task<void> recover(context_type& context, ruvia::redis_handle redis) {
        const std::vector<std::string> scopes{"*"};
        co_await redis_snapshot_cache::invalidate(redis, scopes);
        get_hub(context).recover();
    }
};

namespace notification_detail {

template <typename... types>
struct type_list final {};

template <typename list, typename type>
struct append_unique;
template <typename... types, typename type>
struct append_unique<type_list<types...>, type> {
    using result = std::conditional_t<(std::is_same_v<types, type> || ...), type_list<types...>, type_list<types..., type>>;
};

template <typename list, typename... types>
struct unique {
    using result = list;
};
template <typename list, typename first, typename... rest>
struct unique<list, first, rest...> {
    using result = typename unique<typename append_unique<list, first>::result, rest...>::result;
};

// Recovery of every handler rather than of one stream's handlers.
struct all_streams final {};

template <typename stream_list, typename... handlers>
class reader;

template <typename... streams, typename... handlers>
class reader<type_list<streams...>, handlers...> final {
public:
    static constexpr std::array<std::string_view, sizeof...(streams)> keys{streams::key...};

    // It starts at the newest entries (fresh subscriptions read initial
    // snapshots) and, after losing the streams (Redis restart, timeout,
    // network), recovers every handler once the streams are readable again:
    // entries appended meanwhile may have been lost or never sent.
    template <typename context_type>
    static ruvia::task<void> run(context_type& context) {
        static_assert(distinct_keys(), "two notification stream types name the same Redis stream");
        std::array<notification_cursor, sizeof...(streams)> cursors;
        std::string generation;
        bool initialized{};
        bool interrupted{};
        std::optional<std::int64_t> recovered_at;
        while (!context.get_stop_token().stop_requested()) {
            bool retry{};
            try {
                const auto redis = context.redis().with_options({.timeout_ = notification_read_timeout,
                    .stop_token_ = context.get_stop_token()});
                auto current_generation = co_await service::background::work_available::generation(redis);
                if (!generation.empty() && current_generation != generation) {
                    initialized = false;
                    interrupted = true;
                }
                if (!initialized) {
                    const auto heads = co_await read_notification_batch(redis, keys, cursors, true);
                    if (!matching_notification_generation(heads, current_generation, keys.size())) {
                        continue;
                    }
                    for (std::size_t index = 0; index < keys.size(); ++index) {
                        accept_notification_cursor(cursors[index], heads.array()[1].array()[index]);
                    }
                    generation = std::move(current_generation);
                    (ready<handlers>(context), ...);
                    initialized = true;
                }
                if (interrupted) {
                    for (auto& recovery : recoveries<all_streams>(context, redis)) {
                        co_await std::move(recovery);
                    }
                    recovered_at = unix_milliseconds();
                    interrupted = false;
                    service::logging::info("Resource notifications resumed; subscriptions read recovery snapshots");
                }
                const auto batch = co_await read_notification_batch(redis, keys, cursors, false);
                if (!matching_notification_generation(batch, generation, keys.size())) {
                    initialized = false;
                    interrupted = true;
                    continue;
                }
                const auto results = batch.array()[1].array();
                std::array<bool, sizeof...(streams)> gaps{};
                bool active{};
                for (std::size_t index = 0; index < keys.size(); ++index) {
                    const auto& result = results[index];
                    if (result.kind() != ruvia::redis_value::kind_type::array || result.array().size() != 5 ||
                        result.array()[0].kind() != ruvia::redis_value::kind_type::string ||
                        result.array()[0].string() != keys[index] ||
                        result.array()[1].kind() != ruvia::redis_value::kind_type::array ||
                        result.array()[4].kind() != ruvia::redis_value::kind_type::integer) {
                        throw std::runtime_error("invalid combined notification stream");
                    }
                    // Only the stream that lost unread entries recovers: a gap
                    // of one stream never manufactures unrelated updates.
                    gaps[index] = result.array()[4].integer() == 1;
                    active = active || gaps[index] || !result.array()[1].array().empty();
                }
                std::vector<ruvia::task<void>> gap_recoveries;
                [&]<std::size_t... index>(std::index_sequence<index...>) {
                    ((gaps[index] ? add_recoveries<streams>(gap_recoveries, context, redis) : void()), ...);
                }(std::index_sequence_for<streams...>{});
                for (auto& recovery : gap_recoveries) {
                    co_await std::move(recovery);
                }
                [&]<std::size_t... index>(std::index_sequence<index...>) {
                    (deliver_stream<streams>(context, results[index], recovered_at), ...);
                }(std::index_sequence_for<streams...>{});
                for (std::size_t index = 0; index < keys.size(); ++index) {
                    accept_notification_cursor(cursors[index], results[index]);
                }
                if (active) {
                    continue;
                }
                // The only blocking Redis operation on this Worker. Its reply is
                // accepted by the following atomic batch, or covered by a recovery
                // snapshot if Redis trimmed it while the Worker was suspended.
                std::vector<std::string_view> wait{"XREAD", "BLOCK", "15000", "COUNT", "1", "STREAMS"};
                wait.insert(wait.end(), keys.begin(), keys.end());
                for (const auto& cursor : cursors) {
                    wait.push_back(cursor.id_);
                }
                const auto reply = co_await redis.command(std::span<const std::string_view>(wait));
                service::utils::require_redis_success(reply, "could not wait for worker notifications");
                if (!reply.null() && reply.kind() != ruvia::redis_value::kind_type::array) {
                    throw std::runtime_error("invalid combined notification wakeup");
                }
            } catch (const std::exception& error) {
                if (!context.get_stop_token().stop_requested()) {
                    service::logging::error("Resource notification reader failure: " + std::string(error.what()));
                    retry = true;
                }
            }
            if (retry) {
                initialized = false;
                interrupted = true;
                if (co_await ruvia::sleep_for(context.worker(), notification_retry_delay, context.get_stop_token()) ==
                    ruvia::timer_sleep_result::stop_requested) {
                    co_return;
                }
            }
        }
    }

private:
    static constexpr bool distinct_keys() {
        for (std::size_t first = 0; first < keys.size(); ++first) {
            for (std::size_t second = first + 1; second < keys.size(); ++second) {
                if (keys[first] == keys[second]) {
                    return false;
                }
            }
        }
        return true;
    }

    template <typename handler, typename context_type>
    static void ready(context_type& context) {
        if constexpr (requires { handler::ready(context); }) {
            handler::ready(context);
        }
    }

    // Recovery tasks are lazy: they run in list order when awaited.
    template <typename stream, typename context_type>
    static void add_recoveries(std::vector<ruvia::task<void>>& output, context_type& context, ruvia::redis_handle redis) {
        const auto add = [&]<typename handler>() {
            if constexpr (requires { handler::recover(context, redis); }) {
                if constexpr (std::is_same_v<stream, all_streams> || std::is_same_v<typename handler::stream, stream>) {
                    output.push_back(handler::recover(context, redis));
                }
            }
        };
        (add.template operator()<handlers>(), ...);
    }

    template <typename stream, typename context_type>
    static std::vector<ruvia::task<void>> recoveries(context_type& context, ruvia::redis_handle redis) {
        std::vector<ruvia::task<void>> output;
        add_recoveries<stream>(output, context, redis);
        return output;
    }

    template <typename stream, typename context_type>
    static void deliver_stream(context_type& context, const ruvia::redis_value& result,
        std::optional<std::int64_t> recovered_at) {
        for (const auto& entry : result.array()[1].array()) {
            if (entry.kind() != ruvia::redis_value::kind_type::array || entry.array().size() != 2 ||
                entry.array()[0].kind() != ruvia::redis_value::kind_type::string ||
                entry.array()[1].kind() != ruvia::redis_value::kind_type::array) {
                throw std::runtime_error("invalid worker notification entry");
            }
            const auto value = stream::parse(entry);
            if (!value) {
                continue;
            }
            const auto deliver = [&]<typename handler>() {
                if constexpr (std::is_same_v<typename handler::stream, stream>) {
                    handler::deliver(context, *value, recovered_at);
                }
            };
            (deliver.template operator()<handlers>(), ...);
        }
    }
};

}  // namespace notification_detail

// The reader of the streams `handlers` follow, each stream read once.
template <typename... handlers>
using notification_reader = notification_detail::reader<
    typename notification_detail::unique<notification_detail::type_list<>, typename handlers::stream...>::result,
    handlers...>;

template <typename... handlers, typename context_type>
ruvia::task<void> run_notification_reader(context_type& context) {
    co_await notification_reader<handlers...>::run(context);
}

}  // namespace service::live_resource
