#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent.error.h"
#include "service/domains/agent/agent.types.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::agent {

class AgentReadService final {
  public:
    ruvia::Task<bool> isCurrent(ruvia::Context& c, const AgentPrincipal& principal) const {
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM sys_node WHERE tenant_id = $1 AND id = $2 AND cluster_id = $3 AND "
            "agent_id = $4 AND registration_status = 'registered' AND deleted_at IS NULL LIMIT 1",
            principal.tenantId, principal.nodeId, principal.clusterId, principal.agentId);
        co_return !rows.empty();
    }

    ruvia::Task<DesiredSummary> desiredSummary(ruvia::Context& c,
                                               const AgentPrincipal& principal) const {
        const auto rows = co_await c.db().query(
            "SELECT node.node_spec_revision, release.id, release.manifest_digest FROM sys_node "
            "node "
            "INNER JOIN sys_cluster_release release ON release.tenant_id = node.tenant_id AND "
            "release.id = node.desired_release_id WHERE node.tenant_id = $1 AND node.id = $2 AND "
            "node.cluster_id = $3 AND node.registration_status = 'registered' AND "
            "node.agent_id = $4 AND node.deleted_at IS NULL "
            "LIMIT 1",
            principal.tenantId, principal.nodeId, principal.clusterId, principal.agentId);
        if (rows.empty()) {
            service::common::throwAppError(REVISION_INVALID);
        }
        co_return DesiredSummary{
            .nodeSpecRevision = rows.front()[0].as<std::int64_t>().value_or(1),
            .releaseId = std::string(rows.front()[1].value().value_or("")),
            .manifestDigest = std::string(rows.front()[2].value().value_or("")),
        };
    }

    ruvia::Task<flexedge::node::v2::ObjectBatch>
    objects(ruvia::Context& c, const AgentPrincipal& principal, std::string_view releaseId,
            const google::protobuf::RepeatedPtrField<std::string>& requested) const {
        std::unordered_set<std::string> missing;
        missing.reserve(static_cast<std::size_t>(requested.size()));
        for (const auto& digest : requested) {
            if (!missing.emplace(digest).second) {
                service::common::throwAppError(ARTIFACT_INVALID);
            }
        }
        const auto rows = co_await c.db().query(
            "SELECT mapping.object_digest, object.kind, object.payload_envelope FROM "
            "sys_cluster_release_object mapping INNER JOIN sys_cluster_release release ON "
            "release.tenant_id = mapping.tenant_id AND release.id = mapping.release_id INNER JOIN "
            "sys_delivery_object object ON object.tenant_id = mapping.tenant_id AND "
            "object.digest_sha256 = mapping.object_digest INNER JOIN sys_node node ON "
            "node.tenant_id "
            "= mapping.tenant_id AND node.desired_release_id = mapping.release_id WHERE "
            "mapping.tenant_id = $1 AND mapping.release_id = $2 AND release.cluster_id = $3 AND "
            "node.id = $4 AND node.agent_id = $5 AND "
            "node.registration_status = 'registered' AND node.deleted_at IS NULL ORDER BY "
            "mapping.position ASC",
            principal.tenantId, releaseId, principal.clusterId, principal.nodeId,
            principal.agentId);
        flexedge::node::v2::ObjectBatch result;
        result.set_release_id(releaseId);
        for (const auto& row : rows) {
            const auto digest = std::string(row[0].value().value_or(""));
            if (!missing.erase(digest)) {
                continue;
            }
            service::utils::SensitiveString plaintext(
                service::utils::openSecret(row[2].value().value_or("")));
            auto* object = result.add_objects();
            object->set_digest_sha256(digest);
            if (!flexedge::node::parseArtifact(plaintext.view(), *object->mutable_content()) ||
                flexedge::node::artifactDigest(object->content()) != digest ||
                (row[1].value().value_or("") == "website" && !object->content().has_website()) ||
                (row[1].value().value_or("") == "certificate" &&
                 !object->content().has_certificate())) {
                service::common::throwAppError(ARTIFACT_INVALID);
            }
        }
        if (!missing.empty()) {
            service::common::throwAppError(ARTIFACT_INVALID);
        }
        co_return result;
    }
};

inline const AgentReadService& agentReadService() {
    static const AgentReadService service;
    return service;
}

} // namespace service::agent
