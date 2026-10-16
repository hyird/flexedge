#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <array>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/utils/redis_reply.h"
#include "service/utils/token.h"

namespace service::background::work_available {

inline constexpr std::string_view stream_key{"flexedge:work-available:v1"};
inline constexpr std::string_view generation_key{"flexedge:worker-streams:v1:generation"};
inline constexpr std::int64_t max_entries{10000};
inline constexpr std::array<std::string_view, 7> types{
    "provider-verification", "dns-sync", "certificate", "website-dispatch", "log-ingest", "outbox", "node-reports"};

[[nodiscard]] inline bool valid_type(std::string_view type) noexcept {
    for (const auto candidate : types) {
        if (candidate == type) {
            return true;
        }
    }
    return false;
}

// Only a coalesced availability hint lives here. Payloads, leases, retries and
// receipts remain in Redis/PostgreSQL and are independently claimed there.
class signal final {
public:
    void bind(const ruvia::worker_handle& worker) {
        if (!worker.is_current() || (receiver_ && !receiver_->worker().is_current())) {
            throw std::logic_error("work signal must stay on its owning worker");
        }
        if (!receiver_) {
            auto channel = ruvia::make_channel<bool>(worker, {.capacity_ = 1});
            sender_.emplace(std::move(channel.first));
            receiver_.emplace(std::move(channel.second));
        }
    }

    void notify() {
        if (!receiver_ || !receiver_->worker().is_current()) {
            throw std::logic_error("work signal is not bound to this worker");
        }
        // Full means an equivalent hint is already waiting for this task.
        (void)sender_->send(true);
    }

    ruvia::task<void> wait(std::chrono::milliseconds maximum, ruvia::stop_token stop) {
        if (!receiver_) {
            throw std::logic_error("work signal is not bound");
        }
        if (maximum > std::chrono::milliseconds::zero()) {
            (void)co_await receiver_->receive_for(maximum, std::move(stop));
        }
    }

private:
    std::optional<ruvia::channel_sender<bool>> sender_;
    std::optional<ruvia::channel_receiver<bool>> receiver_;
};

class signals final {
public:
    void bind(const ruvia::worker_handle& worker) {
        for (auto& value : values_) {
            value.bind(worker);
        }
    }

    signal& for_type(std::string_view type) {
        for (std::size_t index = 0; index < types.size(); ++index) {
            if (types[index] == type) {
                return values_[index];
            }
        }
        throw std::invalid_argument("unsupported work availability type");
    }

    void notify_all() {
        for (auto& value : values_) {
            value.notify();
        }
    }

private:
    std::array<signal, types.size()> values_;
};

template <typename context_type>
signals& get_signals(context_type& context) {
    auto& value = context.template worker_state<signals>();
    value.bind(context.worker());
    return value;
}

template <typename context_type>
ruvia::task<void> wait(context_type& context, std::string_view type,
    std::chrono::milliseconds maximum = std::chrono::seconds(15)) {
    co_await get_signals(context).for_type(type).wait(maximum, context.get_stop_token());
}

inline ruvia::task<void> publish(ruvia::redis_handle redis, std::string_view type) {
    if (!valid_type(type)) {
        throw std::invalid_argument("unsupported work availability type");
    }
    const auto result = co_await redis.command("XADD", stream_key, "MAXLEN", "~", "10000", "*", "type", type);
    service::utils::require_redis_success(result, "could not publish work availability");
    if (result.kind() != ruvia::redis_value::kind_type::string) {
        throw std::runtime_error("invalid work availability reply");
    }
}

// This marker detects a Redis reset even when a pooled connection remains
// usable. Each reader keeps only the generation it last recovered, not data.
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
