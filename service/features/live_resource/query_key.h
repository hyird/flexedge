#pragma once

#include <concepts>
#include <optional>
#include <string>
#include <string_view>

namespace service::live_resource {

enum class Resource {
    nodes, clusters, websites, dnsZones, providers, certificates, tasks, overview, accessHistory
};

namespace detail {
inline void appendQueryPart(std::string& key, std::string_view value) {
    key += 's';
    key += std::to_string(value.size());
    key += ':';
    key += value;
}
template <std::integral T> void appendQueryPart(std::string& key, T value) {
    key += 'i';
    key += std::to_string(value);
    key += ';';
}
template <typename T> void appendQueryPart(std::string& key, const std::optional<T>& value) {
    key += value ? '+' : '-';
    if (value)
        appendQueryPart(key, *value);
}
} // namespace detail

// Controllers supply validated, normalized values in a fixed order. Lengths
// and type tags prevent delimiter collisions and distinguish absent filters.
template <typename... Args>
std::string queryKey(std::string_view projection, const Args&... args) {
    std::string key;
    detail::appendQueryPart(key, projection);
    (detail::appendQueryPart(key, args), ...);
    return key;
}

struct SnapshotKey final {
    std::string tenant;
    Resource resource;
    std::string id;
    std::string query;
    friend bool operator==(const SnapshotKey&, const SnapshotKey&) = default;
};

struct SnapshotKeyHash final {
    std::size_t operator()(const SnapshotKey& key) const noexcept {
        return std::hash<std::string>{}(key.tenant) ^
               (std::hash<std::string>{}(key.id) << 1U) ^
               (std::hash<std::string>{}(key.query) << 2U) ^
               (static_cast<std::size_t>(key.resource) << 3U);
    }
};

} // namespace service::live_resource
