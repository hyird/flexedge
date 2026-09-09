#pragma once
#include <stdexcept>
#include <string_view>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>
#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::node_dispatch {

inline ruvia::Task<void> persistObject(ruvia::DbTransaction& transaction, std::string_view tenantId,
                                       const flexedge::node::v2::DeliveryObject& object) {
    if (!object.has_content() ||
        flexedge::node::artifactDigest(object.content()) != object.digest_sha256()) {
        throw std::runtime_error("delivery object digest does not match payload");
    }
    if (!object.content().has_website() && !object.content().has_certificate()) {
        throw std::runtime_error("delivery object has no supported payload");
    }
    const auto kind = object.content().has_website() ? std::string_view{"website"}
                                                     : std::string_view{"certificate"};
    const service::utils::SensitiveString plaintext(flexedge::node::serializeArtifact(object.content()));
    const auto envelope = service::utils::sealSecret(plaintext.view());
    const auto inserted = co_await transaction.execute(
        "INSERT INTO sys_delivery_object (tenant_id, digest_sha256, kind, payload_envelope, "
        "created_at) VALUES ($1, $2, $3, $4, NOW()) ON CONFLICT (tenant_id, digest_sha256) DO "
        "NOTHING",
        tenantId, object.digest_sha256(), kind, std::string_view(envelope));
    if (inserted.affectedRows() == 0) {
        const auto rows = co_await transaction.query(
            "SELECT kind, payload_envelope FROM sys_delivery_object WHERE tenant_id = $1 AND "
            "digest_sha256 = $2 LIMIT 1",
            tenantId, object.digest_sha256());
        if (rows.size() != 1 || rows.front()[0].value().value_or("") != kind) {
            throw std::runtime_error("content-addressed delivery object conflicts with storage");
        }
        service::utils::SensitiveString stored(
            service::utils::openSecret(rows.front()[1].value().value_or("")));
        if (stored.view() != plaintext.view()) {
            throw std::runtime_error(
                "content-addressed delivery object failed storage verification");
        }
    }
    co_return;
}

} // namespace service::node_dispatch
