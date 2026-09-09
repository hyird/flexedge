#include <iostream>
#include <stdexcept>
#include "service/features/geoip/location.h"

#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

int main() {
    try {
        using service::geoip::detail::parseLocation;
        REQUIRE(!parseLocation(""));
        REQUIRE(!parseLocation("|||||Cloudflare|AS13335"));
        REQUIRE(!parseLocation("Asia|0|Province"));
        REQUIRE(!parseLocation("Europe"));
        const auto prefixed = parseLocation("Asia|Country|Province|City|Network|AS123");
        REQUIRE(prefixed && prefixed->country == "Country");
        REQUIRE(prefixed->display == "Country · Province · City");
        REQUIRE(prefixed->asn.empty() && prefixed->asName.empty());
        const auto current = parseLocation(
            "中国|广东省|广州市|AS141425|China Mobile Group Guangdong communications corporation");
        REQUIRE(current && current->display == "中国 · 广东省 · 广州市");
        REQUIRE(current->asn == "AS141425");
        REQUIRE(current->asName == "China Mobile Group Guangdong communications corporation");
        const auto missingAsn = parseLocation("中国|广东省|广州市||");
        REQUIRE(missingAsn && missingAsn->asn.empty() && missingAsn->asName.empty());
        const auto oldOperator = parseLocation("中国|广东省|广州市|中国移动");
        REQUIRE(oldOperator && oldOperator->asn.empty() && oldOperator->asName.empty());
        const auto oldFiveFieldOperator = parseLocation("中国|广东省|广州市|移动|中国移动");
        REQUIRE(oldFiveFieldOperator && oldFiveFieldOperator->asn.empty() &&
                oldFiveFieldOperator->asName.empty());
        const auto unknownCountry = parseLocation("0|0|0|AS13335|CLOUDFLARENET");
        REQUIRE(unknownCountry && unknownCountry->country.empty());
        REQUIRE(unknownCountry->display == "未知地区");
        REQUIRE(unknownCountry->asn == "AS13335" && unknownCountry->asName == "CLOUDFLARENET");
        const auto emptyCountry = parseLocation("|||AS13335|CLOUDFLARENET");
        REQUIRE(emptyCountry && emptyCountry->country.empty());
        REQUIRE(emptyCountry->display == "未知地区");
        REQUIRE(emptyCountry->asn == "AS13335" && emptyCountry->asName == "CLOUDFLARENET");
        REQUIRE(!parseLocation("0|0|0||0"));
        const auto repeated = parseLocation("Country|Country|0|Province|Province|City");
        REQUIRE(repeated && repeated->display == "Country · Province · City");
        const auto onlyCountry = parseLocation("Country|0||0");
        REQUIRE(onlyCountry && onlyCountry->display == "Country");
        const auto overlappingNames = parseLocation("Country|New York|York");
        REQUIRE(overlappingNames && overlappingNames->display == "Country · New York · York");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
