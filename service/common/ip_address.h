#pragma once

#include <string_view>
#include <system_error>

#include <asio/ip/address.hpp>

namespace service::common {

inline bool isIpAddress(std::string_view value) {
    std::error_code error;
    (void)asio::ip::make_address(value, error);
    return !error;
}

inline bool isIpv4Address(std::string_view value) {
    std::error_code error;
    (void)asio::ip::make_address_v4(value, error);
    return !error;
}

} // namespace service::common
