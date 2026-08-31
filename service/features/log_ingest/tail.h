#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Context.h>

#include "service/common/http.h"

namespace service::log_ingest {

inline constexpr std::array<std::int64_t, 3> kTailLimits{100, 1000, 10000};

struct TailCursor final {
    std::int64_t ingestedUnixMicros{};
    std::string id;
};

inline std::optional<TailCursor> parseTailCursor(std::string_view raw) {
    const auto separator = raw.find(':');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    std::int64_t ingestedUnixMicros{};
    const auto [end, error] =
        std::from_chars(raw.data(), raw.data() + separator, ingestedUnixMicros);
    auto id = service::common::parseUuid(raw.substr(separator + 1));
    if (error != std::errc{} || end != raw.data() + separator || ingestedUnixMicros <= 0 || !id) {
        return std::nullopt;
    }
    return TailCursor{.ingestedUnixMicros = ingestedUnixMicros, .id = std::move(*id)};
}

inline std::string encodeTailCursor(std::int64_t ingestedUnixMicros, std::string_view id) {
    return std::to_string(ingestedUnixMicros) + ":" + std::string(id);
}

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

inline bool sseClientDisconnected(const std::exception& error) {
    const std::string_view message{error.what()};
    return message.contains("Broken pipe") || message.contains("Connection reset by peer") ||
           message.contains("redis operation cancelled");
}

} // namespace service::log_ingest
