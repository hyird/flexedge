#pragma once

#include <ruvia/web/redis/redis.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace service::log_ingest::notifications {

inline constexpr std::string_view stream_key{"flexedge:log-notifications:v2"};
inline constexpr std::int64_t max_entries{200000};

enum class log_resource_type {
    access,
    node,
};

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

// The log notification stream as the Worker notification reader follows it.
struct stream final {
    static constexpr std::string_view key{stream_key};
    using value_type = notification;
    [[nodiscard]] static std::optional<notification> parse(const ruvia::redis_value& entry) {
        return parse_entry(entry);
    }
};

}  // namespace service::log_ingest::notifications
