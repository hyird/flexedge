#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace service::geoip {

struct IpLocation final {
    std::string country;
    std::string display;
    std::string asn;
    std::string asName;
};

namespace detail {

[[nodiscard]] inline bool isUnknownLocationPart(std::string_view value) {
    return value.empty() || value == "0" || value == "未知" || value == "未知地区";
}

[[nodiscard]] inline bool isAsn(std::string_view value) {
    if (value.size() < 3 || value[0] != 'A' || value[1] != 'S') {
        return false;
    }
    return std::all_of(value.begin() + 2, value.end(),
                       [](unsigned char character) { return std::isdigit(character) != 0; });
}

[[nodiscard]] inline std::optional<IpLocation> parseLocation(std::string_view value) {
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
        "亚洲", "欧洲",   "非洲",   "北美洲",        "南美洲",        "大洋洲",  "南极洲",
        "Asia", "Europe", "Africa", "North America", "South America", "Oceania", "Antarctica"};
    std::size_t countryIndex{};
    if (std::find(continents.begin(), continents.end(), parts.front()) != continents.end()) {
        countryIndex = 1;
    }
    const bool currentFiveField =
        countryIndex == 0 && parts.size() == 5 &&
        (isUnknownLocationPart(parts[3]) || isAsn(parts[3]));
    const bool hasAsMetadata = currentFiveField &&
                               (isAsn(parts[3]) || !isUnknownLocationPart(parts[4]));
    const bool hasCountry = countryIndex < parts.size() &&
                            !isUnknownLocationPart(parts[countryIndex]);
    if (!hasCountry && !hasAsMetadata) {
        return std::nullopt;
    }

    IpLocation result{.country = hasCountry ? std::string{parts[countryIndex]} : std::string{},
                      .display = hasCountry ? std::string{parts[countryIndex]} : "未知地区",
                      .asn = {},
                      .asName = {}};
    std::size_t details{};
    std::array<std::string_view, 2> selectedDetails{};
    // The current XDB contract is exactly five fields:
    // country|province|city|ASN|AS name. Only that unprefixed shape carries
    // ASN metadata. Older records may contain an ISP/network field and must
    // not be interpreted as an ASN.
    if (currentFiveField) {
        if (isAsn(parts[3])) {
            result.asn = std::string{parts[3]};
        }
        if (!isUnknownLocationPart(parts[4])) {
            result.asName = std::string{parts[4]};
        }
    }
    for (std::size_t index = countryIndex + 1;
         hasCountry &&
         index < parts.size() && (!currentFiveField || index < countryIndex + 3) && details < 2;
         ++index) {
        const auto part = parts[index];
        if (isUnknownLocationPart(part) || part == result.country ||
            std::find(selectedDetails.begin(), selectedDetails.end(), part) != selectedDetails.end()) {
            continue;
        }
        result.display += " · ";
        result.display += part;
        selectedDetails[details++] = part;
    }
    return result;
}

} // namespace detail
} // namespace service::geoip
