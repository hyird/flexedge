#include <iostream>
#include <stdexcept>

#include "service/features/geoip/location.h"

#define REQUIRE(condition)                                      \
    do {                                                        \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

int main() {
    try {
        using service::geoip::detail::parse_location;
        REQUIRE(!parse_location(""));
        REQUIRE(!parse_location("|||||Cloudflare|AS13335"));
        REQUIRE(!parse_location("Asia|0|Province"));
        REQUIRE(!parse_location("Europe"));
        const auto prefixed = parse_location("Asia|Country|Province|City|Network|AS123");
        REQUIRE(prefixed && prefixed->country_ == "Country");
        REQUIRE(prefixed->display_ == "Country · Province · City");
        REQUIRE(prefixed->asn_.empty() && prefixed->as_name_.empty());
        const auto current = parse_location(
            "中国|广东省|广州市|AS141425|China Mobile Group Guangdong communications corporation");
        REQUIRE(current && current->display_ == "中国 · 广东省 · 广州市");
        REQUIRE(current->asn_ == "AS141425");
        REQUIRE(current->as_name_ == "China Mobile Group Guangdong communications corporation");
        const auto missing_asn = parse_location("中国|广东省|广州市||");
        REQUIRE(missing_asn && missing_asn->asn_.empty() && missing_asn->as_name_.empty());
        const auto old_operator = parse_location("中国|广东省|广州市|中国移动");
        REQUIRE(old_operator && old_operator->asn_.empty() && old_operator->as_name_.empty());
        const auto old_five_field_operator = parse_location("中国|广东省|广州市|移动|中国移动");
        REQUIRE(old_five_field_operator && old_five_field_operator->asn_.empty() &&
                old_five_field_operator->as_name_.empty());
        const auto unknown_country = parse_location("0|0|0|AS13335|CLOUDFLARENET");
        REQUIRE(unknown_country && unknown_country->country_.empty());
        REQUIRE(unknown_country->display_ == "未知地区");
        REQUIRE(unknown_country->asn_ == "AS13335" && unknown_country->as_name_ == "CLOUDFLARENET");
        const auto empty_country = parse_location("|||AS13335|CLOUDFLARENET");
        REQUIRE(empty_country && empty_country->country_.empty());
        REQUIRE(empty_country->display_ == "未知地区");
        REQUIRE(empty_country->asn_ == "AS13335" && empty_country->as_name_ == "CLOUDFLARENET");
        REQUIRE(!parse_location("0|0|0||0"));
        const auto repeated = parse_location("Country|Country|0|Province|Province|City");
        REQUIRE(repeated && repeated->display_ == "Country · Province · City");
        const auto only_country = parse_location("Country|0||0");
        REQUIRE(only_country && only_country->display_ == "Country");
        const auto overlapping_names = parse_location("Country|New York|York");
        REQUIRE(overlapping_names && overlapping_names->display_ == "Country · New York · York");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
