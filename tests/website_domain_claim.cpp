#include <array>
#include <iostream>
#include <stdexcept>
#include <variant>
#include <vector>

#include "service/domains/website/domain_claim.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("domain claim policy invariant failed");
    }
}

using claims = std::vector<service::website::domain_claim>;

const claims* get_claims(const std::variant<claims, service::website::domain_claim_error>& result) {
    return std::get_if<claims>(&result);
}

bool failed_with(const std::variant<claims, service::website::domain_claim_error>& result,
    service::website::domain_claim_error expected) {
    const auto* error = std::get_if<service::website::domain_claim_error>(&result);
    return error && *error == expected;
}
}  // namespace

int main() try {
    using service::website::domain_claim_error;
    using service::website::plan_domain_claims;
    const std::array<service::website::dns_zone_reference, 2> zones{{
        {"specific", "sub.example.com"},
        {"parent", "example.com"},
    }};
    std::array<service::website_config::website_domain_data, 1> domains{{
        {"domain-id", "WWW.SUB.EXAMPLE.COM", "managed"},
    }};
    auto result = plan_domain_claims(domains, zones);
    require(get_claims(result) && get_claims(result)->size() == 1);
    require(get_claims(result)->front().id_ == "domain-id");
    require(get_claims(result)->front().key_ == "www.sub.example.com");
    require(get_claims(result)->front().dns_mode_ == "managed");
    require(get_claims(result)->front().dns_zone_id_ == "specific");
    const std::array<service::website::dns_zone_reference, 2> reversed_zones{{zones[1], zones[0]}};
    result = plan_domain_claims(domains, reversed_zones);
    require(get_claims(result) && get_claims(result)->front().dns_zone_id_ == "specific");
    domains[0].hostname_ = "*.www.example.com";
    result = plan_domain_claims(domains, zones);
    require(get_claims(result) && get_claims(result)->front().dns_zone_id_ == "parent");
    for (const auto* host : {"example.com", "*.example.com", "SUB.EXAMPLE.COM"}) {
        domains[0].hostname_ = host;
        result = plan_domain_claims(domains, zones);
        require(failed_with(result, domain_claim_error::managed_zone_apex_unsupported));
    }
    domains[0].hostname_ = "badexample.com";
    result = plan_domain_claims(domains, zones);
    require(failed_with(result, domain_claim_error::managed_zone_not_found));
    domains[0].hostname_ = "www.example.com";
    result = plan_domain_claims(domains, {});
    require(failed_with(result, domain_claim_error::managed_zone_not_found));
    domains[0].dns_mode_ = "external";
    result = plan_domain_claims(domains, {});
    require(get_claims(result) && !get_claims(result)->front().dns_zone_id_);
    result = plan_domain_claims({}, zones);
    require(get_claims(result) && get_claims(result)->empty());
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
