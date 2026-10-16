#pragma once

#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/web/redis/redis_handle.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/config/rate_limit.h"

namespace service::rate_limit {

struct bucket final {
    std::string key_;
    service::config::rate_bucket_policy policy_;
};

struct decision final {
    bool allowed_;
    std::chrono::milliseconds retry_after_;
};

class unavailable final : public std::runtime_error {
public:
    unavailable()
        : std::runtime_error("共享限流服务暂时不可用，请稍后重试") {}
};

class cancelled final : public std::runtime_error {
public:
    cancelled()
        : std::runtime_error("配额等待已取消") {}
};

class exhausted final : public std::runtime_error {
public:
    exhausted()
        : std::runtime_error("第三方服务请求配额繁忙，将稍后重试") {}
};

// All buckets are checked and consumed together, so a denied account never
// consumes the global budget. Redis TIME avoids clocks differing by worker or
// host. Idle keys expire after a full refill; denied traffic does not extend
// their lifetime. Every key in a call must have the same Redis cluster hash tag.
inline constexpr std::string_view consume_script = R"lua(
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
local states = {}
local retry = 0
for index, key in ipairs(KEYS) do
    local capacity = tonumber(ARGV[(index - 1) * 2 + 1])
    local window = tonumber(ARGV[(index - 1) * 2 + 2])
    local state = redis.call('HMGET', key, 'tokens', 'at')
    local tokens = capacity
    local at = now
    if state[1] or state[2] then
        tokens = tonumber(state[1])
        at = tonumber(state[2])
        if not tokens or not at or tokens < 0 or at < 0 then
            return redis.error_reply('invalid rate bucket state')
        end
        tokens = math.min(capacity, tokens + math.max(0, now - at) * capacity / window)
        at = math.max(at, now)
    end
    states[index] = {tokens, at, window}
    if tokens < 1 then
        retry = math.max(retry, math.ceil((1 - tokens) * window / capacity))
    end
end
if retry > 0 then return {0, retry} end
for index, key in ipairs(KEYS) do
    local state = states[index]
    redis.call('HSET', key, 'tokens', tostring(state[1] - 1), 'at', tostring(state[2]))
    redis.call('PEXPIRE', key, state[3] + 1)
end
return {1, 0}
)lua";

inline ruvia::task<decision> try_consume(ruvia::redis_handle redis, std::span<const bucket> buckets,
    ruvia::operation_options options = {}) {
    if (buckets.empty() || buckets.size() > 4) {
        throw std::invalid_argument("a quota requires between one and four buckets");
    }
    if (options.stop_token_.stop_requested()) {
        throw cancelled();
    }
    std::vector<std::string> arguments{"EVAL", std::string(consume_script), std::to_string(buckets.size())};
    for (const auto& bucket_value : buckets) {
        service::config::validate_rate_bucket(bucket_value.policy_);
        if (bucket_value.key_.empty() || bucket_value.key_.size() > 512) {
            throw std::invalid_argument("invalid rate bucket key");
        }
        arguments.push_back(bucket_value.key_);
    }
    for (const auto& bucket_value : buckets) {
        arguments.push_back(std::to_string(bucket_value.policy_.capacity_));
        arguments.push_back(std::to_string(bucket_value.policy_.window_.count()));
    }
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& argument : arguments) {
        views.push_back(argument);
    }
    try {
        const auto response = co_await redis.with_options(options).command(views);
        if (response.kind() != ruvia::redis_value::kind_type::array || response.array().size() != 2) {
            throw unavailable();
        }
        const auto values = response.array();
        if (values[0].kind() != ruvia::redis_value::kind_type::integer ||
            values[1].kind() != ruvia::redis_value::kind_type::integer ||
            (values[0].integer() != 0 && values[0].integer() != 1) || values[1].integer() < 0 ||
            values[1].integer() > std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(24)).count()) {
            throw unavailable();
        }
        co_return decision{.allowed_ = values[0].integer() == 1,
            .retry_after_ = std::chrono::milliseconds(values[1].integer())};
    } catch (...) {
        if (options.stop_token_.stop_requested()) {
            throw cancelled();
        }
        // Fail closed. Do not expose Redis transport details in task errors or
        // accidentally continue without a shared budget during an outage.
        throw unavailable();
    }
}

inline ruvia::task<void> acquire(ruvia::redis_handle redis, const ruvia::worker_handle& worker,
    std::span<const bucket> buckets, std::chrono::milliseconds max_wait, ruvia::stop_token stop) {
    if (max_wait <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("quota wait must have a positive bound");
    }
    const auto deadline = std::chrono::steady_clock::now() + max_wait;
    while (true) {
        if (stop.stop_requested()) {
            throw cancelled();
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            throw exhausted();
        }
        const auto result = co_await try_consume(redis, buckets,
            {.timeout_ = std::min(remaining, std::chrono::milliseconds(2000)), .stop_token_ = stop});
        if (result.allowed_) {
            co_return;
        }
        if (result.retry_after_ >= remaining) {
            throw exhausted();
        }
        const auto waited = co_await ruvia::sleep_for(worker,
            std::max(result.retry_after_, std::chrono::milliseconds(1)), stop);
        if (waited != ruvia::timer_sleep_result::elapsed) {
            throw cancelled();
        }
    }
}

}  // namespace service::rate_limit
