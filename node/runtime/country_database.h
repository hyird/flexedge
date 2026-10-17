#pragma once

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include <asio/ip/address.hpp>

#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

inline void validate_country_database(const v2::CountryDatabase& database) {
    const auto validate = [](const auto& ranges, std::size_t width) {
        std::string_view previous;
        for (const auto& range : ranges) {
            if (range.first().size() != width || range.last().size() != width ||
                range.first() > range.last() || (!previous.empty() && previous >= range.first()) ||
                range.country().empty() || range.country().size() > 128) {
                throw std::runtime_error("invalid country database range");
            }
            previous = range.last();
        }
    };
    validate(database.ipv4(), 4);
    validate(database.ipv6(), 16);
}

inline std::string_view country_for_address(const v2::CountryDatabase& database, std::string_view address) {
    std::error_code error;
    const auto parsed = asio::ip::make_address(address, error);
    if (error) {
        return "XX";
    }
    const auto find = [](const auto& ranges, const auto& bytes) -> std::string_view {
        const std::string_view key(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto found = std::upper_bound(ranges.begin(), ranges.end(), key,
            [](std::string_view value, const auto& range) { return value < range.first(); });
        if (found == ranges.begin()) {
            return "XX";
        }
        --found;
        return key <= found->last() ? std::string_view(found->country()) : std::string_view("XX");
    };
    return parsed.is_v4() ? find(database.ipv4(), parsed.to_v4().to_bytes())
                          : find(database.ipv6(), parsed.to_v6().to_bytes());
}

}  // namespace flexedge::node
