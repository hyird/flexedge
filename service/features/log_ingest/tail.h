#pragma once

#include <ruvia/web/context.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/common/http.h"
#include "service/features/log_ingest/tail_cursor.h"

namespace service::log_ingest {

inline constexpr std::array<std::int64_t, 3> tail_limits{100, 1000, 10000};

inline std::int64_t require_tail_limit(ruvia::context& c) {
    const auto raw = c.req().query("limit");
    if (!raw) {
        return tail_limits.front();
    }
    const auto parsed = service::common::parse_int64(raw);
    if (!parsed || std::ranges::find(tail_limits, *parsed) == tail_limits.end()) {
        service::common::throw_app_error(service::common::validation_error_code,
            "limit 必须是 100、1000 或 10000", 400);
    }
    return *parsed;
}

inline std::optional<tail_cursor> require_tail_cursor(std::string_view raw) {
    const auto parsed = parse_tail_cursor(raw);
    if (!parsed) {
        service::common::throw_app_error(service::common::validation_error_code, "after 游标不正确",
            400);
    }
    return parsed;
}

inline std::optional<tail_cursor> optional_sse_tail_cursor(ruvia::context& c) {
    if (const auto raw = c.req().query("after")) {
        return require_tail_cursor(*raw);
    }
    if (const auto raw = c.req().header("Last-Event-ID")) {
        return require_tail_cursor(*raw);
    }
    return std::nullopt;
}

}  // namespace service::log_ingest
