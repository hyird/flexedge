#pragma once

#include <ruvia/core/task.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent_error.h"
#include "service/domains/agent/agent_types.h"
#include "service/domains/agent/desired_summary_store.h"
#include "service/domains/agent/release_objects_store.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::agent {

class agent_read_service final {
public:
    template <typename runtime_type>
    ruvia::task<bool> is_current(runtime_type& c, const agent_principal& principal) const {
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM sys_node WHERE tenant_id = $1 AND id = $2 AND cluster_id = $3 AND "
            "agent_id = $4 AND registration_status = 'registered' AND deleted_at IS NULL LIMIT 1",
            principal.tenant_id_, principal.node_id_, principal.cluster_id_, principal.agent_id_);
        co_return !rows.empty();
    }

    template <typename runtime_type>
    ruvia::task<service::agent::desired_summary> desired_summary(runtime_type& c,
        const agent_principal& principal) const {
        auto db = c.db();
        auto summary = co_await find_desired_summary(db, principal);
        if (!summary) {
            service::common::throw_app_error(revision_invalid);
        }
        co_return std::move(*summary);
    }

    template <typename runtime_type>
    ruvia::task<flexedge::node::v2::ObjectBatch>
    objects(runtime_type& c, const agent_principal& principal, std::string_view release_id,
        const google::protobuf::RepeatedPtrField<std::string>& requested) const {
        std::unordered_set<std::string> missing;
        missing.reserve(static_cast<std::size_t>(requested.size()));
        for (const auto& digest : requested) {
            if (!missing.emplace(digest).second) {
                service::common::throw_app_error(artifact_invalid);
            }
        }
        auto db = c.db();
        const auto records = co_await load_release_objects(db, principal, release_id);
        flexedge::node::v2::ObjectBatch result;
        result.set_release_id(release_id);
        for (const auto& record : records) {
            const auto& digest = record.digest_;
            if (!missing.erase(digest)) {
                continue;
            }
            service::utils::sensitive_string plaintext(
                service::utils::open_secret(record.payload_envelope_));
            auto* object = result.add_objects();
            object->set_digest_sha256(digest);
            if (!flexedge::node::parse_artifact(plaintext.view(), *object->mutable_content()) ||
                flexedge::node::artifact_digest(object->content()) != digest ||
                (record.kind_ == "website" && !object->content().has_website()) ||
                (record.kind_ == "certificate" &&
                    !object->content().has_certificate())) {
                service::common::throw_app_error(artifact_invalid);
            }
        }
        if (!missing.empty()) {
            service::common::throw_app_error(artifact_invalid);
        }
        co_return result;
    }
};

inline const agent_read_service& get_agent_read_service() {
    static const agent_read_service service;
    return service;
}

}  // namespace service::agent
