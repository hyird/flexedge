#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent.error.h"
#include "service/domains/agent/agent.types.h"
#include "service/domains/agent/release_objects.store.h"
#include "service/domains/agent/desired_summary.store.h"
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
        auto db = c.db();
        auto summary = co_await findDesiredSummary(db, principal);
        if (!summary) service::common::throwAppError(REVISION_INVALID);
        co_return std::move(*summary);
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
        auto db = c.db();
        const auto records = co_await loadReleaseObjects(db, principal, releaseId);
        flexedge::node::v2::ObjectBatch result;
        result.set_release_id(releaseId);
        for (const auto& record : records) {
            const auto& digest = record.digest;
            if (!missing.erase(digest)) {
                continue;
            }
            service::utils::SensitiveString plaintext(
                service::utils::openSecret(record.payloadEnvelope));
            auto* object = result.add_objects();
            object->set_digest_sha256(digest);
            if (!flexedge::node::parseArtifact(plaintext.view(), *object->mutable_content()) ||
                flexedge::node::artifactDigest(object->content()) != digest ||
                (record.kind == "website" && !object->content().has_website()) ||
                (record.kind == "certificate" &&
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
