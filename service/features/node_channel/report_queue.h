#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/sha256.h"
#include "node/proto/edge_control.pb.h"
#include "service/domains/agent/agent_protocol.h"
#include "service/domains/agent/agent_types.h"
#include "service/features/attempt_trace/recorder.h"
#include "service/features/background/work_available.h"
#include "service/features/node_channel/report_waits.h"
#include "service/features/node_channel/store.h"
#include "service/utils/redis_reply.h"

namespace service::node_channel {

struct report final {
    service::agent::agent_principal principal_;
    std::string verified_hash_;
    std::int64_t received_unix_ms_{};
    flexedge::node::v2::ClientEnvelope envelope_;
    std::optional<std::int64_t> elapsed_milliseconds_;
    std::optional<service::attempt_trace::stored_trace> trace_;
};

namespace report_queue {

inline constexpr std::string_view stream_key{"flexedge:node-channel:v1:reports"};
inline constexpr std::string_view bytes_key{"flexedge:node-channel:v1:reports:bytes"};
inline constexpr std::string_view retry_key{"flexedge:node-channel:v1:reports:retry"};
inline constexpr std::string_view attempts_key{"flexedge:node-channel:v1:reports:attempts"};
inline constexpr std::string_view positions_key{"flexedge:node-channel:v1:reports:positions"};
inline constexpr std::string_view failed_key{"flexedge:node-channel:v1:reports:failed"};
inline constexpr std::string_view failed_bytes_key{"flexedge:node-channel:v1:reports:failed:bytes"};
inline constexpr std::string_view cleanup_key{"flexedge:node-channel:v1:reports:cleanup"};
inline constexpr std::string_view cleanup_identity_key{"flexedge:node-channel:v1:reports:cleanup-identity"};
inline constexpr std::string_view group{"flexedge-node-reports-v1"};
inline constexpr std::int64_t max_entries{100000};
inline constexpr std::int64_t max_bytes{256 * 1024 * 1024};
inline constexpr std::int64_t max_failed_entries{10000};
inline constexpr std::int64_t max_failed_bytes{64 * 1024 * 1024};
inline constexpr std::size_t max_payload_bytes{2 * 1024 * 1024};
inline constexpr std::int64_t abandoned_idle_ms{5 * 60 * 1000};

inline std::int64_t integer(std::string_view text) {
    std::int64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0) {
        throw std::runtime_error("invalid node report integer");
    }
    return value;
}

inline bool valid_id(std::string_view id) {
    return flexedge::crypto::is_sha256_digest(id);
}

inline std::string receipt_key(std::string_view id) {
    if (!valid_id(id)) {
        throw std::invalid_argument("invalid node report receipt id");
    }
    return "flexedge:node-channel:v1:report-receipt:" + std::string(id);
}

inline bool valid_report(const report& value) {
    const auto& identity = value.principal_;
    if (!service::common::parse_uuid(identity.tenant_id_) || !service::common::parse_uuid(identity.node_id_) ||
        !service::common::parse_uuid(identity.cluster_id_) || identity.agent_id_.empty() ||
        !flexedge::crypto::is_sha256_digest(value.verified_hash_) || value.received_unix_ms_ <= 0 || value.received_unix_ms_ > 253402300799999 ||
        !service::agent::valid_request_id(value.envelope_.request_id()) ||
        value.envelope_.ByteSizeLong() > max_payload_bytes ||
        (value.elapsed_milliseconds_ && *value.elapsed_milliseconds_ < 0)) {
        return false;
    }
    if (value.envelope_.has_authenticate()) {
        if (!value.envelope_.authenticate().secret().empty() ||
            value.envelope_.authenticate().node_id() != identity.agent_id_) {
            return false;
        }
    } else if (value.envelope_.has_heartbeat()) {
        if (value.envelope_.heartbeat().node_id() != identity.node_id_ ||
            !service::agent::valid_heartbeat(value.envelope_.heartbeat())) {
            return false;
        }
    } else if (value.envelope_.has_apply_result()) {
        if (value.envelope_.apply_result().node_id() != identity.node_id_ ||
            !service::agent::valid_apply_result(value.envelope_.apply_result())) {
            return false;
        }
    } else {
        return false;
    }
    return !value.trace_ || (value.envelope_.has_apply_result() && value.trace_->trace_bytes_ >= 0 && value.trace_->trace_bytes_ <= 262144 &&
                                value.trace_->summary_json_.size() <= max_payload_bytes && value.trace_->trace_json_.size() <= max_payload_bytes);
}

inline std::string encode(const report& value) {
    if (!valid_report(value)) {
        throw std::invalid_argument("invalid immutable node report");
    }
    const auto envelope = value.envelope_.SerializeAsString();
    const auto received = std::to_string(value.received_unix_ms_);
    const auto elapsed = value.elapsed_milliseconds_ ? std::to_string(*value.elapsed_milliseconds_) : std::string{};
    const auto trace_bytes = value.trace_ ? std::to_string(value.trace_->trace_bytes_) : std::string{};
    const std::array<std::string_view, 12> fields{"1", value.principal_.tenant_id_, value.principal_.node_id_,
        value.principal_.cluster_id_, value.principal_.agent_id_, value.verified_hash_, received, envelope, elapsed,
        value.trace_ ? std::string_view{value.trace_->summary_json_} : std::string_view{},
        value.trace_ ? std::string_view{value.trace_->trace_json_} : std::string_view{}, trace_bytes};
    std::string output;
    for (const auto field : fields) {
        output += std::to_string(field.size()) + ":";
        output += field;
    }
    if (output.size() > max_payload_bytes) {
        throw std::invalid_argument("node report exceeds byte budget");
    }
    return output;
}

inline std::optional<report> decode(std::string_view payload) {
    try {
        if (payload.size() > max_payload_bytes) {
            return std::nullopt;
        }
        std::array<std::string_view, 12> fields;
        for (auto& field : fields) {
            const auto colon = payload.find(':');
            if (colon == std::string_view::npos) {
                return std::nullopt;
            }
            const auto length = integer(payload.substr(0, colon));
            payload.remove_prefix(colon + 1);
            if (static_cast<std::size_t>(length) > payload.size()) {
                return std::nullopt;
            }
            field = payload.substr(0, length);
            payload.remove_prefix(length);
        }
        if (!payload.empty() || fields[0] != "1") {
            return std::nullopt;
        }
        report value{.principal_ = {.node_id_ = std::string(fields[2]), .cluster_id_ = std::string(fields[3]), .tenant_id_ = std::string(fields[1]), .agent_id_ = std::string(fields[4])},
            .verified_hash_ = std::string(fields[5]),
            .received_unix_ms_ = integer(fields[6]),
            .envelope_ = {},
            .elapsed_milliseconds_ = std::nullopt,
            .trace_ = std::nullopt};
        if (!value.envelope_.ParseFromArray(fields[7].data(), static_cast<int>(fields[7].size()))) {
            return std::nullopt;
        }
        if (!fields[8].empty()) {
            value.elapsed_milliseconds_ = integer(fields[8]);
        }
        if (!fields[11].empty()) {
            value.trace_ = service::attempt_trace::stored_trace{
                std::string(fields[9]), std::string(fields[10]), integer(fields[11])};
        }
        if (!valid_report(value)) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline constexpr std::string_view initialize_script{R"lua(
local result = redis.pcall('XGROUP','CREATE',KEYS[1],ARGV[1],'0','MKSTREAM')
if type(result)=='table' and result.err and not string.find(result.err,'BUSYGROUP',1,true) then
    return redis.error_reply(result.err)
end
return 1
)lua"};

inline constexpr std::string_view enqueue_script{R"lua(
local wake_type = redis.call('TYPE',KEYS[5]).ok
if wake_type ~= 'none' and wake_type ~= 'stream' then return redis.error_reply('invalid work wakeup key type') end
local positions_type = redis.call('TYPE',KEYS[6]).ok
if positions_type ~= 'none' and positions_type ~= 'hash' then return redis.error_reply('invalid node report positions key type') end
if redis.call('HGET',KEYS[1],'tenant_id') ~= ARGV[1] or
   redis.call('HGET',KEYS[1],'node_id') ~= ARGV[2] or
   redis.call('HGET',KEYS[1],'cluster_id') ~= ARGV[3] or
   redis.call('HGET',KEYS[1],'secret_hash') ~= ARGV[4] or
   redis.call('HGET',KEYS[1],'agent_id') ~= ARGV[8] then return 0 end
local status = redis.call('HGET',KEYS[1],'status')
if status ~= 'pending' and status ~= 'registered' then return 0 end
local clock=redis.call('TIME')
local now=tonumber(clock[1])*1000+math.floor(tonumber(clock[2])/1000)
if tonumber(ARGV[9]) > now+5000 then return 0 end
local byte_text = redis.call('GET',KEYS[3]) or '0'
if not string.match(byte_text,'^%d+$') then return redis.error_reply('invalid node report byte counter') end
local bytes = tonumber(byte_text)
local retained = string.len(ARGV[5])+string.len(ARGV[10])+128
if redis.call('XLEN',KEYS[2]) + redis.call('ZCARD',KEYS[4]) >= tonumber(ARGV[6]) or
   retained > tonumber(ARGV[7]) - bytes then return 0 end
local id=redis.call('XADD',KEYS[2],'*','payload',ARGV[5],'report_id',ARGV[10])
redis.call('HSET',KEYS[6],ARGV[10],id)
redis.call('INCRBY',KEYS[3],retained)
redis.call('XADD',KEYS[5],'MAXLEN','~',10000,'*','type','node-reports')
return ARGV[10]
)lua"};

inline constexpr std::string_view fence_script{R"lua(
local pending = redis.call('XPENDING',KEYS[1],ARGV[1],ARGV[2],ARGV[2],1)
if #pending ~= 1 or pending[1][2] ~= ARGV[3] or #redis.call('XRANGE',KEYS[1],ARGV[2],ARGV[2],'COUNT',1) ~= 1 then return 0 end
local fields=redis.call('XRANGE',KEYS[1],ARGV[2],ARGV[2],'COUNT',1)[1][2]
local report_id=''
for i=1,#fields,2 do if fields[i]=='report_id' then report_id=fields[i+1] end end
if report_id ~= ARGV[4] then return 0 end
redis.call('XCLAIM',KEYS[1],ARGV[1],ARGV[3],0,ARGV[2],'JUSTID')
return 1
)lua"};

inline constexpr std::string_view claim_script{R"lua(
local clock = redis.call('TIME')
local now = tonumber(clock[1])*1000 + math.floor(tonumber(clock[2])/1000)
local due = redis.call('ZRANGEBYSCORE',KEYS[2],'-inf',now,'LIMIT',0,1)
if #due > 0 then
    local entries = redis.call('XCLAIM',KEYS[1],ARGV[1],ARGV[2],0,due[1])
    redis.call('ZREM',KEYS[2],due[1])
    return entries
end
local pending = redis.call('XPENDING',KEYS[1],ARGV[1],'IDLE',ARGV[3],'-','+',1)
if #pending > 0 then
    local entries = redis.call('XCLAIM',KEYS[1],ARGV[1],ARGV[2],ARGV[3],pending[1][1])
    redis.call('ZREM',KEYS[2],pending[1][1])
    return entries
end
return {}
)lua"};

inline constexpr std::string_view retry_script{R"lua(
local pending = redis.call('XPENDING',KEYS[1],ARGV[1],ARGV[2],ARGV[2],1)
if #pending ~= 1 or pending[1][2] ~= ARGV[3] then return 0 end
local entries=redis.call('XRANGE',KEYS[1],ARGV[2],ARGV[2],'COUNT',1)
if #entries ~= 1 then return 0 end
local report_id=''
for i=1,#entries[1][2],2 do if entries[1][2][i]=='report_id' then report_id=entries[1][2][i+1] end end
if report_id ~= ARGV[4] then return 0 end
local attempt = redis.call('HINCRBY',KEYS[3],ARGV[2],1)
local delay = math.min(60000,1000*2^math.min(attempt-1,16))
local clock = redis.call('TIME')
local now = tonumber(clock[1])*1000 + math.floor(tonumber(clock[2])/1000)
redis.call('ZADD',KEYS[2],now+delay,ARGV[2])
redis.call('XCLAIM',KEYS[1],ARGV[1],ARGV[3],0,ARGV[2],'JUSTID')
return delay
)lua"};

// Validate every mutable key before the first mutation: Redis Lua errors do
// not roll back preceding commands. Cleanup metadata remains budgeted until
// its PostgreSQL ledger was safely deleted under the node row lock.
inline constexpr std::string_view complete_script{R"lua(
local kinds = {'stream','string','zset','hash','string','stream','zset','hash','stream','string','stream','hash'}
for i,kind in ipairs(kinds) do
    local actual = redis.call('TYPE',KEYS[i]).ok
    if actual ~= 'none' and actual ~= kind then return redis.error_reply('invalid node report key type') end
end
local pending = redis.call('XPENDING',KEYS[1],ARGV[1],ARGV[2],ARGV[2],1)
if #pending ~= 1 or pending[1][2] ~= ARGV[3] then return -1 end
local entries = redis.call('XRANGE',KEYS[1],ARGV[2],ARGV[2],'COUNT',1)
if #entries ~= 1 then return -1 end
local payload = ''
local report_id=''
for i=1,#entries[1][2],2 do
    if entries[1][2][i]=='payload' then payload=entries[1][2][i+1] end
    if entries[1][2][i]=='report_id' then report_id=entries[1][2][i+1] end
end
if report_id ~= ARGV[9] then return -1 end
local byte_text = redis.call('GET',KEYS[2]) or '0'
local failed_byte_text = redis.call('GET',KEYS[10]) or '0'
if not string.match(byte_text,'^%d+$') or not string.match(failed_byte_text,'^%d+$') then
    return redis.error_reply('invalid node report byte counter')
end
local reserved = 0
if ARGV[6] ~= '' then reserved = string.len(ARGV[2])+string.len(ARGV[6])+128 end
local remaining = tonumber(byte_text)-string.len(payload)-string.len(report_id)-128+reserved
if ARGV[4]=='0' then
    local failed_bytes = string.len(payload)+string.len(ARGV[5])+string.len(ARGV[2])+64
    if redis.call('XLEN',KEYS[9]) >= tonumber(ARGV[7]) or
       failed_bytes > tonumber(ARGV[8])-tonumber(failed_byte_text) then return 0 end
    redis.call('XADD',KEYS[9],'*','source_id',ARGV[2],'payload',payload,'reason',ARGV[5],'retained_bytes',failed_bytes)
    redis.call('INCRBY',KEYS[10],failed_bytes)
end
if ARGV[6] ~= '' then
    redis.call('HSET',KEYS[8],ARGV[2],ARGV[6])
    redis.call('ZADD',KEYS[7],0,ARGV[2])
end
redis.call('SET',KEYS[5],ARGV[4],'EX',300)
redis.call('XADD',KEYS[6],'MAXLEN','~',10000,'*','report_id',report_id)
if ARGV[4]=='1' then redis.call('XADD',KEYS[11],'MAXLEN','~',10000,'*','type','outbox') end
redis.call('XACK',KEYS[1],ARGV[1],ARGV[2])
redis.call('XDEL',KEYS[1],ARGV[2])
redis.call('HDEL',KEYS[12],report_id)
if remaining > 0 then redis.call('SET',KEYS[2],remaining) else redis.call('DEL',KEYS[2]) end
redis.call('ZREM',KEYS[3],ARGV[2])
redis.call('HDEL',KEYS[4],ARGV[2])
return 1
)lua"};

struct entry final {
    std::string id_;
    std::string payload_;
    std::string report_id_;
};

inline entry parse_entry(const ruvia::redis_value& value) {
    if (value.kind() != ruvia::redis_value::kind_type::array || value.array().size() != 2) {
        throw std::runtime_error("invalid node report entry");
    }
    const auto parts = value.array();
    if (parts[0].kind() != ruvia::redis_value::kind_type::string ||
        parts[1].kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("invalid node report fields");
    }
    entry output{std::string(parts[0].string()), {}, {}};
    const auto fields = parts[1].array();
    for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
        if (fields[index].kind() != ruvia::redis_value::kind_type::string || fields[index + 1].kind() != ruvia::redis_value::kind_type::string) {
            throw std::runtime_error("invalid node report field");
        }
        if (fields[index].string() == "payload") {
            output.payload_ = fields[index + 1].string();
        }
        if (fields[index].string() == "report_id") {
            output.report_id_ = fields[index + 1].string();
        }
    }
    if (!valid_id(output.report_id_)) {
        throw std::runtime_error("node report identifier missing");
    }
    if (fields.size() != 4) {
        output.payload_.clear();
    }
    return output;
}

inline ruvia::task<void> initialize(ruvia::redis_handle redis) {
    const std::array keys{stream_key};
    const std::array args{group};
    service::utils::require_redis_success(co_await redis.eval(initialize_script, keys, args), "could not initialize node report queue");
}

inline ruvia::task<std::optional<entry>> claim(ruvia::redis_handle redis, std::string_view consumer) {
    const std::array keys{stream_key, retry_key};
    const auto idle = std::to_string(abandoned_idle_ms);
    const std::array<std::string_view, 3> args{group, consumer, idle};
    const auto reclaimed = co_await redis.eval(claim_script, keys, args);
    service::utils::require_redis_success(reclaimed, "could not reclaim node report");
    if (reclaimed.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("invalid node report claim");
    }
    if (!reclaimed.array().empty()) {
        co_return parse_entry(reclaimed.array().front());
    }
    const std::array streams{ruvia::redis_stream_read_view{.stream_ = stream_key, .id_ = ">"}};
    const auto result = co_await redis.xread_group(group, consumer, streams, {.count_ = 1});
    if (!result || result->streams().empty() || result->streams().front().entries().empty()) {
        co_return std::nullopt;
    }
    const auto& source = result->streams().front().entries().front();
    entry output{std::string(source.id()), {}, {}};
    for (const auto& field : source.fields()) {
        if (field.key() == "payload") {
            output.payload_ = field.value();
        }
        if (field.key() == "report_id") {
            output.report_id_ = field.value();
        }
    }
    if (!valid_id(output.report_id_)) {
        throw std::runtime_error("node report identifier missing");
    }
    if (source.fields().size() != 2) {
        output.payload_.clear();
    }
    co_return output;
}

inline ruvia::task<bool> owns(ruvia::redis_handle redis, std::string_view consumer, const entry& entry) {
    const std::array keys{stream_key};
    const std::array<std::string_view, 4> args{group, entry.id_, consumer, entry.report_id_};
    const auto result = co_await redis.eval(fence_script, keys, args);
    service::utils::require_redis_success(result, "could not verify node report owner");
    co_return result.integer() == 1;
}

inline ruvia::task<std::chrono::milliseconds> retry(ruvia::redis_handle redis, std::string_view consumer, const entry& entry) {
    const std::array keys{stream_key, retry_key, attempts_key};
    const std::array<std::string_view, 4> args{group, entry.id_, consumer, entry.report_id_};
    const auto result = co_await redis.eval(retry_script, keys, args);
    service::utils::require_redis_success(result, "could not schedule node report retry");
    co_return std::chrono::milliseconds(result.integer());
}

// -1: ownership lost; 0: failure archive full (source retained); 1: completed.
inline ruvia::task<std::int64_t> complete(ruvia::redis_handle redis, std::string_view consumer, const entry& entry,
    bool success, std::string_view reason, const service::agent::agent_principal* principal = nullptr) {
    const auto receipt = receipt_key(entry.report_id_);
    const auto identity = principal ? principal->tenant_id_ + "|" + principal->node_id_ + "|" + entry.report_id_ : std::string{};
    const auto count = std::to_string(max_failed_entries), bytes = std::to_string(max_failed_bytes);
    const std::array<std::string_view, 12> keys{stream_key, bytes_key, retry_key, attempts_key, receipt,
        completion_stream_key, cleanup_key, cleanup_identity_key, failed_key, failed_bytes_key, service::background::work_available::stream_key, positions_key};
    const std::array<std::string_view, 9> args{group, entry.id_, consumer, success ? "1" : "0", reason, identity, count, bytes, entry.report_id_};
    const auto result = co_await redis.eval(complete_script, keys, args);
    service::utils::require_redis_success(result, "could not complete node report");
    co_return result.integer();
}

inline ruvia::task<std::chrono::milliseconds> idle_wait(ruvia::redis_handle redis) {
    constexpr std::string_view script{R"lua(
local due = redis.call('ZRANGE',KEYS[1],0,0,'WITHSCORES')
if #due == 0 then return 15000 end
local clock=redis.call('TIME')
local now=tonumber(clock[1])*1000+math.floor(tonumber(clock[2])/1000)
return math.max(1,math.min(15000,tonumber(due[2])-now))
)lua"};
    const std::array keys{retry_key};
    const std::array<std::string_view, 0> args{};
    const auto result = co_await redis.eval(script, keys, args);
    service::utils::require_redis_success(result, "could not inspect node report retry deadline");
    co_return std::chrono::milliseconds(result.integer());
}

}  // namespace report_queue

inline ruvia::task<std::optional<std::string>> enqueue(ruvia::redis_handle redis, const report& value) {
    const auto payload = report_queue::encode(value);
    const auto report_id = service::utils::random_token();
    const auto identity = agent_key(value.principal_.agent_id_);
    const auto received = std::to_string(value.received_unix_ms_);
    const auto count = std::to_string(report_queue::max_entries), bytes = std::to_string(report_queue::max_bytes);
    const std::array<std::string_view, 6> keys{identity, report_queue::stream_key, report_queue::bytes_key, report_queue::cleanup_key, service::background::work_available::stream_key, report_queue::positions_key};
    const std::array<std::string_view, 10> args{value.principal_.tenant_id_, value.principal_.node_id_, value.principal_.cluster_id_,
        value.verified_hash_, payload, count, bytes, value.principal_.agent_id_, received, report_id};
    const auto result = co_await redis.eval(report_queue::enqueue_script, keys, args);
    service::utils::require_redis_success(result, "could not enqueue immutable node report");
    if (result.kind() == ruvia::redis_value::kind_type::integer && result.integer() == 0) {
        co_return std::nullopt;
    }
    if (result.kind() != ruvia::redis_value::kind_type::string) {
        throw std::runtime_error("invalid node report enqueue result");
    }
    co_return std::string(result.string());
}

inline ruvia::task<std::optional<bool>> completion(ruvia::redis_handle redis, std::string_view id) {
    const auto result = co_await redis.command("GET", report_queue::receipt_key(id));
    service::utils::require_redis_success(result, "could not read node report completion");
    if (result.null()) {
        co_return std::nullopt;
    }
    if (result.kind() != ruvia::redis_value::kind_type::string || (result.string() != "0" && result.string() != "1")) {
        throw std::runtime_error("invalid node report completion");
    }
    co_return result.string() == "1";
}

}  // namespace service::node_channel
