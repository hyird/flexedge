#pragma once

#include <algorithm>
#include <array>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/features/log_ingest/tail_cursor.h"

namespace service::log_ingest {

inline constexpr std::array<std::int64_t, 3> kTailLimits{100, 1000, 10000};

inline std::int64_t requireTailLimit(ruvia::Context& c) {
    const auto raw = c.req().query("limit");
    if (!raw) {
        return kTailLimits.front();
    }
    const auto parsed = service::common::parseInt64(raw);
    if (!parsed || std::ranges::find(kTailLimits, *parsed) == kTailLimits.end()) {
        service::common::throwAppError(service::common::kValidationErrorCode,
                                       "limit 必须是 100、1000 或 10000", 400);
    }
    return *parsed;
}

inline std::optional<TailCursor> requireTailCursor(std::string_view raw) {
    const auto parsed = parseTailCursor(raw);
    if (!parsed) {
        service::common::throwAppError(service::common::kValidationErrorCode, "after 游标不正确",
                                       400);
    }
    return parsed;
}

inline std::optional<TailCursor> optionalSseTailCursor(ruvia::Context& c) {
    if (const auto raw = c.req().query("after")) {
        return requireTailCursor(*raw);
    }
    if (const auto raw = c.req().header("Last-Event-ID")) {
        return requireTailCursor(*raw);
    }
    return std::nullopt;
}


} // namespace service::log_ingest
