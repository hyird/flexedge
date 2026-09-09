#pragma once

#include <cstdlib>
#include "service/features/geoip/xdb_database.h"

namespace service::geoip {
namespace detail {
[[nodiscard]] inline std::optional<std::string> environmentValue(const char* name) {
#ifdef _WIN32
    char* raw{};
    std::size_t length{};
    if (_dupenv_s(&raw, &length, name) != 0 || !raw) {
        return std::nullopt;
    }
    std::string result{raw, length > 0 ? length - 1 : 0};
    std::free(raw);
    return result;
#else
    const auto* raw = std::getenv(name);
    if (!raw || !*raw) {
        return std::nullopt;
    }
    return std::string{raw};
#endif
}

} // namespace detail

[[nodiscard]] inline const XdbDatabase& xdbDatabase() {
    static const XdbDatabase database(
        detail::environmentValue("FLEXEDGE_XDB_V4_PATH").value_or(""),
        detail::environmentValue("FLEXEDGE_XDB_V6_PATH").value_or(""));
    return database;
}

} // namespace service::geoip
