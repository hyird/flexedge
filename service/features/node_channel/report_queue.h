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
#include "service/features/attempt_trace/recorder.h"
#include "service/features/background/reliable_stream.h"
#include "service/features/background/work_available.h"
#include "service/features/node_channel/protocol.h"
#include "service/features/node_channel/report_waits.h"
#include "service/features/node_channel/store.h"
#include "service/features/node_channel/types.h"
#include "service/utils/redis_reply.h"
#include "service/utils/text.h"
#include "service/utils/token.h"

namespace service::node_channel {

struct report final {
    agent_principal principal_;
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

inline constexpr service::background::reliable_stream::definition stream{
    .stream_ = stream_key,
    .group_ = group,
    .bytes_ = bytes_key,
    .retry_ = retry_key,
    .attempts_ = attempts_key,
    .failed_ = failed_key,
    .failed_bytes_ = failed_bytes_key,
    .max_failed_entries_ = max_failed_entries,
    .max_failed_bytes_ = max_failed_bytes,
    .retry_initial_ = std::chrono::seconds(1),
    .retry_maximum_ = std::chrono::seconds(60),
    .reclaim_idle_ = std::chrono::milliseconds(abandoned_idle_ms),
};

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
        !valid_request_id(value.envelope_.request_id()) ||
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
            !valid_heartbeat(value.envelope_.heartbeat())) {
            return false;
        }
    } else if (value.envelope_.has_apply_result()) {
        if (value.envelope_.apply_result().node_id() != identity.node_id_ ||
            !valid_apply_result(value.envelope_.apply_result())) {
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
        service::utils::append_length_prefixed(output, field);
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
            const auto parsed = service::utils::consume_length_prefixed(payload);
            if (!parsed) {
                return std::nullopt;
            }
            field = *parsed;
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

// The live budget accounts each report with its identifier and a fixed
// overhead; pending receipt cleanups count against the entry limit.
inline constexpr std::string_view enqueue_body{R"lua(
local positions_type = redis.call('TYPE',KEYS[5]).ok
if positions_type ~= 'none' and positions_type ~= 'hash' then return redis.error_reply('invalid node report positions key type') end
if redis.call('HGET',KEYS[1],'tenant_id') ~= ARGV[1] or
   redis.call('HGET',KEYS[1],'node_id') ~= ARGV[2] or
   redis.call('HGET',KEYS[1],'cluster_id') ~= ARGV[3] or
   redis.call('HGET',KEYS[1],'secret_hash') ~= ARGV[4] or
   redis.call('HGET',KEYS[1],'agent_id') ~= ARGV[8] then return 0 end
local status = redis.call('HGET',KEYS[1],'status')
if status ~= 'pending' and status ~= 'registered' then return 0 end
if tonumber(ARGV[9]) > reliable_now()+5000 then return 0 end
local retained = string.len(ARGV[5])+string.len(ARGV[10])+128
if not reliable_admit(KEYS[3], redis.call('XLEN',KEYS[2]) + redis.call('ZCARD',KEYS[4]),
       tonumber(ARGV[6]), tonumber(ARGV[7]), retained) then return 0 end
local id=redis.call('XADD',KEYS[2],'*','payload',ARGV[5],'report_id',ARGV[10])
redis.call('HSET',KEYS[5],ARGV[10],id)
redis.call('INCRBY',KEYS[3],retained)
return ARGV[10]
)lua"};

// An owned entry still carrying the claimed report identifier.
inline constexpr std::string_view fence_body{R"lua(
if not reliable_owned(KEYS[1],ARGV[1],ARGV[2],ARGV[3]) then return 0 end
local fields = reliable_fields(KEYS[1],ARGV[2])
if fields == nil or fields['report_id'] ~= ARGV[4] then return 0 end
redis.call('XCLAIM',KEYS[1],ARGV[1],ARGV[3],0,ARGV[2],'JUSTID')
return 1
)lua"};

inline constexpr std::string_view retry_body{R"lua(
if not reliable_owned(KEYS[1],ARGV[1],ARGV[2],ARGV[3]) then return 0 end
local fields = reliable_fields(KEYS[1],ARGV[2])
if fields == nil or fields['report_id'] ~= ARGV[4] then return 0 end
return reliable_retry(KEYS[1],ARGV[1],KEYS[2],KEYS[3],ARGV[2],ARGV[3],tonumber(ARGV[5]),tonumber(ARGV[6]))
)lua"};

// Validate every mutable key before the first mutation: Redis Lua errors do
// not roll back preceding commands. Cleanup metadata remains budgeted until
// its PostgreSQL ledger was safely deleted under the node row lock.
inline constexpr std::string_view complete_body{R"lua(
local kinds = {'stream','string','zset','hash','string','stream','zset','hash','stream','string','hash'}
for i,kind in ipairs(kinds) do
    local actual = redis.call('TYPE',KEYS[i]).ok
    if actual ~= 'none' and actual ~= kind then return redis.error_reply('invalid node report key type') end
end
reliable_counter(KEYS[2])
reliable_counter(KEYS[10])
if not reliable_owned(KEYS[1],ARGV[1],ARGV[2],ARGV[3]) then return -1 end
local fields = reliable_fields(KEYS[1],ARGV[2])
if fields == nil or fields['report_id'] ~= ARGV[9] then return -1 end
local payload = fields['payload'] or ''
local report_id = fields['report_id']
local reserved = 0
if ARGV[6] ~= '' then reserved = string.len(ARGV[2])+string.len(ARGV[6])+128 end
if ARGV[4]=='0' then
    local failed_bytes = string.len(payload)+string.len(ARGV[5])+string.len(ARGV[2])+64
    if not reliable_archive(KEYS[9],KEYS[10],tonumber(ARGV[7]),tonumber(ARGV[8]),failed_bytes,
           {'source_id',ARGV[2],'payload',payload,'reason',ARGV[5],'retained_bytes',tostring(failed_bytes)}) then
        return 0
    end
end
if ARGV[6] ~= '' then
    redis.call('HSET',KEYS[8],ARGV[2],ARGV[6])
    redis.call('ZADD',KEYS[7],0,ARGV[2])
end
redis.call('SET',KEYS[5],ARGV[4],'EX',300)
redis.call('XADD',KEYS[6],'MAXLEN','~',10000,'*','report_id',report_id)
reliable_remove(KEYS[1],ARGV[1],KEYS[3],KEYS[4],KEYS[2],ARGV[2],string.len(payload)+string.len(report_id)+128-reserved)
redis.call('HDEL',KEYS[11],report_id)
return 1
)lua"};

struct entry final {
    std::string id_;
    std::string payload_;
    std::string report_id_;
};

// A report entry carries exactly its payload and identifier; any other shape
// keeps the identifier but no payload, so it completes as invalid.
inline entry to_entry(service::background::reliable_stream::entry source) {
    entry output{std::move(source.id_), {}, {}};
    const bool complete = source.fields_.size() == 2;
    for (auto& [name, value] : source.fields_) {
        if (name == "payload") {
            output.payload_ = std::move(value);
        }
        if (name == "report_id") {
            output.report_id_ = std::move(value);
        }
    }
    if (!valid_id(output.report_id_)) {
        throw std::runtime_error("node report identifier missing");
    }
    if (!complete) {
        output.payload_.clear();
    }
    return output;
}

inline ruvia::task<void> initialize(ruvia::redis_handle redis) {
    co_await service::background::reliable_stream::initialize(redis, stream);
}

// A due retry first, then a report abandoned by a stopped claimant, then a
// new report.
inline ruvia::task<std::optional<entry>> claim(ruvia::redis_handle redis, std::string_view consumer) {
    if (auto claimed = co_await service::background::reliable_stream::claim_next(redis, stream, consumer)) {
        co_return to_entry(std::move(*claimed));
    }
    co_return std::nullopt;
}

inline ruvia::task<bool> owns(ruvia::redis_handle redis, std::string_view consumer, const entry& entry) {
    const std::array keys{stream_key};
    const std::array<std::string_view, 4> args{group, entry.id_, consumer, entry.report_id_};
    const auto result = co_await redis.eval(service::background::reliable_stream::script<fence_body>, keys, args);
    service::utils::require_redis_success(result, "could not verify node report owner");
    co_return result.integer() == 1;
}

inline ruvia::task<std::chrono::milliseconds> retry(ruvia::redis_handle redis, std::string_view consumer, const entry& entry) {
    const std::array keys{stream_key, retry_key, attempts_key};
    const auto initial = std::to_string(stream.retry_initial_.count());
    const auto maximum = std::to_string(stream.retry_maximum_.count());
    const std::array<std::string_view, 6> args{group, entry.id_, consumer, entry.report_id_, initial, maximum};
    const auto result = co_await redis.eval(service::background::reliable_stream::script<retry_body>, keys, args);
    service::utils::require_redis_success(result, "could not schedule node report retry");
    co_return std::chrono::milliseconds(result.integer());
}

// -1: ownership lost; 0: failure archive full (source retained); 1: completed.
inline ruvia::task<std::int64_t> complete(ruvia::redis_handle redis, std::string_view consumer, const entry& entry,
    bool success, std::string_view reason, const agent_principal* principal = nullptr) {
    const auto receipt = receipt_key(entry.report_id_);
    const auto identity = principal ? principal->tenant_id_ + "|" + principal->node_id_ + "|" + entry.report_id_ : std::string{};
    const auto count = std::to_string(max_failed_entries), bytes = std::to_string(max_failed_bytes);
    const std::array<std::string_view, 11> keys{stream_key, bytes_key, retry_key, attempts_key, receipt,
        completion_stream_key, cleanup_key, cleanup_identity_key, failed_key, failed_bytes_key, positions_key};
    const std::array<std::string_view, 9> args{group, entry.id_, consumer, success ? "1" : "0", reason, identity, count, bytes, entry.report_id_};
    const auto result = co_await redis.eval(service::background::reliable_stream::script<complete_body>, keys, args);
    service::utils::require_redis_success(result, "could not complete node report");
    co_return result.integer();
}

inline ruvia::task<std::chrono::milliseconds> idle_wait(ruvia::redis_handle redis) {
    co_return co_await service::background::reliable_stream::next_due(redis, stream, std::chrono::seconds(15));
}

}  // namespace report_queue

// The work type of the report claim loop.
inline constexpr std::string_view report_work_type{"node-reports"};

// The enqueuing Worker wakes its own report claim loop after enqueue; other
// Workers find the report at their next bounded scan.
template <typename context_type>
void wake_own_reports(context_type& context) {
    static constexpr std::array work{report_work_type};
    service::background::work_available::wake_own_claims(context, work);
}

inline ruvia::task<std::optional<std::string>> enqueue(ruvia::redis_handle redis, const report& value) {
    const auto payload = report_queue::encode(value);
    const auto report_id = service::utils::random_token();
    const auto identity = agent_key(value.principal_.agent_id_);
    const auto received = std::to_string(value.received_unix_ms_);
    const auto count = std::to_string(report_queue::max_entries), bytes = std::to_string(report_queue::max_bytes);
    const std::array<std::string_view, 5> keys{identity, report_queue::stream_key, report_queue::bytes_key, report_queue::cleanup_key, report_queue::positions_key};
    const std::array<std::string_view, 10> args{value.principal_.tenant_id_, value.principal_.node_id_, value.principal_.cluster_id_,
        value.verified_hash_, payload, count, bytes, value.principal_.agent_id_, received, report_id};
    const auto result = co_await redis.eval(service::background::reliable_stream::script<report_queue::enqueue_body>, keys, args);
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
