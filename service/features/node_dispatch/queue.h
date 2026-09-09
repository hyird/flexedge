#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbTransaction.h>

#include "node/proto/artifact.h"
#include "service/features/node_dispatch/deployment_source.store.h"
#include "service/features/node_dispatch/object.store.h"
#include "service/features/node_dispatch/release.store.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/utils/secret.h"

namespace service::node_dispatch {

inline ruvia::Task<void> publishClusterRelease(ruvia::DbTransaction& transaction,
                                               std::string_view tenantId,
                                               std::string_view clusterId) {
    const auto allocation = co_await allocateClusterRelease(transaction, tenantId, clusterId,
        static_cast<std::int64_t>(flexedge::node::kClusterReleaseSchemaVersion));
    const auto& releaseId = allocation.id;
    const auto generation = allocation.generation;
    const auto deployment = co_await loadClusterDeploymentSource(
        transaction, std::string(tenantId), std::string(clusterId),
        allocation.accessDomain, allocation.enabled);
    const auto artifact = buildClusterRelease(releaseId, generation, deployment);

    std::int64_t position{};
    for (const auto& object : artifact.objects) {
        co_await persistObject(transaction, tenantId, object);
        co_await linkReleaseObject(transaction, tenantId, releaseId, position++, object.digest_sha256());
    }
    const auto manifestBytes = flexedge::node::serializeArtifact(artifact.manifest);
    const auto manifestEnvelope = service::utils::sealSecret(manifestBytes);
    co_await activateClusterRelease(transaction, tenantId, clusterId, releaseId,
        artifact.manifest.digest_sha256(), manifestEnvelope);

    co_return;
}

inline ruvia::Task<void> ensureClusterRelease(ruvia::DbTransaction& transaction,
                                              std::string_view tenantId,
                                              std::string_view clusterId) {
    const auto schema = co_await lockCurrentReleaseSchemaVersion(transaction, tenantId, clusterId);
    if (schema != static_cast<std::int64_t>(flexedge::node::kClusterReleaseSchemaVersion)) {
        co_await publishClusterRelease(transaction, tenantId, clusterId);
    }
    co_return;
}

inline ruvia::Task<void> enqueueCertificateConsumers(ruvia::DbTransaction& transaction,
                                                     std::string_view tenantId,
                                                     std::string_view certificateId) {
    const auto clusters = co_await findCertificateConsumerClusters(transaction, tenantId, certificateId);
    for (const auto& cluster : clusters) {
        co_await publishClusterRelease(transaction, tenantId, cluster);
    }
    co_return;
}

} // namespace service::node_dispatch
