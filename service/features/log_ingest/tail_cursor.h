#pragma once
#include <charconv>
#include <ruvia/web/db/db_query.h>
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

inline void apply_tail_cursor_window(ruvia::db_query& entries, std::string_view alias,
    std::int64_t limit, const std::optional<tail_cursor>& after) {
    const auto created_at_col = std::string(alias) + ".created_at";
    const auto id_col = std::string(alias) + ".id";
    if (after) {
        entries.and_where(entries.sql({"(" + created_at_col + ", " + id_col +
                                          ") > (TIMESTAMPTZ 'epoch' + ",
                                          "::bigint * INTERVAL '1 microsecond', ", "::uuid)"},
            {entries.value(after->ingested_unix_micros_), entries.value(after->id_)}));
    }
    entries.order_by(entries.sql(created_at_col), ruvia::db_order_direction::desc)
        .add_order_by(entries.sql(id_col), ruvia::db_order_direction::desc)
        .limit(static_cast<std::uint64_t>(limit));
}

}  // namespace service::log_ingest
