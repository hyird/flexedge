#pragma once
#include <string>
#include <utility>
#include <vector>

#include "service/features/geoip/xdb_database.h"

namespace service::geoip {
struct country_range final {
    std::string first_{}, last_{}, country_{};
};
struct country_snapshot_data final {
    std::vector<country_range> ipv4_{}, ipv6_{};
};

inline country_snapshot_data country_snapshot(const xdb_database& database) {
    country_snapshot_data result;
    database.visit_countries([&](int version, std::string first, std::string last, const std::string& country) {
        auto& ranges = version == 4 ? result.ipv4_ : result.ipv6_;
        if (!ranges.empty()) {
            auto& previous = ranges.back();
            auto after = previous.last_;
            bool overflow = true;
            for (std::size_t position = after.size(); position > 0; --position) {
                auto& byte = after[position - 1];
                const auto value = static_cast<unsigned char>(byte);
                byte = static_cast<char>(value + 1);
                if (value != 255) {
                    overflow = false;
                    break;
                }
            }
            if (!overflow && previous.country_ == country && after == first) {
                previous.last_ = std::move(last);
                return;
            }
        }
        ranges.push_back({std::move(first), std::move(last), country});
    });
    return result;
}
}  // namespace service::geoip
