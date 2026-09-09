#pragma once

#include <optional>
#include <string_view>
#include <system_error>

#include <asio/ip/address.hpp>

namespace service::common {

[[nodiscard]] inline std::optional<asio::ip::address> parseIpAddress(std::string_view value) {
    // Persisted endpoint addresses have no local interface scope (PostgreSQL inet).
    if (value.find('%') != std::string_view::npos) {
        return std::nullopt;
    }
    std::error_code error;
    const auto address = asio::ip::make_address(value, error);
    if (error) return std::nullopt;
    return address;
}

inline bool isIpv4Address(std::string_view value) {
    std::error_code error;
    (void)asio::ip::make_address_v4(value, error);
    return !error;
}

} // namespace service::common
