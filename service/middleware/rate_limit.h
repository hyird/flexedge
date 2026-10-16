#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/next.h>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

#include "service/common/http.h"
#include "service/config/rate_limit.h"
#include "service/features/auth_session/principal.h"
#include "service/features/rate_limit/store.h"
#include "service/utils/token.h"

namespace service::middleware {

inline ruvia::task<void> require_request_budget(ruvia::context& context,
    std::span<const service::rate_limit::bucket> buckets) {
    service::rate_limit::decision decision{};
    try {
        decision = co_await service::rate_limit::try_consume(context.redis(), buckets,
            {.timeout_ = std::chrono::seconds(2), .stop_token_ = context.get_stop_token()});
    } catch (const service::rate_limit::unavailable&) {
        service::common::throw_app_error(service::common::server_error_code, "请求配额服务暂时不可用，请稍后重试", 503);
    }
    if (!decision.allowed_) {
        const auto seconds = std::max<std::int64_t>(1, (decision.retry_after_.count() + 999) / 1000);
        context.header("Retry-After", std::to_string(seconds));
        service::common::throw_app_error(service::common::unknown_error_code, "请求过于频繁，请稍后重试", 429);
    }
}

// Called only after the session was checked against the database. Rate state
// cannot grant an identity, bypass revocation or replace authorization.
inline ruvia::task<void> require_principal_budget(ruvia::context& context,
    const service::auth::authenticated_principal& principal) {
    const auto& config = service::config::rate_limits();
    const auto tenant = service::utils::token_hash(principal.system_tenant_id_);
    const auto prefix = "flexedge:rate:{api:tenant:" + tenant + "}:";
    const std::array buckets{
        service::rate_limit::bucket{prefix + "tenant", config.api_tenant_},
        service::rate_limit::bucket{prefix + "admin:" + service::utils::token_hash(principal.admin_id_), config.api_admin_},
    };
    co_await require_request_budget(context, buckets);
}

class request_rate_limit_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        // Route identity is already normalized by Ruvia. Inspecting only the
        // raw request path would let percent-encoded route segments bypass the
        // address or authentication bucket while still reaching the handler.
        const auto route = context.req().route_path();
        const auto path = route.empty() || route == "*" ? context.req().path() : route;
        if (!path.starts_with("/api/") || path.starts_with("/api/agent/") || path == "/api/agent") {
            co_await next();
            co_return;
        }
        const auto& config = service::config::rate_limits();
        // conn().client() is derived by Ruvia from the transport and configured
        // trusted proxies, never directly from a user-supplied forwarding header.
        const auto address = service::utils::token_hash(context.conn().client().address());
        const bool auth = path == "/api/auth/login" || path == "/api/auth/refresh";
        const std::array buckets{
            service::rate_limit::bucket{"flexedge:rate:{api:address}:global", config.api_global_},
            service::rate_limit::bucket{"flexedge:rate:{api:address}:ip:" + address, config.api_address_},
            service::rate_limit::bucket{"flexedge:rate:{api:address}:auth:" + address, config.auth_address_},
        };
        co_await require_request_budget(context, std::span(buckets).first(auth ? 3 : 2));
        co_await next();
    }
};

}  // namespace service::middleware
