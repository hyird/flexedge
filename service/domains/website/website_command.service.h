#pragma once

#include "service/features/node_dispatch/notifications.h"
#include "service/features/live_resource/fanout.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/http.h"
#include "service/domains/website/website.error.h"
#include "service/domains/website/domain_claim.h"
#include "service/domains/website/aggregate.store.h"
#include "service/domains/website/relation.store.h"
#include "service/domains/website/reference.store.h"
#include "service/domains/website/website.types.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_config/mapper.h"
#include "service/features/website_dns/projection.h"

namespace service::website {

class WebsiteCommandService final {
  public:
    ruvia::Task<void> requestDnsProbe(ruvia::Context& c, const std::string& tenantId,
                                      const std::string& id) {
        auto transaction = co_await c.db().beginTransaction();
        const auto revision = co_await lockWebsiteRevision(transaction, tenantId, id);
        if (!revision) {
            service::common::throwAppError(WebsiteError::NOT_FOUND);
        }
        (void)co_await service::sync_runtime::upsertMarker(
            transaction, tenantId, service::sync_runtime::MarkerResourceType::website, id,
            service::sync_runtime::MarkerOperation::apply, *revision);
        co_await transaction.commit();
        service::node_dispatch::notifications::published(tenantId);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::websites,
                                              id);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks);
        co_return;
    }

    ruvia::Task<void> create(ruvia::Context& c, const std::string& tenantId,
                             const std::string& clusterId,
                             const ruvia::ValidatedJson<WebsiteSaveInput>& body) {
        const auto normalized = normalize(body.value());
        if (!normalized) {
            throwCorruptConfig();
        }
        const auto& status = normalized->status;
        const auto& config = normalized->config;
        const auto configJson = serializeConfig(c, config);
        try {
            auto transaction = co_await c.db().beginTransaction();
            co_await requireCluster(transaction, tenantId, clusterId);
            const auto domainClaims =
                co_await buildDomainClaims(transaction, tenantId, std::nullopt, config);
            co_await validateCertificates(transaction, tenantId, config);
            const auto websiteId = co_await insertWebsite(transaction, tenantId, clusterId, status, configJson);
            co_await replaceRelationProjections(transaction, tenantId, websiteId, domainClaims,
                                                config.certificateIds);
            co_await service::node_dispatch::publishClusterRelease(transaction, tenantId,
                                                                   clusterId);
            (void)co_await service::sync_runtime::upsertMarker(
                transaction, tenantId, service::sync_runtime::MarkerResourceType::website,
                websiteId, service::sync_runtime::MarkerOperation::apply, 1);
            co_await service::website_dns::reconcileConfigChange(transaction, tenantId,
                                                                 std::nullopt, configJson);
            co_await transaction.commit();
            service::node_dispatch::notifications::published(tenantId);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::websites, websiteId);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks);
        } catch (const ruvia::DbError& error) {
            if (isDomainClaimConflict(error)) {
                service::common::throwAppError(WebsiteError::DOMAIN_EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> update(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             const std::string& clusterId, std::int64_t expectedRevision,
                             const ruvia::ValidatedJson<WebsiteSaveInput>& body) {
        const auto normalized = normalize(body.value());
        if (!normalized) {
            throwCorruptConfig();
        }
        const auto& status = normalized->status;
        const auto& config = normalized->config;
        const auto configJson = serializeConfig(c, config);
        try {
            auto transaction = co_await c.db().beginTransaction();
            const auto previous = co_await lockWebsiteSnapshot(transaction, tenantId, id);
            if (!previous) {
                service::common::throwAppError(WebsiteError::NOT_FOUND);
            }
            if (previous->revision != expectedRevision) {
                service::common::throwAppError(WebsiteError::REVISION_CONFLICT);
            }
            co_await requireCluster(transaction, tenantId, clusterId);
            const auto domainClaims = co_await buildDomainClaims(transaction, tenantId, id, config);
            co_await validateCertificates(transaction, tenantId, config);
            const auto& previousConfig = previous->configJson;
            const auto& previousClusterId = previous->clusterId;
            const auto revision = co_await updateWebsite(transaction, tenantId, id, expectedRevision,
                                                         clusterId, status, configJson);
            if (!revision) {
                service::common::throwAppError(WebsiteError::REVISION_CONFLICT);
            }
            co_await replaceRelationProjections(transaction, tenantId, id, domainClaims, config.certificateIds);
            if (previousClusterId != clusterId) {
                co_await service::node_dispatch::publishClusterRelease(transaction, tenantId,
                                                                       previousClusterId);
            }
            co_await service::node_dispatch::publishClusterRelease(transaction, tenantId,
                                                                   clusterId);
            (void)co_await service::sync_runtime::upsertMarker(
                transaction, tenantId, service::sync_runtime::MarkerResourceType::website, id,
                service::sync_runtime::MarkerOperation::apply, *revision);
            co_await service::website_dns::reconcileConfigChange(transaction, tenantId,
                                                                 previousConfig, configJson);
            co_await transaction.commit();
            service::node_dispatch::notifications::published(tenantId);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::websites, id);
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks);
        } catch (const ruvia::DbError& error) {
            if (isDomainClaimConflict(error)) {
                service::common::throwAppError(WebsiteError::DOMAIN_EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto previous = co_await softDeleteWebsite(transaction, tenantId, id, expectedRevision);
        if (!previous) {
            if (!(co_await findWebsiteRevision(transaction, tenantId, id))) {
                service::common::throwAppError(WebsiteError::NOT_FOUND);
            }
            service::common::throwAppError(WebsiteError::REVISION_CONFLICT);
        }
        const auto& previousConfig = previous->configJson;
        const auto& clusterId = previous->clusterId;
        co_await clearRelationProjections(transaction, tenantId, id);
        co_await service::node_dispatch::publishClusterRelease(transaction, tenantId, clusterId);
        co_await service::sync_runtime::removeMarker(
            transaction, tenantId, service::sync_runtime::MarkerResourceType::website, id);
        co_await service::website_dns::reconcileConfigChange(transaction, tenantId, previousConfig,
                                                             std::nullopt);
        co_await transaction.commit();
        service::node_dispatch::notifications::published(tenantId);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::websites, id);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::accessHistory, id);
        service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks);
        co_return;
    }

  private:
    static std::string serializeConfig(ruvia::Context& c,
                                       const service::website_config::WebsiteConfigData& input) {
        const auto output = service::website_config::toOutput(input, {.resource = c.resource()});
        const auto json = ruvia::toJson(output, {.resource = c.resource()});
        return std::string(json.data(), json.size());
    }

    static ruvia::Task<void> requireCluster(ruvia::DbTransaction& db, const std::string& tenantId,
                                            const std::string& clusterId) {
        if (!(co_await lockAvailableCluster(db, tenantId, clusterId))) {
            service::common::throwAppError(WebsiteError::CLUSTER_UNAVAILABLE);
        }
        co_return;
    }

    static ruvia::Task<std::vector<DomainClaim>>
    buildDomainClaims(ruvia::DbTransaction& db, const std::string& tenantId,
                      const std::optional<std::string>& excludedWebsiteId,
                      const service::website_config::WebsiteConfigData& config) {
        const auto zones = co_await lockDnsZoneReferences(db, tenantId);
        auto planned = planDomainClaims(config.domains, zones);
        if (!planned) {
            switch (planned.error()) {
            case DomainClaimError::managedZoneNotFound:
                service::common::throwAppError(WebsiteError::MANAGED_ZONE_NOT_FOUND);
            case DomainClaimError::managedZoneApexUnsupported:
                service::common::throwAppError(WebsiteError::MANAGED_ZONE_APEX_UNSUPPORTED);
            }
        }
        auto claims = std::move(*planned);
        if (co_await hasConflictingDomainClaims(db, tenantId, claims, excludedWebsiteId)) {
            service::common::throwAppError(WebsiteError::DOMAIN_EXISTS);
        }
        co_return claims;
    }

    static ruvia::Task<void>
    validateCertificates(ruvia::DbTransaction& db, const std::string& tenantId,
                         const service::website_config::WebsiteConfigData& config) {
        const auto httpsEnabled = config.httpsEnabled;
        const auto& certificateIds = config.certificateIds;
        if ((httpsEnabled && certificateIds.empty()) ||
            (!httpsEnabled && !certificateIds.empty())) {
            service::common::throwAppError(WebsiteError::HTTPS_CERTIFICATE_SELECTION_INVALID);
        }
        if (certificateIds.empty()) {
            co_return;
        }

        if (!(co_await lockAvailableCertificates(db, tenantId, certificateIds))) {
            service::common::throwAppError(WebsiteError::CERTIFICATE_UNAVAILABLE);
        }
        co_return;
    }

    [[noreturn]] static void throwCorruptConfig() {
        service::common::throwAppError(service::common::kServerErrorCode, "聚合配置损坏", 500);
    }
};

inline WebsiteCommandService& websiteCommandService() {
    static WebsiteCommandService service;
    return service;
}

} // namespace service::website
