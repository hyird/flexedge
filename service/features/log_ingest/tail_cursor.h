#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/common/uuid.h"

namespace service::log_ingest {
struct tail_cursor final {
    std::int64_t ingested_unix_micros_{};
    std::string id_;
};

inline std::optional<tail_cursor> parse_tail_cursor(std::string_view raw) {
    const auto separator = raw.find(':');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    std::int64_t ingested_unix_micros{};
    const auto [end, error] =
        std::from_chars(raw.data(), raw.data() + separator, ingested_unix_micros);
    auto id = service::common::parse_uuid(raw.substr(separator + 1));
    if (error != std::errc{} || end != raw.data() + separator || ingested_unix_micros <= 0 || !id) {
        return std::nullopt;
    }
    return tail_cursor{.ingested_unix_micros_ = ingested_unix_micros, .id_ = std::move(*id)};
}

inline std::string encode_tail_cursor(std::int64_t ingested_unix_micros, std::string_view id) {
    return std::to_string(ingested_unix_micros) + ":" + std::string(id);
}

}  // namespace service::log_ingest
