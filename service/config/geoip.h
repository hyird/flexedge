#pragma once

#include <ruvia/web/dotenv.h>

#include <filesystem>

namespace service::config {

// Optional ip2region XDB files; an empty path leaves that address family without locations.
struct geoip_configuration final {
    std::filesystem::path ipv4_path_;
    std::filesystem::path ipv6_path_;
};

inline geoip_configuration get_geoip_configuration(const ruvia::env& env) {
    return {
        .ipv4_path_ = std::filesystem::path(env.get("FLEXEDGE_XDB_V4_PATH").value_or("")),
        .ipv6_path_ = std::filesystem::path(env.get("FLEXEDGE_XDB_V6_PATH").value_or("")),
    };
}

}  // namespace service::config
