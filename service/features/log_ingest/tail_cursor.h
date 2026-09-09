#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "service/common/uuid.h"

namespace service::log_ingest {
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

} // namespace service::log_ingest
