#pragma once

#include <algorithm>
#include <span>

#include "service/common/domain_name.h"
#include "service/domains/website/domain_claim_types.h"
#include "service/features/website_config/model.h"

namespace service::website {

// A bound certificate must cover at least one website hostname. Hostnames left
// uncovered by the whole selection are served over HTTP and do not reject the save.
inline const certificate_reference* find_unmatched_certificate(
    std::span<const certificate_reference> certificates,
    std::span<const service::website_config::website_domain_data> domains) {
    const auto matches = [&](const certificate_reference& certificate) {
        return std::ranges::any_of(certificate.names_, [&](const auto& name) {
            return std::ranges::any_of(domains, [&](const auto& domain) {
                return service::common::certificate_covers_hostname(name, domain.hostname_);
            });
        });
    };
    const auto unmatched = std::ranges::find_if_not(certificates, matches);
    return unmatched == certificates.end() ? nullptr : &*unmatched;
}

}  // namespace service::website
