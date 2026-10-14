#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "service/common/domain_name.h"
#include "service/domains/website/domain_claim_types.h"
#include "service/features/website_config/model.h"

namespace service::website {

enum class domain_claim_error { managed_zone_not_found,
    managed_zone_apex_unsupported };

inline std::variant<std::vector<domain_claim>, domain_claim_error> plan_domain_claims(
    std::span<const service::website_config::website_domain_data> domains,
    std::span<const dns_zone_reference> zones) {
    std::vector<domain_claim> claims;
    claims.reserve(domains.size());
    for (const auto& domain : domains) {
        auto key = domain.hostname_;
        std::ranges::transform(key, key.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        domain_claim claim{.id_ = domain.id_, .key_ = std::move(key), .dns_mode_ = domain.dns_mode_};
        if (claim.dns_mode_ == "managed") {
            const dns_zone_reference* owner = nullptr;
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
            auto hostname = std::string_view{domain.hostname_};
            if (hostname.starts_with("*.")) {
                hostname.remove_prefix(2);
            }
            if (service::common::normalize_domain_name(hostname) ==
                service::common::normalize_domain_name(owner->domain_)) {
                return domain_claim_error::managed_zone_apex_unsupported;
            }
            claim.dns_zone_id_ = owner->id_;
        }
        claims.push_back(std::move(claim));
    }
    return claims;
}

}  // namespace service::website
