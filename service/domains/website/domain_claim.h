#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "service/common/domain_name.h"
#include "service/domains/website/domain_claim.types.h"
#include "service/features/website_config/model.h"

namespace service::website {

enum class DomainClaimError { managedZoneNotFound, managedZoneApexUnsupported };

inline std::expected<std::vector<DomainClaim>, DomainClaimError> planDomainClaims(
    std::span<const service::website_config::WebsiteDomainData> domains,
    std::span<const DnsZoneReference> zones) {
    std::vector<DomainClaim> claims;
    claims.reserve(domains.size());
    for (const auto& domain : domains) {
        auto key = domain.hostname;
        std::ranges::transform(key, key.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        DomainClaim claim{.id = domain.id, .key = std::move(key), .dnsMode = domain.dnsMode};
        if (claim.dnsMode == "managed") {
            const DnsZoneReference* owner = nullptr;
            std::size_t ownerLength = 0;
            for (const auto& zone : zones) {
                if (!service::common::domainBelongsToZone(domain.hostname, zone.domain)) continue;
                const auto length = service::common::normalizeDomainName(zone.domain).size();
                if (!owner || length > ownerLength) {
                    owner = &zone;
                    ownerLength = length;
                }
            }
            if (!owner) {
                return std::unexpected(DomainClaimError::managedZoneNotFound);
            }
            auto hostname = std::string_view{domain.hostname};
            if (hostname.starts_with("*.")) hostname.remove_prefix(2);
            if (service::common::normalizeDomainName(hostname) ==
                service::common::normalizeDomainName(owner->domain)) {
                return std::unexpected(DomainClaimError::managedZoneApexUnsupported);
            }
            claim.dnsZoneId = owner->id;
        }
        claims.push_back(std::move(claim));
    }
    return claims;
}

} // namespace service::website
