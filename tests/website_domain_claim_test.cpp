#include <array>
#include <iostream>
#include <stdexcept>
#include "service/domains/website/domain_claim.h"

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("domain claim policy invariant failed");
}
}

int main() try {
    using service::website::DomainClaimError;
    using service::website::planDomainClaims;
    const std::array<service::website::DnsZoneReference, 2> zones{{
        {"specific", "sub.example.com"}, {"parent", "example.com"},
    }};
    std::array<service::website_config::WebsiteDomainData, 1> domains{{
        {"domain-id", "WWW.SUB.EXAMPLE.COM", "managed"},
    }};
    auto result = planDomainClaims(domains, zones);
    require(result && result->size() == 1);
    require(result->front().id == "domain-id");
    require(result->front().key == "www.sub.example.com");
    require(result->front().dnsMode == "managed");
    require(result->front().dnsZoneId == "specific");
    const std::array<service::website::DnsZoneReference, 2> reversedZones{{zones[1], zones[0]}};
    result = planDomainClaims(domains, reversedZones);
    require(result && result->front().dnsZoneId == "specific");
    domains[0].hostname = "*.www.example.com";
    result = planDomainClaims(domains, zones);
    require(result && result->front().dnsZoneId == "parent");
    for (const auto* host : {"example.com", "*.example.com", "SUB.EXAMPLE.COM"}) {
        domains[0].hostname = host;
        result = planDomainClaims(domains, zones);
        require(!result && result.error() == DomainClaimError::managedZoneApexUnsupported);
    }
    domains[0].hostname = "badexample.com";
    result = planDomainClaims(domains, zones);
    require(!result && result.error() == DomainClaimError::managedZoneNotFound);
    domains[0].hostname = "www.example.com";
    result = planDomainClaims(domains, {});
    require(!result && result.error() == DomainClaimError::managedZoneNotFound);
    domains[0].dnsMode = "external";
    result = planDomainClaims(domains, {});
    require(result && !result->front().dnsZoneId);
    result = planDomainClaims({}, zones);
    require(result && result->empty());
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
