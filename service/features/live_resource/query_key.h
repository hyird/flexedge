#pragma once

#include <concepts>
#include <optional>
#include <string>
#include <string_view>

namespace service::live_resource {

enum class resource {
    nodes,
    clusters,
    websites,
    dns_zones,
    providers,
    certificates,
    operations,
    overview,
    access_history,
    overview_traffic
};

namespace detail {
inline void append_query_part(std::string& key, std::string_view value) {
    key += 's';
    key += std::to_string(value.size());
    key += ':';
    key += value;
}
template <std::integral t_type>
void append_query_part(std::string& key, t_type value) {
    key += 'i';
    key += std::to_string(value);
    key += ';';
}
template <typename t_type>
void append_query_part(std::string& key, const std::optional<t_type>& value) {
    key += value ? '+' : '-';
    if (value) {
        append_query_part(key, *value);
    }
}
}  // namespace detail

// Controllers supply validated, normalized values in a fixed order. Lengths
// and type tags prevent delimiter collisions and distinguish absent filters.
template <typename... args_type>
std::string query_key(std::string_view projection, const args_type&... args) {
    std::string key;
    detail::append_query_part(key, projection);
    (detail::append_query_part(key, args), ...);
    return key;
}

struct snapshot_key final {
    std::string tenant_;
    resource resource_;
    std::string id_;
    std::string query_;
    friend bool operator==(const snapshot_key&, const snapshot_key&) = default;
};

struct snapshot_key_hash final {
    std::size_t operator()(const snapshot_key& key) const noexcept {
        return std::hash<std::string>{}(key.tenant_) ^
               (std::hash<std::string>{}(key.id_) << 1U) ^
               (std::hash<std::string>{}(key.query_) << 2U) ^
               (static_cast<std::size_t>(key.resource_) << 3U);
    }
};

}  // namespace service::live_resource
