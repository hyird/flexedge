#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace service::geoip {

struct ip_location final {
    std::string country_;
    std::string display_;
    std::string asn_;
    std::string as_name_;
};

namespace detail {

[[nodiscard]] inline bool is_unknown_location_part(std::string_view value) {
    return value.empty() || value == "0" || value == "未知" || value == "未知地区";
}

[[nodiscard]] inline bool is_asn(std::string_view value) {
    if (value.size() < 3 || value[0] != 'A' || value[1] != 'S') {
        return false;
    }
    return std::all_of(value.begin() + 2, value.end(),
        [](unsigned char character) { return std::isdigit(character) != 0; });
}

[[nodiscard]] inline std::optional<ip_location> parse_location(std::string_view value) {
    std::vector<std::string_view> parts;
    std::size_t start{};
    while (start <= value.size()) {
        const auto end = value.find('|', start);
        parts.push_back(value.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                          : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (parts.empty()) {
        return std::nullopt;
    }

    constexpr std::array<std::string_view, 14> continents{
        "亚洲", "欧洲", "非洲", "北美洲", "南美洲", "大洋洲", "南极洲",
        "Asia", "Europe", "Africa", "North America", "South America", "Oceania", "Antarctica"};
    std::size_t country_index{};
    if (std::find(continents.begin(), continents.end(), parts.front()) != continents.end()) {
        country_index = 1;
    }
    const bool current_five_field =
        country_index == 0 && parts.size() == 5 &&
        (is_unknown_location_part(parts[3]) || is_asn(parts[3]));
    const bool has_as_metadata = current_five_field &&
                                 (is_asn(parts[3]) || !is_unknown_location_part(parts[4]));
    const bool has_country = country_index < parts.size() &&
                             !is_unknown_location_part(parts[country_index]);
    if (!has_country && !has_as_metadata) {
        return std::nullopt;
    }

    ip_location result{.country_ = has_country ? std::string{parts[country_index]} : std::string{},
        .display_ = has_country ? std::string{parts[country_index]} : "未知地区",
        .asn_ = {},
        .as_name_ = {}};
    std::size_t details{};
    std::array<std::string_view, 2> selected_details{};
    // The current XDB contract is exactly five fields:
    // country|province|city|ASN|AS name. Only that unprefixed shape carries
    // ASN metadata. Older records may contain an ISP/network field and must
    // not be interpreted as an ASN.
    if (current_five_field) {
        if (is_asn(parts[3])) {
            result.asn_ = std::string{parts[3]};
        }
        if (!is_unknown_location_part(parts[4])) {
            result.as_name_ = std::string{parts[4]};
        }
    }
    for (std::size_t index = country_index + 1;
        has_country &&
        index < parts.size() && (!current_five_field || index < country_index + 3) && details < 2;
        ++index) {
        const auto part = parts[index];
        if (is_unknown_location_part(part) || part == result.country_ ||
            std::find(selected_details.begin(), selected_details.end(), part) != selected_details.end()) {
            continue;
        }
        result.display_ += " · ";
        result.display_ += part;
        selected_details[details++] = part;
    }
    return result;
}

}  // namespace detail
}  // namespace service::geoip
