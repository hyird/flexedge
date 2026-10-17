#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "service/common/domain_name.h"
#include "service/domains/dns_zone/dns_zone_reference.h"
#include "service/domains/website/domain_claim_types.h"
#include "service/features/website_config/model.h"
#include "service/utils/text.h"

namespace service::website {

enum class domain_claim_error { managed_zone_not_found };

inline std::variant<std::vector<domain_claim>, domain_claim_error> plan_domain_claims(
    std::span<const service::website_config::website_domain_data> domains,
    std::span<const service::dns_zone::dns_zone_reference> zones) {
    std::vector<domain_claim> claims;
    claims.reserve(domains.size());
    for (const auto& domain : domains) {
        domain_claim claim{.id_ = domain.id_,
            .key_ = service::utils::to_lower_ascii(domain.hostname_),
            .dns_mode_ = domain.dns_mode_};
        if (claim.dns_mode_ == "managed") {
            const service::dns_zone::dns_zone_reference* owner = nullptr;
            std::size_t owner_length = 0;
            for (const auto& zone : zones) {
                if (!service::common::domain_belongs_to_zone(domain.hostname_, zone.domain_)) {
                    continue;
                }
                const auto length = service::common::normalize_domain_name(zone.domain_).size();
                if (!owner || length > owner_length) {
                    owner = &zone;
                    owner_length = length;
                }
            }
            if (!owner) {
                return domain_claim_error::managed_zone_not_found;
            }
            claim.dns_zone_id_ = owner->id_;
        }
        claims.push_back(std::move(claim));
    }
    return claims;
}

}  // namespace service::website
