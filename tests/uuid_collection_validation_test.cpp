#include <iostream>
#include <string>
#include <ruvia/web/ModelJson.h>
#include "service/domains/website/website.schema.h"
#include "service/domains/dns_zone/dns_zone.schema.h"

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
             position = json.find("SECOND")) json.replace(position, 6, second);
        const auto config = ruvia::fromJson<service::website_config::WebsiteConfigInput>(json);
        if (!config || !config->get<"domains">() || !config->get<"origins">() ||
            !config->get<"routeRules">() ||
            service::website::validDomainSet(*config->get<"domains">()) != distinct ||
            service::website::validOriginSet(*config->get<"origins">()) != distinct ||
            service::website::validRouteRuleSet(*config->get<"routeRules">()) != distinct) {
            std::cerr << "Website UUID set validation mismatch\n";
            return 1;
        }
    }

    const auto website = ruvia::fromJson<service::website_config::WebsiteConfigInput>(R"({
        "certificate_ids":["aaaaaaaa-0000-0000-0000-000000000001",
                           "AAAAAAAA-0000-0000-0000-000000000001"]})");
    const auto dns = ruvia::fromJson<service::dns_sync::ZoneConfigInput>(R"({
        "records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001"},
                   {"id":"AAAAAAAA-0000-0000-0000-000000000001"}]})");
    if (!website || !website->get<"certificateIds">() || !dns || !dns->get<"records">()) {
        std::cerr << "Invalid UUID collection fixtures\n";
        return 1;
    }
    if (service::website::hasUniqueCertificateIds(*website->get<"certificateIds">()) ||
        service::dns_zone::hasUniqueRecordIds(*dns->get<"records">())) {
        std::cerr << "Equivalent UUID collection entries admitted\n";
        return 1;
    }
    const auto distinctWebsite = ruvia::fromJson<service::website_config::WebsiteConfigInput>(R"({
        "certificate_ids":["aaaaaaaa-0000-0000-0000-000000000001",
                           "AAAAAAAA-0000-0000-0000-000000000002"]})");
    const auto distinctDns = ruvia::fromJson<service::dns_sync::ZoneConfigInput>(R"({
        "records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001"},
                   {"id":"AAAAAAAA-0000-0000-0000-000000000002"}]})");
    if (!distinctWebsite || !distinctWebsite->get<"certificateIds">() ||
        !distinctDns || !distinctDns->get<"records">() ||
        !service::website::hasUniqueCertificateIds(*distinctWebsite->get<"certificateIds">()) ||
        !service::dns_zone::hasUniqueRecordIds(*distinctDns->get<"records">())) {
        std::cerr << "Distinct UUID collection entries rejected\n";
        return 1;
    }

}
