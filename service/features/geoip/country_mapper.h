#pragma once
#include "node/proto/edge_control.pb.h"
#include "service/features/geoip/xdb_database.h"

namespace service::geoip {
inline flexedge::node::v2::CountryDatabase country_snapshot(const xdb_database& database) {
    flexedge::node::v2::CountryDatabase result;
    database.visit_countries([&](int version, std::string first, std::string last, const std::string& country) {
        auto* ranges = version == 4 ? result.mutable_ipv4() : result.mutable_ipv6();
        if (!ranges->empty()) {
            auto* previous = ranges->Mutable(ranges->size() - 1);
            auto after = previous->last();
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
            if (!overflow && previous->country() == country && after == first) {
                previous->set_last(std::move(last));
                return;
            }
        }
        auto* range = ranges->Add();
        range->set_first(std::move(first));
        range->set_last(std::move(last));
        range->set_country(country);
    });
    return result;
}
}  // namespace service::geoip
