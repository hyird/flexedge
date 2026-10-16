#pragma once

#include <ruvia/core/operation_options.h>
#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/background/work_available.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/live_resource/redis_snapshot_cache.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/logging/logger.h"
#include "service/features/node_channel/report_waits.h"
#include "service/features/node_dispatch/fanout.h"

namespace service::live_resource {

inline constexpr auto notification_retry_delay = std::chrono::seconds(1);

inline constexpr std::array notification_streams{notifications::stream_key, service::log_ingest::notifications::stream_key,
    service::background::work_available::stream_key, service::node_channel::completion_stream_key};

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
    const std::array<notification_cursor, notification_streams.size()>& cursors, bool head) {
    std::vector<std::string_view> keys(notification_streams.begin(), notification_streams.end());
    keys.push_back(service::background::work_available::generation_key);
    std::array<std::string, notification_streams.size()> counts;
    std::vector<std::string_view> arguments{head ? "head" : "read"};
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        counts[index] = std::to_string(cursors[index].seen_entries_);
        arguments.push_back(cursors[index].id_);
        arguments.push_back(counts[index]);
    }
    co_return co_await redis.eval(notification_batch_script, keys, arguments);
}

inline bool matching_notification_generation(const ruvia::redis_value& batch, std::string_view generation) {
    service::utils::require_redis_success(batch, "could not read notification batch");
    if (batch.kind() != ruvia::redis_value::kind_type::array || batch.array().size() != 2 ||
        batch.array()[0].kind() != ruvia::redis_value::kind_type::string ||
        batch.array()[1].kind() != ruvia::redis_value::kind_type::array ||
        batch.array()[1].array().size() != notification_streams.size()) {
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

// Every subscription of the calling worker reads one recovery snapshot and
// every node control connection it holds re-reads its desired state.
template <typename context_type>
void recover_subscriptions(context_type& context, std::optional<std::string_view> tenant = std::nullopt) {
    get_hub(context).recover(tenant);
    service::log_ingest::fanout::get_hub(context).recover(tenant);
    auto& dispatch = service::node_dispatch::fanout::get_hub(context);
    if (tenant) {
        dispatch.publish(*tenant);
    } else {
        dispatch.publish_all();
    }
}

// Delivers one stream entry to the calling worker's own subscriptions.
// `recovered_at`: when this reader last recovered from losing the stream; an
// owed recovery of a producer from before that instant is already covered.
template <typename context_type>
void deliver_notification(context_type& context, const notifications::resource_notification& value,
    std::optional<std::int64_t> recovered_at) {
    if (value.missed_until_ && (!recovered_at || *recovered_at < *value.missed_until_)) {
        if (value.tenant_id_ == notifications::all_tenants) {
            recover_subscriptions(context);
        } else {
            recover_subscriptions(context, std::string_view(value.tenant_id_));
        }
    }
    if (value.tenant_id_ == notifications::all_tenants) {
        return;
    }
    auto& hub_value = get_hub(context);
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
    if (value.node_config_) {
        service::node_dispatch::fanout::get_hub(context).publish(tenant);
    }
}

// The read loop of run_notification_reader. It starts at the newest entry
// (fresh subscriptions read initial snapshots) and, after losing the stream
// (Redis restart, timeout, network), recovers every subscription of this
// worker once the stream is readable again: entries appended meanwhile may
// have been lost or never sent.
template <typename context_type>
ruvia::task<void> read_notifications(context_type& context) {
    namespace available = service::background::work_available;
    const auto& streams = notification_streams;
    std::array<notification_cursor, notification_streams.size()> cursors;
    auto& signals = available::get_signals(context);
    auto& report_waits = context.template worker_state<service::node_channel::report_waits>();
    std::string generation;
    bool initialized{};
    bool interrupted{};
    std::optional<std::int64_t> recovered_at;
    while (!context.get_stop_token().stop_requested()) {
        bool retry{};
        try {
            const auto redis = context.redis().with_options({.timeout_ = notifications::read_timeout,
                .stop_token_ = context.get_stop_token()});
            auto current_generation = co_await available::generation(redis);
            if (!generation.empty() && current_generation != generation) {
                initialized = false;
                interrupted = true;
            }
            if (!initialized) {
                // All local signal channels exist before the cursor snapshot.
                // The following catchup hint covers work that raced startup,
                // while later appends stay visible to the independent cursors.
                const auto heads = co_await read_notification_batch(redis, cursors, true);
                if (!matching_notification_generation(heads, current_generation)) {
                    continue;
                }
                for (std::size_t index = 0; index < streams.size(); ++index) {
                    accept_notification_cursor(cursors[index], heads.array()[1].array()[index]);
                }
                generation = std::move(current_generation);
                service::log_ingest::fanout::get_hub(context).mark_ready();
                signals.notify_all();
                report_waits.recover();
                initialized = true;
            }
            if (interrupted) {
                const std::vector<std::string> scopes{"*"};
                co_await redis_snapshot_cache::invalidate(redis, scopes);
                recover_subscriptions(context);
                recovered_at = unix_milliseconds();
                interrupted = false;
                service::logging::info("Resource notifications resumed; subscriptions read recovery snapshots");
            }
            const auto batch = co_await read_notification_batch(redis, cursors, false);
            if (!matching_notification_generation(batch, generation)) {
                initialized = false;
                interrupted = true;
                continue;
            }
            bool active{};
            for (const auto& stream_value : batch.array()[1].array()) {
                if (stream_value.kind() != ruvia::redis_value::kind_type::array || stream_value.array().size() != 5) {
                    throw std::runtime_error("invalid combined notification stream");
                }
                const auto stream = stream_value.array();
                if (stream[0].kind() != ruvia::redis_value::kind_type::string ||
                    stream[1].kind() != ruvia::redis_value::kind_type::array) {
                    throw std::runtime_error("invalid combined notification stream fields");
                }
                std::size_t stream_index{};
                while (stream_index < streams.size() && streams[stream_index] != stream[0].string()) {
                    ++stream_index;
                }
                if (stream_index == streams.size()) {
                    throw std::runtime_error("unknown worker notification stream");
                }
                if (stream[4].kind() != ruvia::redis_value::kind_type::integer) {
                    throw std::runtime_error("invalid notification gap flag");
                }
                if (stream[4].integer() == 1) {
                    // Only the stream that lost unread entries recovers. A
                    // resource gap also invalidates reusable snapshots and
                    // wakes configuration watches; work/receipt gaps never
                    // manufacture unrelated SSE updates.
                    if (stream_index == 0) {
                        const std::vector<std::string> scopes{"*"};
                        co_await redis_snapshot_cache::invalidate(redis, scopes);
                        get_hub(context).recover();
                        service::node_dispatch::fanout::get_hub(context).publish_all();
                    } else if (stream_index == 1) {
                        service::log_ingest::fanout::get_hub(context).recover();
                    } else if (stream_index == 2) {
                        signals.notify_all();
                    } else {
                        report_waits.recover();
                    }
                    active = true;
                }
                for (const auto& entry : stream[1].array()) {
                    active = true;
                    const auto id = service::log_ingest::notifications::entry_id(entry);
                    if (!id || entry.array()[1].kind() != ruvia::redis_value::kind_type::array) {
                        throw std::runtime_error("invalid worker notification entry");
                    }
                    if (stream_index == 0) {
                        if (const auto notification = notifications::parse_fields(entry.array()[1].array())) {
                            deliver_notification(context, *notification, recovered_at);
                        }
                    } else if (stream_index == 1) {
                        if (const auto notification = service::log_ingest::notifications::parse_entry(entry)) {
                            service::log_ingest::fanout::deliver(context, *notification);
                        }
                    } else {
                        const auto fields = entry.array()[1].array();
                        if (fields.size() % 2 != 0) {
                            throw std::runtime_error("invalid worker signal fields");
                        }
                        std::optional<std::string_view> value;
                        const auto name = stream_index == 2 ? "type" : "report_id";
                        for (std::size_t index = 0; index < fields.size(); index += 2) {
                            if (fields[index].kind() != ruvia::redis_value::kind_type::string ||
                                fields[index + 1].kind() != ruvia::redis_value::kind_type::string) {
                                throw std::runtime_error("invalid worker signal field type");
                            }
                            if (fields[index].string() == name) {
                                value = fields[index + 1].string();
                            }
                        }
                        if (!value || value->empty()) {
                            throw std::runtime_error("missing worker signal field");
                        }
                        if (stream_index == 2) {
                            signals.for_type(*value).notify();
                        } else {
                            report_waits.publish(*value);
                        }
                    }
                }
                accept_notification_cursor(cursors[stream_index], stream_value);
            }
            if (active) {
                continue;
            }
            // The only blocking Redis operation on this Worker. Its reply is
            // accepted by the following atomic batch, or covered by a recovery
            // snapshot if Redis trimmed it while the Worker was suspended.
            const auto reply = co_await redis.command("XREAD", "BLOCK", "15000", "COUNT", "1", "STREAMS",
                streams[0], streams[1], streams[2], streams[3], cursors[0].id_, cursors[1].id_, cursors[2].id_, cursors[3].id_);
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

// Each Web Worker follows committed notifications for its own subscriptions.
template <typename context_type>
ruvia::task<void> run_notification_reader(context_type& context) {
    co_await read_notifications(context);
}

}  // namespace service::live_resource
