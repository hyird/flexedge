#include "service/domains/website/website_access_log_mapper.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

// Mirrors the database field calls the mapper makes on query rows.
class access_log_field final {
public:
    std::optional<std::string> text_;

    [[nodiscard]] std::optional<std::string_view> value() const {
        if (!text_) {
            return std::nullopt;
        }
        return std::string_view{*text_};
    }

    template <typename t_type>
    [[nodiscard]] std::optional<t_type> as() const {
        if (!text_) {
            return std::nullopt;
        }
        if constexpr (std::is_same_v<t_type, bool>) {
            return *text_ == "t";
        } else {
            t_type converted{};
            std::from_chars(text_->data(), text_->data() + text_->size(), converted);
            return converted;
        }
    }
};

using access_log_row = std::array<access_log_field, 22>;

access_log_row make_row(std::optional<std::string> client_ip) {
    access_log_row row{};
    row[0].text_ = "33333333-3333-4333-8333-333333333333";
    row[1].text_ = "2026-10-14T08:00:00.000000+00";
    row[2].text_ = "44444444-4444-4444-8444-444444444444";
    row[3].text_ = "edge-1";
    row[4].text_ = std::move(client_ip);
    row[5].text_ = "https";
    row[6].text_ = "GET";
    row[7].text_ = "example.com";
    row[8].text_ = "/";
    row[9].text_ = "200";
    row[10].text_ = "120";
    row[11].text_ = "512";
    row[12].text_ = "3";
    row[17].text_ = "f";
    return row;
}

std::string map_to_json(std::optional<std::string> client_ip) {
    service::website::website_access_log_dto item;
    service::website::fill_website_access_log(item, make_row(std::move(client_ip)));
    return std::string(ruvia::to_json(item));
}

bool contains(std::string_view json, std::string_view text) {
    return json.find(text) != std::string_view::npos;
}

void require_without_asn(std::string_view json, std::string_view label) {
    require(!contains(json, "\"client_ip_asn\""), std::string(label) + ": unexpected client_ip_asn");
    require(!contains(json, "\"client_ip_as_name\""), std::string(label) + ": unexpected client_ip_as_name");
}

void verify_with_xdb() {
    require(service::geoip::get_xdb_database().available(), "synthetic XDB did not load");

    const auto chinanet = map_to_json("1.2.3.4");
    require(contains(chinanet, "\"client_ip_asn\":\"AS4134\""), "IPv4 ASN missing: " + chinanet);
    require(contains(chinanet, "\"client_ip_as_name\":\"Chinanet\""), "IPv4 AS name missing: " + chinanet);
    require(contains(chinanet, "\"client_ip_location\":\"中国 · 广东省 · 广州市\""),
        "IPv4 location changed: " + chinanet);

    const auto without_asn = map_to_json("1.2.4.4");
    require(contains(without_asn, "\"client_ip_location\""), "location missing without ASN");
    require_without_asn(without_asn, "five-field record without ASN");

    const auto legacy = map_to_json("1.2.5.4");
    require(contains(legacy, "\"client_ip_location\""), "legacy location missing");
    require_without_asn(legacy, "legacy four-field record");

    const auto long_name = map_to_json("1.2.6.4");
    require(contains(long_name, "\"client_ip_asn\":\"AS4294967295\""), "maximum ASN missing: " + long_name);
    require(contains(long_name, "\"client_ip_as_name\":\"Very Long Autonomous System Name Very"),
        "long AS name missing: " + long_name);
    require(contains(long_name, "\"client_ip_location\":\"未知地区\""), "unknown country display changed");

    const auto ipv6 = map_to_json("2001:db8::1");
    require(contains(ipv6, "\"client_ip_asn\":\"AS15169\""), "IPv6 ASN missing: " + ipv6);
    require(contains(ipv6, "\"client_ip_as_name\":\"Google LLC\""), "IPv6 AS name missing: " + ipv6);

    const auto unknown = map_to_json("unknown");
    require(contains(unknown, "\"client_ip\":\"unknown\""), "unknown client IP not kept");
    require(!contains(unknown, "\"client_ip_location\""), "unknown client IP has a location");
    require_without_asn(unknown, "unknown client IP");

    require_without_asn(map_to_json("1.2.7.4"), "address outside every range");
    require_without_asn(map_to_json(std::nullopt), "absent client IP");
}

void verify_missing_xdb() {
    require(!service::geoip::get_xdb_database().available(), "absent XDB paths loaded a database");
    const auto json = map_to_json("1.2.3.4");
    require(contains(json, "\"client_ip\":\"1.2.3.4\""), "client IP missing without XDB");
    require(!contains(json, "\"client_ip_location\""), "location present without XDB");
    require_without_asn(json, "missing XDB");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        const auto path = [](const char* name) {
            const auto* value = std::getenv(name);
            return std::filesystem::path(value ? value : "");
        };
        service::geoip::configure_xdb_database({.ipv4_path_ = path("FLEXEDGE_XDB_V4_PATH"),
            .ipv6_path_ = path("FLEXEDGE_XDB_V6_PATH")});
        if (mode == "with-xdb") {
            verify_with_xdb();
        } else if (mode == "missing-xdb") {
            verify_missing_xdb();
        } else {
            std::cerr << "usage: website_access_log_mapper with-xdb|missing-xdb\n";
            return 2;
        }
        std::cout << "access-log mapper " << mode << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
