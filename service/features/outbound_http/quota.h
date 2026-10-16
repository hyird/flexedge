#pragma once

#include <ruvia/core/task.h>

#include <array>
#include <string>
#include <string_view>

#include "service/config/rate_limit.h"
#include "service/features/rate_limit/store.h"
#include "service/utils/token.h"

namespace service::outbound_http {

struct quota_scope final {
    std::string tenant_id_;
    std::string account_id_;
};

inline std::array<service::rate_limit::bucket, 2> quota_buckets(std::string_view origin,
    const quota_scope& scope, const service::config::rate_limit_config& config) {
    if (origin.empty() || origin.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string_view::npos ||
        scope.tenant_id_.empty() || scope.account_id_.empty()) {
        throw std::invalid_argument("outbound quota requires an origin, tenant and provider account");
    }
    const auto prefix = "flexedge:rate:{outbound:" + std::string(origin) + "}:";
    // Hash even the account identifier: AccessKey IDs and account emails must
    // never become Redis keys, and neither credentials nor request URLs enter
    // this namespace. The length prefix makes the tenant/account pair unambiguous.
    const auto identity = std::to_string(scope.tenant_id_.size()) + ":" + scope.tenant_id_ + scope.account_id_;
    return {service::rate_limit::bucket{prefix + "global", config.outbound_global_},
        service::rate_limit::bucket{prefix + "account:" + service::utils::token_hash(identity), config.outbound_account_}};
}

template <typename runtime_type>
ruvia::task<void> acquire_quota(runtime_type& context, std::string_view origin, const quota_scope& scope) {
    const auto& config = service::config::rate_limits();
    const auto buckets = quota_buckets(origin, scope, config);
    co_await service::rate_limit::acquire(context.redis(), context.worker(), buckets,
        config.outbound_max_wait_, context.get_stop_token());
}

}  // namespace service::outbound_http
