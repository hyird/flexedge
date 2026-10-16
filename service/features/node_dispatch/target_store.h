#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/db/db_transaction.h>

#include <string>
#include <string_view>

#include "service/features/background/queued_work.h"
#include "service/features/node_dispatch/dispatch_entity.h"

namespace service::node_dispatch {

namespace target_store_detail {

inline ruvia::db_expression pending_target(ruvia::db_query& query, std::string_view tenant,
    std::string_view node, std::string_view alias = {}) {
    auto predicate = query.binary(query.binary(query.column("tenant_id", alias), ruvia::db_binary_operator::equal,
                                      query.value(tenant)),
        ruvia::db_binary_operator::and_value,
        query.binary(query.column("node_id", alias), ruvia::db_binary_operator::equal, query.value(node)));
    return query.binary(predicate, ruvia::db_binary_operator::and_value,
        query.binary(query.column("status", alias), ruvia::db_binary_operator::in,
            query.list({query.value("pending"), query.value("failed")})));
}

inline ruvia::db_expression release_scope(ruvia::db_query& query, std::string_view cluster) {
    return query.binary(query.binary(query.column("tenant_id", "release"), ruvia::db_binary_operator::equal,
                            query.column("tenant_id", "target")),
        ruvia::db_binary_operator::and_value,
        query.binary(query.binary(query.column("id", "release"), ruvia::db_binary_operator::equal,
                         query.column("release_id", "target")),
            ruvia::db_binary_operator::and_value,
            query.binary(query.column("cluster_id", "release"), ruvia::db_binary_operator::equal, query.value(cluster))));
}

}  // namespace target_store_detail

inline ruvia::task<void> exclude_pending_node_targets(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& node_id, service::background::queued_work& work) {
    ruvia::db_query expressions;
    auto update = transaction.get_repository<dispatch_target_entity>().create_update_builder();
    update.set("status", expressions.value("excluded")).set("updated_at", expressions.call("now")).where(target_store_detail::pending_target(expressions, tenant_id, node_id));
    if ((co_await update.execute()).affected_rows() != 0) {
        work.changed(node_id);
    }
}

inline ruvia::task<void> exclude_pending_node_cluster_targets(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& node_id, const std::string& cluster_id,
    service::background::queued_work& work) {
    ruvia::db_query expressions;
    auto update = transaction.get_repository<dispatch_target_entity>().create_update_builder("target");
    update.set("status", expressions.value("excluded")).set("updated_at", expressions.call("now")).update_from<dispatch_release_entity>("release").where(target_store_detail::pending_target(expressions, tenant_id, node_id, "target")).and_where(target_store_detail::release_scope(expressions, cluster_id));
    if ((co_await update.execute()).affected_rows() != 0) {
        work.changed(node_id);
    }
}

inline ruvia::task<bool> mark_node_target_applied(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view release_id, std::string_view node_id) {
    ruvia::db_query expressions;
    auto update = transaction.get_repository<dispatch_target_entity>().create_update_builder();
    update.set("status", expressions.value("applied")).set("failed_phase", expressions.null_value()).set("error_code", expressions.null_value()).set("last_error", expressions.null_value()).set("retryable", expressions.null_value()).set("applied_at", expressions.call("now")).set("updated_at", expressions.call("now")).where(target_store_detail::pending_target(expressions, tenant_id, node_id)).and_where(expressions.binary(expressions.column("release_id"), ruvia::db_binary_operator::equal, expressions.value(release_id)));
    co_return (co_await update.execute()).affected_rows() != 0;
}

struct target_failure {
    std::string phase_;
    std::string error_code_;
    std::string error_;
    bool retryable_{};
};

inline ruvia::task<bool> mark_node_target_failed(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view release_id, std::string_view node_id,
    std::string_view cluster_id, const target_failure& failure) {
    ruvia::db_query expressions;
    auto update = transaction.get_repository<dispatch_target_entity>().create_update_builder("target");
    update.set("status", expressions.value("failed")).set("failed_phase", expressions.value(failure.phase_)).set("error_code", expressions.value(failure.error_code_)).set("last_error", expressions.value(failure.error_)).set("retryable", expressions.value(failure.retryable_)).set("updated_at", expressions.call("now")).update_from<dispatch_release_entity>("release").where(target_store_detail::pending_target(expressions, tenant_id, node_id, "target")).and_where(expressions.binary(expressions.column("release_id", "target"), ruvia::db_binary_operator::equal, expressions.value(release_id))).and_where(target_store_detail::release_scope(expressions, cluster_id));
    co_return (co_await update.execute()).affected_rows() != 0;
}

}  // namespace service::node_dispatch
