#include <ruvia/web/model_json.h>

#include <iostream>
#include <string>

#include "service/domains/dns_zone/dns_zone_schema.h"
#include "service/features/website_config/validation.h"

int main() {
    for (const bool distinct : {false, true}) {
        std::string json = R"({
          "domains":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","hostname":"a.example"},
                     {"id":"SECOND","hostname":"b.example"}],
          "origins":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","group":"g","status":"enabled","role":"primary"},
                     {"id":"SECOND","group":"g","status":"enabled","role":"primary"}],
          "route_rules":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","origin_group":"g","request_headers":[],"response_headers":[]},
                         {"id":"SECOND","origin_group":"g","request_headers":[],"response_headers":[]}]
        })";
        const std::string second = distinct ? "AAAAAAAA-0000-0000-0000-000000000002"
                                            : "AAAAAAAA-0000-0000-0000-000000000001";
        for (auto position = json.find("SECOND"); position != std::string::npos;
            position = json.find("SECOND")) {
            json.replace(position, 6, second);
        }
        const auto config = ruvia::from_json<service::website_config::website_config_input>(json);
        if (!config || !config->get<"domains">() || !config->get<"origins">() ||
            !config->get<"route_rules">() ||
            service::website_config::valid_domain_set(*config->get<"domains">()) != distinct ||
            service::website_config::valid_origin_set(*config->get<"origins">()) != distinct ||
            service::website_config::valid_route_rule_set(*config->get<"route_rules">()) != distinct) {
            std::cerr << "Website UUID set validation mismatch\n";
            return 1;
        }
    }

    const auto website = ruvia::from_json<service::website_config::website_config_input>(R"({
        "certificate_ids":["aaaaaaaa-0000-0000-0000-000000000001",
                           "AAAAAAAA-0000-0000-0000-000000000001"]})");
    const auto dns = ruvia::from_json<service::dns_sync::zone_config_input>(R"({
        "records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001"},
                   {"id":"AAAAAAAA-0000-0000-0000-000000000001"}]})");
    if (!website || !website->get<"certificate_ids">() || !dns || !dns->get<"records">()) {
        std::cerr << "Invalid UUID collection fixtures\n";
        return 1;
    }
    if (service::website_config::has_unique_certificate_ids(*website->get<"certificate_ids">()) ||
        service::dns_sync::has_unique_record_ids(*dns->get<"records">())) {
        std::cerr << "Equivalent UUID collection entries admitted\n";
        return 1;
    }
    const auto distinct_website = ruvia::from_json<service::website_config::website_config_input>(R"({
        "certificate_ids":["aaaaaaaa-0000-0000-0000-000000000001",
                           "AAAAAAAA-0000-0000-0000-000000000002"]})");
    const auto distinct_dns = ruvia::from_json<service::dns_sync::zone_config_input>(R"({
        "records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001"},
                   {"id":"AAAAAAAA-0000-0000-0000-000000000002"}]})");
    if (!distinct_website || !distinct_website->get<"certificate_ids">() ||
        !distinct_dns || !distinct_dns->get<"records">() ||
        !service::website_config::has_unique_certificate_ids(*distinct_website->get<"certificate_ids">()) ||
        !service::dns_sync::has_unique_record_ids(*distinct_dns->get<"records">())) {
        std::cerr << "Distinct UUID collection entries rejected\n";
        return 1;
    }
}
