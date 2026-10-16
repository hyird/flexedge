#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/utils/redis_reply.h"

namespace service::background::stream_consumers {

inline constexpr auto cleanup_interval = std::chrono::seconds(60);
inline constexpr auto stale_after = std::chrono::minutes(5);

// Inspection and deletion share one atomic operation: a concurrent claim can
// never gain a pending entry between the check and DELCONSUMER. Only empty
// identities are removed; abandoned pending entries stay available to reclaim.
inline constexpr std::string_view remove_script{R"lua(
local consumers=redis.pcall('XINFO','CONSUMERS',KEYS[1],ARGV[1])
if consumers.err then
    if string.find(consumers.err,'NOGROUP',1,true) or string.find(consumers.err,'no such key',1,true) then return 0 end
    return redis.error_reply(consumers.err)
end
local removed=0
for _,fields in ipairs(consumers) do
    local name,pending,idle
    for i=1,#fields,2 do
        if fields[i]=='name' then name=fields[i+1]
        elseif fields[i]=='pending' then pending=tonumber(fields[i+1])
        elseif fields[i]=='idle' then idle=tonumber(fields[i+1]) end
    end
    local eligible=name==ARGV[2]
    if ARGV[3]=='idle' then eligible=name~=ARGV[2] and idle and idle>=tonumber(ARGV[4]) end
    if name and pending==0 and eligible then
        redis.call('XGROUP','DELCONSUMER',KEYS[1],ARGV[1],name)
        removed=removed+1
        if removed>=tonumber(ARGV[5]) then break end
    end
end
return removed
)lua"};

inline ruvia::task<std::size_t> remove_idle(ruvia::redis_handle redis, std::string_view stream,
    std::string_view group, std::string_view current_consumer,
    std::chrono::milliseconds minimum_idle = stale_after) {
    if (minimum_idle < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("consumer idle duration must not be negative");
    }
    const auto idle = std::to_string(minimum_idle.count());
    const std::array keys{stream};
    const std::array<std::string_view, 5> args{group, current_consumer, "idle", idle, "64"};
    const auto result = co_await redis.eval(remove_script, keys, args);
    service::utils::require_redis_success(result, "could not remove idle stream consumers");
    co_return static_cast<std::size_t>(result.integer());
}

// Called after draining available work, before waiting. This executes while the
// owning Worker's clients are still open, rather than attempting I/O after stop.
// A scheduled retry still has a pending entry and therefore keeps its identity.
inline ruvia::task<bool> remove_empty(ruvia::redis_handle redis, std::string_view stream,
    std::string_view group, std::string_view consumer) {
    const std::array keys{stream};
    const std::array<std::string_view, 5> args{group, consumer, "empty", "0", "1"};
    const auto result = co_await redis.eval(remove_script, keys, args);
    service::utils::require_redis_success(result, "could not remove drained stream consumer");
    co_return result.integer() == 1;
}

}  // namespace service::background::stream_consumers
