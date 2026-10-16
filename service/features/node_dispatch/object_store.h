#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <stdexcept>
#include <string_view>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::node_dispatch {

inline ruvia::task<void> persist_object(ruvia::db_transaction& transaction, std::string_view tenant_id,
    const flexedge::node::v2::DeliveryObject& object) {
    const auto content = flexedge::node::open_artifact<flexedge::node::v2::DeliveryObjectContent>(
        object.digest_sha256(), object.content());
    if (!content) {
        throw std::runtime_error("delivery object digest does not match payload");
    }
    if (!content->has_website() && !content->has_certificate()) {
        throw std::runtime_error("delivery object has no supported payload");
    }
    const auto kind = content->has_website() ? std::string_view{"website"}
                                             : std::string_view{"certificate"};
    const std::string_view plaintext = object.content();
    const auto envelope = service::utils::seal_secret(plaintext);
    const auto inserted = co_await transaction.execute(
        "INSERT INTO sys_delivery_object (tenant_id, digest_sha256, kind, payload_envelope, "
        "created_at) VALUES ($1, $2, $3, $4, NOW()) ON CONFLICT (tenant_id, digest_sha256) DO "
        "NOTHING",
        tenant_id, object.digest_sha256(), kind, std::string_view(envelope));
    if (inserted.affected_rows() == 0) {
        const auto rows = co_await transaction.query(
            "SELECT kind, payload_envelope FROM sys_delivery_object WHERE tenant_id = $1 AND "
            "digest_sha256 = $2 LIMIT 1",
            tenant_id, object.digest_sha256());
        if (rows.size() != 1 || rows.front()[0].value().value_or("") != kind) {
            throw std::runtime_error("content-addressed delivery object conflicts with storage");
        }
        service::utils::sensitive_string stored(
            service::utils::open_secret(rows.front()[1].value().value_or("")));
        if (stored.view() != plaintext) {
            throw std::runtime_error(
                "content-addressed delivery object failed storage verification");
        }
    }
    co_return;
}

}  // namespace service::node_dispatch
