#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/Context.h>

#include "service/common/domain_name.h"
#include "service/domains/website/website.types.h"
#include "service/features/node_runtime/model.h"
#include "service/features/website_config/model.h"
#include "service/features/website_dns/model.h"

namespace service::website::detail {

struct BoundCertificate final {
    std::string id;
    std::vector<std::string> domains;
    bool usable;
};

struct OriginRuntimeState final {
    std::string nodeId;
    std::string nodeName;
    service::node_runtime::NodeRuntimeData::OriginHealth health;
};

inline WebsiteRuntimeDto toRuntime(ruvia::Context& c,
                                   const service::website_config::WebsiteConfigData& config,
                                   const service::website_dns::WebsiteRuntimeData& input,
                                   const std::vector<BoundCertificate>& certificates,
                                   std::int64_t targetCount, std::int64_t syncedCount,
                                   const std::vector<OriginRuntimeState>& originStates = {}) {
    WebsiteRuntimeDto output(c);
    const auto deployStatus = targetCount == 0             ? std::string_view{"no_nodes"}
                              : syncedCount == targetCount ? std::string_view{"applied"}
                              : syncedCount == 0           ? std::string_view{"pending"}
                                                           : std::string_view{"partial"};
    output.set<"deployStatus">(deployStatus);
    output.set<"targetNodeCount">(targetCount);
    output.set<"syncedNodeCount">(syncedCount);
    auto& domains = output.ensure<"domainStates">();
    for (const auto& domain : config.domains) {
        const auto state = std::ranges::find_if(input.domainStates, [&](const auto& candidate) {
            return candidate.id && *candidate.id == domain.id;
        });
        const auto* runtimeState = state == input.domainStates.end() ? nullptr : &*state;
        const bool https =
            config.httpsEnabled && std::ranges::any_of(certificates, [&](const auto& certificate) {
                return certificate.usable &&
                       std::ranges::any_of(certificate.domains, [&](const auto& certificateDomain) {
                           return service::common::certificateCoversHostname(certificateDomain,
                                                                             domain.hostname);
                       });
            });
        auto& item = domains.emplace_back(c);
        const auto resolutionStatus = runtimeState && runtimeState->resolutionStatus
                                          ? std::string_view(*runtimeState->resolutionStatus)
                                          : std::string_view{"unverified"};
        item.set<"id">(domain.id);
        item.set<"accessProtocol">(https ? std::string_view{"https"} : std::string_view{"http"});
        item.set<"resolutionStatus">(resolutionStatus);
        if (runtimeState && runtimeState->lastVerifiedAt) {
            item.set<"lastVerifiedAt">(*runtimeState->lastVerifiedAt);
        }
        if (runtimeState && runtimeState->lastError) {
            item.set<"lastError">(*runtimeState->lastError);
        }
    }
    auto& origins = output.ensure<"originStates">();
    origins.reserve(originStates.size());
    for (const auto& state : originStates) {
        auto& item = origins.emplace_back(c);
        item.set<"nodeId">(state.nodeId);
        item.set<"nodeName">(state.nodeName);
        item.set<"originId">(state.health.originId);
        item.set<"status">(state.health.status);
        item.set<"checkedAtUnixMillis">(state.health.checkedAtUnixMillis);
        item.set<"latencyMillis">(state.health.latencyMillis);
        if (state.health.lastError) {
            item.set<"lastError">(*state.health.lastError);
        }
    }
    return output;
}

} // namespace service::website::detail
