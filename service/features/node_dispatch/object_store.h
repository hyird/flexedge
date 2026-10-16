#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/db/db_transaction.h>

#include <stdexcept>
#include <string_view>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/features/node_dispatch/dispatch_entity.h"
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
    dispatch_object_entity record;
    record.set<"tenant_id">(tenant_id);
    record.set<"digest_sha256">(object.digest_sha256());
    record.set<"kind">(kind);
    record.set<"payload_envelope">(envelope);
    const auto objects = transaction.get_repository<dispatch_object_entity>();
    const auto inserted = co_await objects.upsert(record,
        {.conflict_paths_ = {"tenant_id", "digest_sha256"}, .do_nothing_ = true});
    if (inserted.affected_rows() == 0) {
        ruvia::db_query expressions;
        auto query = objects.create_query_builder();
        query.select({{"kind", {}}, {"payload_envelope", {}}})
            .where(expressions.binary(expressions.column("tenant_id"), ruvia::db_binary_operator::equal,
                expressions.value(tenant_id)))
            .and_where(expressions.binary(expressions.column("digest_sha256"), ruvia::db_binary_operator::equal,
                expressions.value(object.digest_sha256())))
            .take(1);
        const auto stored = co_await query.get_one<stored_object_payload>();
        if (!stored || stored->get<"kind">() != kind) {
            throw std::runtime_error("content-addressed delivery object conflicts with storage");
        }
        service::utils::sensitive_string stored_plaintext(
            service::utils::open_secret(stored->get<"payload_envelope">()));
        if (stored_plaintext.view() != plaintext) {
            throw std::runtime_error(
                "content-addressed delivery object failed storage verification");
        }
    }
    co_return;
}

}  // namespace service::node_dispatch
