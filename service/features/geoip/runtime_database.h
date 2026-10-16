#pragma once

#include <optional>
#include <stdexcept>

#include "service/config/geoip.h"
#include "service/features/geoip/xdb_database.h"

namespace service::geoip {

namespace detail {

inline std::optional<xdb_database>& xdb_database_storage() {
    static std::optional<xdb_database> database;
    return database;
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
}

[[nodiscard]] inline const xdb_database& get_xdb_database() {
    const auto& storage = detail::xdb_database_storage();
    if (!storage) {
        throw std::logic_error("xdb database is not configured");
    }
    return *storage;
}

}  // namespace service::geoip
