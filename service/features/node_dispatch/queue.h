#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "node/proto/artifact.h"
#include "service/features/background/queued_work.h"
#include "service/features/node_dispatch/deployment_source_store.h"
#include "service/features/node_dispatch/object_store.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/node_dispatch/release_store.h"
#include "service/utils/secret.h"

namespace service::node_dispatch {

// Builds and activates a new release of the cluster in the caller's
// transaction; the node tasks of its targets change in `work`. Returns the
// new current release id.
inline ruvia::task<std::string> publish_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    std::string_view cluster_id, service::background::queued_work& work) {
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
    co_await activate_cluster_release(transaction, tenant_id, cluster_id, release_id,
        artifact.manifest_.digest_sha256(), manifest_envelope, work);

    co_return release_id;
}

// Publishes only when the cluster has no release of the current schema;
// returns the cluster's current release id.
inline ruvia::task<std::string> ensure_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    std::string_view cluster_id, service::background::queued_work& work) {
    auto current = co_await lock_current_release(transaction, tenant_id, cluster_id);
    if (!current ||
        current->schema_version_ != static_cast<std::int64_t>(flexedge::node::cluster_release_schema_version)) {
        co_return co_await publish_cluster_release(transaction, tenant_id, cluster_id, work);
    }
    co_return std::move(current->id_);
}

inline ruvia::task<void> enqueue_certificate_consumers(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    std::string_view certificate_id, service::background::queued_work& work) {
    const auto clusters = co_await find_certificate_consumer_clusters(transaction, tenant_id, certificate_id);
    for (const auto& cluster : clusters) {
        co_await publish_cluster_release(transaction, tenant_id, cluster, work);
    }
    co_return;
}

}  // namespace service::node_dispatch
