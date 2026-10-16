#pragma once

#include <ruvia/web/db/db_query.h>
#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/model_object.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/utils/utf8.h"

namespace service::sync_runtime {

inline constexpr std::string_view event_outcome_completed{"completed"};
inline constexpr std::string_view event_outcome_failed{"failed"};
// The attempt advanced its task, which now waits on another task.
inline constexpr std::string_view event_outcome_progressed{"progressed"};

// Bounds of one attempt detail document stored in sys_sync_event.detail and
// returned verbatim by the task history stream.
inline constexpr std::size_t detail_max_bytes{16384};
inline constexpr std::size_t detail_list_bytes{12288};
inline constexpr std::size_t detail_list_items{50};
inline constexpr std::size_t detail_text_bytes{256};
inline constexpr std::string_view detail_none_json{R"({"kind":"none","truncated":false})"};
// Replaces a detail that could not be produced; the attempt result is still recorded.
inline constexpr std::string_view detail_dropped_json{R"({"kind":"none","truncated":true})"};

// Copies at most `limit` bytes of `value`: invalid UTF-8 is dropped, control
// characters become spaces (PostgreSQL rejects NUL in text and jsonb) and the
// cut never splits a character. Reports whether anything was cut.
[[nodiscard]] inline std::string bounded_detail_text(std::string_view value, bool& cut,
    std::size_t limit = detail_text_bytes) {
    std::string result;
    result.reserve(value.size() < limit ? value.size() : limit);
    for (std::size_t index = 0; index < value.size();) {
        const auto length = service::utils::utf8_sequence_length(value, index);
        if (length == 0) {
            ++index;
            continue;
        }
        if (result.size() + length > limit) {
            cut = true;
            break;
        }
        const auto lead = static_cast<unsigned char>(value[index]);
        if (length == 1 && (lead < 0x20 || lead == 0x7F)) {
            result.push_back(' ');
        } else {
            result.append(value.substr(index, length));
        }
        index += length;
    }
    return result;
}

// Accumulates truncation while one detail document is mapped and shares the
// list byte budget across all of its arrays.
class detail_bounds final {
public:
    [[nodiscard]] std::string text(std::string_view value, std::size_t limit = detail_text_bytes) {
        return bounded_detail_text(value, truncated_, limit);
    }

    // Appends `item` while the list holds fewer than detail_list_items and its
    // serialized size fits the shared byte budget; otherwise drops it and
    // marks the detail truncated.
    template <typename list_type, typename item_type>
    bool append(list_type& list, item_type&& item, std::pmr::memory_resource* resource) {
        if (list.size() >= detail_list_items) {
            truncated_ = true;
            return false;
        }
        const auto bytes = ruvia::to_json(item, {.resource_ = resource}).size() + 1;
        if (bytes > remaining_bytes_) {
            truncated_ = true;
            return false;
        }
        remaining_bytes_ -= bytes;
        list.push_back(std::forward<item_type>(item));
        return true;
    }

    void mark_truncated() noexcept {
        truncated_ = true;
    }

    [[nodiscard]] bool truncated() const noexcept {
        return truncated_;
    }

private:
    std::size_t remaining_bytes_{detail_list_bytes};
    bool truncated_{false};
};

// Returns `detail` when PostgreSQL will store it as a bounded JSON object with
// a kind, otherwise the dropped marker. Binding only validated text keeps the
// result transaction from failing because of its detail: a failed statement
// retires the whole Ruvia transaction, so it cannot be isolated by a savepoint.
[[nodiscard]] inline std::string_view accepted_detail(std::string_view detail) noexcept {
    if (detail.size() > detail_max_bytes || detail.find("\\u0000") != std::string_view::npos) {
        return detail_dropped_json;
    }
    for (std::size_t index = 0; index < detail.size();) {
        const auto length = service::utils::utf8_sequence_length(detail, index);
        if (length == 0) {
            return detail_dropped_json;
        }
        index += length;
    }
    const auto object = ruvia::json_object::parse(detail);
    if (!object) {
        return detail_dropped_json;
    }
    const auto kind = object->get<ruvia::string>("kind");
    const auto truncated = object->get<ruvia::bool_value>("truncated");
    if (!kind || kind->view().empty() || !truncated) {
        return detail_dropped_json;
    }
    return detail;
}

// Serializes a detail produced by `map_detail(resource)`; any failure while
// mapping or serializing yields the dropped marker instead of an exception.
template <typename map_type>
[[nodiscard]] std::string serialize_detail(std::pmr::memory_resource* resource, map_type&& map_detail) noexcept {
    try {
        const auto json = ruvia::to_json(map_detail(resource), {.resource_ = resource});
        return std::string(accepted_detail(std::string_view(json)));
    } catch (...) {
        return std::string(detail_dropped_json);
    }
}

// Milliseconds since `started` on the monotonic clock, or nothing when the
// attempt start is unknown.
[[nodiscard]] inline std::optional<std::int64_t>
elapsed_milliseconds(std::chrono::steady_clock::time_point started) noexcept {
    if (started == std::chrono::steady_clock::time_point{}) {
        return std::nullopt;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (elapsed < std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

struct qualified_column final {
    std::string_view name_;
    std::string_view table_;
};

// Columns identifying one attempt sequence: tenant, task, resource and version.
struct attempt_sequence_columns final {
    qualified_column tenant_id_;
    qualified_column task_id_;
    qualified_column resource_id_;
    qualified_column version_;
};

inline constexpr std::string_view attempt_clock_alias{"attempt_clock"};

// Joins a single clock_timestamp() row so emitted_at and started_at derive from
// the same instant; returns that instant.
inline ruvia::db_expression join_attempt_clock(ruvia::db_query& source) {
    ruvia::db_query clock(source.resource());
    clock.select(clock.alias(clock.call("clock_timestamp"), "at"));
    source.join(ruvia::db_join_type::cross, clock, {}, attempt_clock_alias);
    return source.column("at", attempt_clock_alias);
}

// started_at = emitted_at - elapsed: the duration comes from the worker's
// monotonic clock and the start is placed on PostgreSQL's clock, so
// started_at + duration_ms = emitted_at. Unknown starts stay NULL.
inline ruvia::db_expression attempt_started_at(ruvia::db_query& source, ruvia::db_expression emitted_at,
    std::optional<std::int64_t> elapsed) {
    if (!elapsed) {
        return source.cast(source.null_value(), ruvia::db_data_type::timestamp_tz);
    }
    const auto millisecond = source.cast(source.value(std::string_view{"1 millisecond"}),
        ruvia::db_data_type::interval);
    const auto duration =
        source.binary(source.cast(source.value(static_cast<double>(*elapsed)), ruvia::db_data_type::double_value),
            ruvia::db_binary_operator::multiply, millisecond);
    return source.binary(emitted_at, ruvia::db_binary_operator::subtract, duration);
}

// Newest retained attempt number of the sequence plus one. The result
// transaction already holds the row lock of the task marker or node, so
// attempts of one sequence are numbered serially.
inline ruvia::db_expression next_attempt_number(ruvia::db_query& source,
    const attempt_sequence_columns& sequence) {
    ruvia::db_query previous(source.resource());
    const auto matches = [&](std::string_view column, const qualified_column& outer) {
        return previous.binary(previous.column(column, "previous_event"), ruvia::db_binary_operator::equal,
            previous.column(outer.name_, outer.table_));
    };
    previous.select(previous.column("attempt", "previous_event"))
        .from("sys_sync_event", "previous_event")
        .where(matches("tenant_id", sequence.tenant_id_))
        .and_where(matches("task_id", sequence.task_id_))
        .and_where(matches("resource_id", sequence.resource_id_))
        .and_where(matches("version", sequence.version_))
        .order_by(previous.column("id", "previous_event"), ruvia::db_order_direction::desc)
        .limit(1);
    return source.binary(source.coalesce({source.subquery(previous), source.value(std::int64_t{0})}),
        ruvia::db_binary_operator::add, source.value(std::int64_t{1}));
}

// Columns written by every attempt record, in the order the source selects
// them.
inline constexpr std::array attempt_event_columns{std::string_view{"tenant_id"}, std::string_view{"task_id"},
    std::string_view{"resource_type"}, std::string_view{"resource_id"}, std::string_view{"operation"},
    std::string_view{"version"}, std::string_view{"outcome"}, std::string_view{"error"},
    std::string_view{"attempt"}, std::string_view{"started_at"}, std::string_view{"duration_ms"},
    std::string_view{"detail"}, std::string_view{"emitted_at"}};

}  // namespace service::sync_runtime
