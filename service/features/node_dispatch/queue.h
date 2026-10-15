#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "node/proto/artifact.h"
#include "service/features/node_dispatch/deployment_source_store.h"
#include "service/features/node_dispatch/object_store.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/node_dispatch/release_store.h"
#include "service/utils/secret.h"

namespace service::node_dispatch {

// Builds and activates a new release of the cluster in the caller's
// transaction; `origin` owns the node publish subtasks it creates.
inline ruvia::task<void> publish_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    std::string_view cluster_id, service::operation::work_origin& origin) {
    const auto allocation = co_await allocate_cluster_release(transaction, tenant_id, cluster_id,
        static_cast<std::int64_t>(flexedge::node::cluster_release_schema_version));
    const auto& release_id = allocation.id_;
    const auto generation = allocation.generation_;
    const auto deployment = co_await load_cluster_deployment_source(
        transaction, std::string(tenant_id), std::string(cluster_id),
        allocation.access_domain_, allocation.enabled_);
    const auto artifact = build_cluster_release(release_id, generation, deployment);

    std::int64_t position{};
    for (const auto& object : artifact.objects_) {
        co_await persist_object(transaction, tenant_id, object);
        co_await link_release_object(transaction, tenant_id, release_id, position++, object.digest_sha256());
    }
    const auto manifest_bytes = flexedge::node::serialize_artifact(artifact.manifest_);
    const auto manifest_envelope = service::utils::seal_secret(manifest_bytes);
    co_await activate_cluster_release(transaction, tenant_id, cluster_id, release_id, generation,
        artifact.manifest_.digest_sha256(), manifest_envelope, origin);

    co_return;
}

// Publishes only when the cluster has no release of the current schema.
inline ruvia::task<void> ensure_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    std::string_view cluster_id, service::operation::work_origin& origin) {
    const auto schema = co_await lock_current_release_schema_version(transaction, tenant_id, cluster_id);
    if (schema != static_cast<std::int64_t>(flexedge::node::cluster_release_schema_version)) {
        co_await publish_cluster_release(transaction, tenant_id, cluster_id, origin);
    }
    co_return;
}

inline ruvia::task<void> enqueue_certificate_consumers(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    std::string_view certificate_id, service::operation::work_origin& origin) {
    const auto clusters = co_await find_certificate_consumer_clusters(transaction, tenant_id, certificate_id);
    for (const auto& cluster : clusters) {
        co_await publish_cluster_release(transaction, tenant_id, cluster, origin);
    }
    co_return;
}

}  // namespace service::node_dispatch
