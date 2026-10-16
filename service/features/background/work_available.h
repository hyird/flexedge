#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/background/keyed_signal_hub.h"
#include "service/utils/redis_reply.h"
#include "service/utils/token.h"

namespace service::background::work_available {

inline constexpr std::string_view generation_key{"flexedge:worker-streams:v1:generation"};
// The claim loops a worker runs. The four marker loops are named: the sync
// marker type table refers to them.
inline constexpr std::string_view provider_verification_work{"provider-verification"};
inline constexpr std::string_view dns_sync_work{"dns-sync"};
inline constexpr std::string_view certificate_work{"certificate"};
inline constexpr std::string_view website_dispatch_work{"website-dispatch"};
inline constexpr std::array<std::string_view, 7> types{provider_verification_work, dns_sync_work, certificate_work,
    website_dispatch_work, "log-ingest", "outbox", "node-reports"};

[[nodiscard]] inline bool valid_type(std::string_view type) noexcept {
    for (const auto candidate : types) {
        if (candidate == type) {
            return true;
        }
    }
    return false;
}

// The calling worker's wakeups of its own claim loops, keyed by work type.
// Only a coalesced hint lives here: payloads, leases, retries and receipts
// remain in Redis/PostgreSQL and are independently claimed there.
struct claim_signals final : keyed_signal_hub<std::string_view> {};

// A claim loop subscribes before its first scan, so work committed after
// that scan on this worker wakes it; nothing committed earlier is missed.
template <typename context_type>
keyed_signal_hub<std::string_view>::subscription subscribe(context_type& context, std::string_view type) {
    for (const auto candidate : types) {
        if (candidate == type) {
            return context.template worker_state<claim_signals>().subscribe(context.worker(), candidate);
        }
    }
    throw std::invalid_argument("unsupported work availability type");
}

// Wakes the calling worker's own claim loops for work it just committed. No
// other worker is woken: they find the work at their next bounded scan or
// after its lease expires, so work of a busy or exited committer is still
// claimed.
template <typename context_type>
void wake_own_claims(context_type& context, std::span<const std::string_view> work_types) {
    if constexpr (requires { context.record_own_claims(work_types); }) {
        context.record_own_claims(work_types);
    } else {
        auto& own = context.template worker_state<claim_signals>();
        for (const auto type : work_types) {
            if (!valid_type(type)) {
                throw std::invalid_argument("unsupported work availability type");
            }
            own.publish(type);
        }
    }
}

// Detects a Redis reset even when a pooled connection remains usable. Each
// notification reader keeps only the generation it last recovered, not data.
inline ruvia::task<std::string> generation(ruvia::redis_handle redis) {
    const auto existing = co_await redis.command("GET", generation_key);
    service::utils::require_redis_success(existing, "could not inspect worker stream generation");
    if (existing.kind() == ruvia::redis_value::kind_type::string && !existing.string().empty()) {
        co_return std::string(existing.string());
    }
    if (!existing.null()) {
        throw std::runtime_error("invalid worker stream generation");
    }
    // Generate a fresh value for every recreation. Reusing this reader's
    // startup nonce would hide a reset if the same reader recreated the key.
    const auto nonce = service::utils::random_token();
    constexpr std::string_view script{R"lua(
redis.call('SET', KEYS[1], ARGV[1], 'NX')
return redis.call('GET', KEYS[1])
)lua"};
    const std::array keys{generation_key};
    const std::array<std::string_view, 1> arguments{nonce};
    const auto result = co_await redis.eval(script, keys, arguments);
    service::utils::require_redis_success(result, "could not inspect worker stream generation");
    if (result.kind() != ruvia::redis_value::kind_type::string || result.string().empty()) {
        throw std::runtime_error("invalid worker stream generation");
    }
    co_return std::string(result.string());
}

}  // namespace service::background::work_available
