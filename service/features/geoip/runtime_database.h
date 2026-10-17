#pragma once

#include <optional>
#include <stdexcept>

#include "service/config/geoip.h"
#include "service/features/geoip/country_mapper.h"
#include "service/features/geoip/xdb_database.h"

namespace service::geoip {

namespace detail {

inline std::optional<xdb_database>& xdb_database_storage() {
    static std::optional<xdb_database> database;
    return database;
}
inline std::optional<country_snapshot_data>& country_snapshot_storage() {
    static std::optional<country_snapshot_data> countries;
    return countries;
}

}  // namespace detail

// Loads the configured XDB files once at startup, before Workers start; the
// database is immutable afterwards.
inline void configure_xdb_database(const service::config::geoip_configuration& config) {
    auto& storage = detail::xdb_database_storage();
    if (storage) {
        throw std::logic_error("xdb database already configured");
    }
    storage.emplace(config.ipv4_path_, config.ipv6_path_);
    detail::country_snapshot_storage().emplace(country_snapshot(*storage));
}

[[nodiscard]] inline const xdb_database& get_xdb_database() {
    const auto& storage = detail::xdb_database_storage();
    if (!storage) {
        throw std::logic_error("xdb database is not configured");
    }
    return *storage;
}
[[nodiscard]] inline const country_snapshot_data& get_country_snapshot() {
    const auto& snapshot = detail::country_snapshot_storage();
    if (!snapshot) {
        throw std::logic_error("country snapshot is not configured");
    }
    return *snapshot;
}

}  // namespace service::geoip
