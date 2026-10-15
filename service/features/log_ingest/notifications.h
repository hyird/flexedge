#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/utils/redis_reply.h"

namespace service::log_ingest::notifications {

using service::utils::require_redis_success;

inline constexpr std::string_view stream_key{"flexedge:log-notifications:v2"};
inline constexpr std::int64_t max_entries{200000};
inline constexpr std::uint64_t read_batch_size{64};
inline constexpr auto read_block = std::chrono::seconds(15);
inline constexpr auto read_timeout = std::chrono::seconds(20);

enum class log_resource_type {
    access,
    node,
};

[[nodiscard]] inline constexpr std::string_view resource_type_name(log_resource_type resource_type) {
    switch (resource_type) {
        case log_resource_type::access:
            return "access";
        case log_resource_type::node:
            return "node";
    }
    throw std::invalid_argument("unsupported log notification resource type");
}

[[nodiscard]] inline std::optional<log_resource_type> parse_resource_type(std::string_view value) {
    if (value == "access") {
        return log_resource_type::access;
    }
    if (value == "node") {
        return log_resource_type::node;
    }
    return std::nullopt;
}

struct notification final {
    std::string id_;
    std::string tenant_id_;
    log_resource_type resource_type_{};
    std::string resource_id_;
};

struct read_batch final {
    std::vector<notification> notifications_;
    std::optional<std::string> cursor_;
};

inline ruvia::task<void> publish(ruvia::redis_handle redis, log_resource_type resource_type,
    std::string_view tenant_id, std::string_view resource_id) {
    if (tenant_id.empty() || resource_id.empty()) {
        co_return;
    }
    const auto maximum_entries = std::to_string(max_entries);
    const auto result = co_await redis.command(
        "XADD", stream_key, "MAXLEN", "~", maximum_entries, "*", "tenant_id", tenant_id,
        "resource_type", resource_type_name(resource_type), "resource_id", resource_id);
    require_redis_success(result, "could not publish log notification");
    if (result.kind() != ruvia::redis_value::kind_type::string || result.string().empty()) {
        throw std::runtime_error("unexpected log notification publish reply");
    }
}

inline std::string current_entry_id(const ruvia::redis_value& value) {
    require_redis_success(value, "could not read log notification cursor");
    if (value.null()) {
        return "0-0";
    }
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected log notification cursor reply");
    }
    const auto entries = value.array();
    if (entries.empty()) {
        return "0-0";
    }
    const auto entry = entries.front().array();
    if (entry.size() != 2 || entry.front().kind() != ruvia::redis_value::kind_type::string) {
        throw std::runtime_error("unexpected log notification cursor entry");
    }
    return std::string(entry.front().string());
}

inline ruvia::task<std::string> current_cursor(ruvia::redis_handle redis) {
    co_return current_entry_id(
        co_await redis.command("XREVRANGE", stream_key, "+", "-", "COUNT", "1"));
}

inline std::optional<std::string> field_value(std::span<const ruvia::redis_value> fields,
    std::string_view name) {
    if (fields.size() % 2 != 0) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < fields.size(); index += 2) {
        if (fields[index].kind() != ruvia::redis_value::kind_type::string ||
            fields[index + 1].kind() != ruvia::redis_value::kind_type::string) {
            return std::nullopt;
        }
        if (fields[index].string() == name) {
            return std::string(fields[index + 1].string());
        }
    }
    return std::nullopt;
}

inline std::optional<notification> parse_entry(const ruvia::redis_value& value) {
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        return std::nullopt;
    }
    const auto entry = value.array();
    if (entry.size() != 2 || entry[0].kind() != ruvia::redis_value::kind_type::string ||
        entry[1].kind() != ruvia::redis_value::kind_type::array) {
        return std::nullopt;
    }
    const auto fields = entry[1].array();
    const auto tenant_id = field_value(fields, "tenant_id");
    const auto resource_type = field_value(fields, "resource_type");
    const auto resource_id = field_value(fields, "resource_id");
    if (!tenant_id || !resource_type || !resource_id || tenant_id->empty() || resource_id->empty()) {
        return std::nullopt;
    }
    const auto parsed_resource_type = parse_resource_type(*resource_type);
    if (!parsed_resource_type) {
        return std::nullopt;
    }
    return notification{
        .id_ = std::string(entry[0].string()),
        .tenant_id_ = std::move(*tenant_id),
        .resource_type_ = *parsed_resource_type,
        .resource_id_ = std::move(*resource_id),
    };
}

inline std::optional<std::string> entry_id(const ruvia::redis_value& value) {
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        return std::nullopt;
    }
    const auto entry = value.array();
    if (entry.size() != 2 || entry[0].kind() != ruvia::redis_value::kind_type::string) {
        return std::nullopt;
    }
    return std::string(entry[0].string());
}

inline read_batch parse_read_result(const ruvia::redis_value& value) {
    require_redis_success(value, "could not read log notifications");
    read_batch output;
    if (value.null()) {
        return output;
    }
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected log notification read reply");
    }
    const auto streams = value.array();
    if (streams.empty()) {
        return output;
    }
    if (streams.size() != 1 || streams.front().kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected log notification read reply");
    }
    const auto stream = streams.front().array();
    if (stream.size() != 2 || stream[0].kind() != ruvia::redis_value::kind_type::string ||
        stream[0].string() != stream_key || stream[1].kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected log notification stream reply");
    }
    const auto entries = stream[1].array();
    output.notifications_.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto id = entry_id(entry);
        if (!id) {
            throw std::runtime_error("unexpected log notification entry");
        }
        output.cursor_ = *id;
        if (const auto parsed = parse_entry(entry)) {
            output.notifications_.push_back(*parsed);
        }
    }
    return output;
}

inline ruvia::task<read_batch> read(ruvia::redis_handle redis, std::string_view cursor) {
    const auto block_ms =
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(read_block).count());
    const auto count = std::to_string(read_batch_size);
    const auto redis_with_timeout = redis.with_options({.timeout_ = read_timeout});
    co_return parse_read_result(co_await redis_with_timeout.command(
        "XREAD", "BLOCK", block_ms, "COUNT", count, "STREAMS", stream_key, cursor));
}

}  // namespace service::log_ingest::notifications
