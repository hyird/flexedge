#pragma once

#include <ruvia/web/dotenv.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace service::config {

// One token is consumed per operation; capacity tokens refill over window.
// These are deployment budgets, not a substitute for a provider's other quotas.
struct rate_bucket_policy final {
    std::int64_t capacity_;
    std::chrono::milliseconds window_;
};

struct rate_limit_config final {
    rate_bucket_policy outbound_global_{8, std::chrono::seconds(1)};
    rate_bucket_policy outbound_account_{2, std::chrono::seconds(1)};
    std::chrono::milliseconds outbound_max_wait_{5000};
    rate_bucket_policy api_global_{2048, std::chrono::seconds(1)};
    rate_bucket_policy api_address_{128, std::chrono::seconds(1)};
    rate_bucket_policy api_tenant_{512, std::chrono::seconds(1)};
    rate_bucket_policy api_admin_{64, std::chrono::seconds(1)};
    rate_bucket_policy auth_address_{16, std::chrono::minutes(1)};
};

inline void validate_rate_bucket(const rate_bucket_policy& policy) {
    if (policy.capacity_ <= 0 || policy.capacity_ > 1000000 || policy.window_.count() <= 0 ||
        policy.window_ > std::chrono::hours(24)) {
        throw std::invalid_argument("rate bucket capacity/window is outside the supported range");
    }
}

inline rate_limit_config get_rate_limit_config(const ruvia::env& env) {
    rate_limit_config config;
    const auto number = [&](std::string_view name, std::int64_t fallback) {
        const auto text = env.get(name);
        if (!text) {
            return fallback;
        }
        std::int64_t value{};
        const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
        if (error != std::errc{} || end != text->data() + text->size()) {
            throw std::invalid_argument(std::string(name) + " must be a decimal integer");
        }
        return value;
    };
    const auto read = [&](std::string_view prefix, rate_bucket_policy& policy) {
        const auto capacity_name = std::string(prefix) + "_CAPACITY";
        const auto window_name = std::string(prefix) + "_WINDOW_MS";
        policy.capacity_ = number(capacity_name, policy.capacity_);
        policy.window_ = std::chrono::milliseconds(number(window_name, policy.window_.count()));
        validate_rate_bucket(policy);
    };
    read("OUTBOUND_RATE_GLOBAL", config.outbound_global_);
    read("OUTBOUND_RATE_ACCOUNT", config.outbound_account_);
    read("API_RATE_GLOBAL", config.api_global_);
    read("API_RATE_ADDRESS", config.api_address_);
    read("API_RATE_TENANT", config.api_tenant_);
    read("API_RATE_ADMIN", config.api_admin_);
    read("AUTH_RATE_ADDRESS", config.auth_address_);
    config.outbound_max_wait_ = std::chrono::milliseconds(
        number("OUTBOUND_RATE_MAX_WAIT_MS", config.outbound_max_wait_.count()));
    if (config.outbound_max_wait_ <= std::chrono::milliseconds::zero() ||
        config.outbound_max_wait_ > std::chrono::minutes(1)) {
        throw std::invalid_argument("OUTBOUND_RATE_MAX_WAIT_MS must be between 1 and 60000");
    }
    return config;
}

inline std::optional<rate_limit_config>& rate_limit_storage() {
    static std::optional<rate_limit_config> config;
    return config;
}

// Set once before starting workers; every subsequent access is read-only.
inline void configure_rate_limits(rate_limit_config config) {
    auto& storage = rate_limit_storage();
    if (storage) {
        throw std::logic_error("rate limits already configured");
    }
    storage.emplace(std::move(config));
}

inline const rate_limit_config& rate_limits() {
    const auto& storage = rate_limit_storage();
    if (!storage) {
        throw std::logic_error("rate limits are not configured");
    }
    return *storage;
}

}  // namespace service::config
