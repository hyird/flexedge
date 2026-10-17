#include "node/data/origin_request_codec.h"

#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition)) {                                              \
            throw std::runtime_error("requirement failed: " #condition); \
        }                                                                \
    } while (false)

namespace {
using namespace flexedge::node;

v2::Website website() {
    v2::Website value;
    value.set_id("encoding-website");
    value.set_default_origin_group("default");
    auto* origin = value.add_origins();
    origin->set_id("origin");
    origin->set_enabled(true);
    origin->set_role("primary");
    origin->set_weight(1);
    return value;
}

v2::RouteRule& proxy_rule(v2::Website& value) {
    auto* rule = value.add_route_rules();
    rule->set_id("encoding-policy");
    rule->set_enabled(true);
    rule->set_action("proxy");
    rule->set_match_type("prefix");
    rule->set_path("/");
    rule->set_origin_group("default");
    return *rule;
}

void mutation(v2::RouteRule& rule, std::string_view name, std::string_view value) {
    auto* header = rule.add_request_headers();
    header->set_name(name);
    header->set_value(value);
}

std::vector<std::string> values(std::string_view wire, std::string_view name) {
    std::vector<std::string> result;
    auto begin = wire.find("\r\n") + 2;
    while (begin < wire.size()) {
        const auto end = wire.find("\r\n", begin);
        if (end == begin || end == std::string_view::npos) {
            break;
        }
        const auto line = wire.substr(begin, end - begin);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            flexedge::route_policy::ascii_equal(line.substr(0, colon), name)) {
            result.emplace_back(flexedge::route_policy::trim_ows(line.substr(colon + 1)));
        }
        begin = end + 2;
    }
    return result;
}

void verify_overrides_and_filtering() {
    auto site = website();
    site.set_pass_client_ip(true);
    auto& rule = proxy_rule(site);
    mutation(rule, "X-Value", "route");
    mutation(rule, "Accept-Encoding", "br");
    mutation(rule, "X-Forwarded-For", "203.0.113.5");
    const route_program program(site);
    const auto route = evaluate_route_rules(program, site, "GET", "/a");
    REQUIRE(route);
    const std::array<ruvia::http_header_view, 8> fields{{
        {"Host", "ignored.invalid"},
        {"X-Value", "first"},
        {"x-value", "second"},
        {"Accept-Encoding", "gzip"},
        {"X-Forwarded-For", "untrusted"},
        {"Expect", "100-continue"},
        {"Connection", "keep-alive"},
        {"Upgrade", "websocket"},
    }};
    const auto prepared = prepare_origin_request(
        {.method_ = "GET", .target_ = "/a", .headers_ = fields, .body_ = {}, .has_body_ = false},
        site, "WWW.Example.COM.:443", "198.51.100.10", true, false, false, true, &*route);
    REQUIRE(prepared);
    REQUIRE(values(prepared->bytes_, "Host") == std::vector<std::string>{"www.example.com"});
    REQUIRE(values(prepared->bytes_, "X-Value") == std::vector<std::string>{"route"});
    REQUIRE(values(prepared->bytes_, "Accept-Encoding") == std::vector<std::string>{"br"});
    REQUIRE(values(prepared->bytes_, "X-Forwarded-For") == std::vector<std::string>{"203.0.113.5"});
    REQUIRE(values(prepared->bytes_, "X-Forwarded-Proto") == std::vector<std::string>{"https"});
    REQUIRE(values(prepared->bytes_, "Expect").empty());
    REQUIRE(values(prepared->bytes_, "Connection").empty());
    REQUIRE(values(prepared->bytes_, "Upgrade").empty());
}

void verify_field_capacity_after_replacement() {
    auto site = website();
    std::vector<std::string> names;
    std::vector<ruvia::http_header_view> fields;
    names.reserve(ruvia::max_http_header_fields);
    fields.reserve(ruvia::max_http_header_fields);
    for (std::size_t index = 0; index != ruvia::max_http_header_fields - 1; ++index) {
        names.push_back("X-Field-" + std::to_string(index));
        fields.emplace_back(names.back(), "value");
    }
    origin_request_view incoming{.method_ = "GET", .target_ = "/", .headers_ = fields, .body_ = {}, .has_body_ = false};
    auto prepare = [&] {
        incoming.headers_ = fields;
        return prepare_origin_request(incoming, site, "www.example.com", "198.51.100.1",
            false, false, false);
    };
    REQUIRE(prepare());  // The generated Host is the last permitted field.
    names.emplace_back("X-One-Too-Many");
    fields.emplace_back(names.back(), "value");
    REQUIRE(!prepare());

    auto& rule = proxy_rule(site);
    mutation(rule, "X-Collapsed", "route");
    const route_program program(site);
    const auto route = evaluate_route_rules(program, site, "GET", "/");
    REQUIRE(route);
    fields.assign(ruvia::max_http_header_fields, {"x-collapsed", "incoming"});
    incoming.headers_ = fields;
    const auto replaced = prepare_origin_request(incoming, site, "www.example.com",
        "198.51.100.1", false, false, false, false, &*route);
    REQUIRE(replaced);
    REQUIRE(values(replaced->bytes_, "X-Collapsed") == std::vector<std::string>{"route"});
}

void verify_large_heads_and_owned_body() {
    const auto site = website();
    const std::string body = std::string("\0binary\r\npayload", 16) + std::string(4096, 'b');
    for (const auto field_size : {15000U, 20000U}) {
        const auto prepared = [&] {
            const std::string field(field_size, 'v');
            const std::array<ruvia::http_header_view, 2> fields{{{"X-Large", field}, {"Content-Type", "application/octet-stream"}}};
            const std::string transient_body = body;
            return prepare_origin_request({.method_ = "POST", .target_ = "/upload", .headers_ = fields, .body_ = transient_body, .has_body_ = true},
                site, "www.example.com", "198.51.100.1", false, false, false);
        }();
        REQUIRE(prepared);
        REQUIRE(values(prepared->bytes_, "X-Large") == std::vector<std::string>{std::string(field_size, 'v')});
        REQUIRE(values(prepared->bytes_, "Content-Length") == std::vector<std::string>{std::to_string(body.size())});
        REQUIRE(prepared->bytes_.ends_with(body));
        REQUIRE(prepared->bytes_.find("\r\n\r\n") + 4 + body.size() == prepared->bytes_.size());
    }
}
}  // namespace

int main() {
    try {
        verify_overrides_and_filtering();
        verify_field_capacity_after_replacement();
        verify_large_heads_and_owned_body();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
