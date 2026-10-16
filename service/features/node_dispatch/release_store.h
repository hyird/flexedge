#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/background/queued_work.h"
#include "service/features/node_dispatch/dispatch_entity.h"

namespace service::node_dispatch {

namespace release_store_detail {

inline ruvia::db_expression equal(ruvia::db_query& query, std::string_view column,
    std::string_view value, std::string_view alias = {}) {
    return query.binary(query.column(column, alias), ruvia::db_binary_operator::equal, query.value(value));
}

inline ruvia::db_expression same(ruvia::db_query& query, std::string_view column,
    std::string_view left, std::string_view right) {
    return query.binary(query.column(column, left), ruvia::db_binary_operator::equal, query.column(column, right));
}

// Disabled registered nodes keep the desired release so re-enabling does not
// require another publication; only enabled nodes receive an active target.
inline ruvia::db_expression registered_node(ruvia::db_query& query, std::string_view tenant,
    std::string_view cluster, std::string_view alias) {
    auto predicate = query.binary(equal(query, "tenant_id", tenant, alias), ruvia::db_binary_operator::and_value,
        equal(query, "cluster_id", cluster, alias));
    predicate = query.binary(predicate, ruvia::db_binary_operator::and_value,
        equal(query, "registration_status", "registered", alias));
    for (const auto column : {"agent_id", "node_secret_hash", "node_secret_envelope"}) {
        predicate = query.binary(predicate, ruvia::db_binary_operator::and_value,
            query.unary(ruvia::db_unary_operator::is_not_null, query.column(column, alias)));
    }
    return query.binary(predicate, ruvia::db_binary_operator::and_value,
        query.unary(ruvia::db_unary_operator::is_null, query.column("deleted_at", alias)));
}

}  // namespace release_store_detail

struct release_allocation {
    std::string id_;
    std::int64_t generation_{};
    std::string access_domain_;
    bool enabled_{};
};

inline ruvia::task<release_allocation> allocate_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view cluster_id, std::int64_t schema_version) {
    if (!std::in_range<std::int32_t>(schema_version)) {
        throw std::out_of_range("cluster release schema version is out of range");
    }
    using namespace release_store_detail;
    ruvia::db_query expressions;
    auto allocation = transaction.get_repository<dispatch_cluster_entity>().create_update_builder("cluster");
    const auto access_domain = expressions.binary(expressions.binary(expressions.column("hostname_prefix", "cluster"),
                                                      ruvia::db_binary_operator::concat, expressions.value(".")),
        ruvia::db_binary_operator::concat,
        expressions.column("domain", "zone"));
    allocation.set("release_generation", expressions.binary(expressions.column("release_generation", "cluster"),
                                             ruvia::db_binary_operator::add, expressions.value(1)))
        .set("updated_at", expressions.call("now"))
        .update_from<dispatch_zone_entity>("zone")
        .where(equal(expressions, "tenant_id", tenant_id, "cluster"))
        .and_where(equal(expressions, "id", cluster_id, "cluster"))
        .and_where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "cluster")))
        .and_where(same(expressions, "tenant_id", "zone", "cluster"))
        .and_where(expressions.binary(expressions.column("id", "zone"), ruvia::db_binary_operator::equal,
            expressions.column("dns_zone_id", "cluster")))
        .and_where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "zone")))
        .returning({{"generation", expressions.column("release_generation", "cluster")},
            {"access_domain", access_domain}, {"enabled", equal(expressions, "status", "enabled", "cluster")}});
    const auto clusters = co_await allocation.get_many<allocated_cluster>();
    if (clusters.empty()) {
        throw std::runtime_error("cluster release target does not exist");
    }
    const auto& cluster = clusters[0];
    dispatch_release_entity release;
    release.set<"tenant_id">(tenant_id);
    release.set<"cluster_id">(cluster_id);
    release.set<"generation">(cluster.get<"generation">());
    release.set<"schema_version">(static_cast<std::int32_t>(schema_version));
    release.set<"status">("building");
    auto insert = transaction.get_repository<dispatch_release_entity>().create_insert_builder(release);
    insert.returning({{"id", {}}});
    const auto releases = co_await insert.get_many<release_identity>();
    if (releases.empty()) {
        throw std::runtime_error("cluster release identity could not be allocated");
    }
    co_return release_allocation{.id_ = std::string(releases[0].get<"id">()),
        .generation_ = cluster.get<"generation">(),
        .access_domain_ = std::string(cluster.get<"access_domain">()),
        .enabled_ = cluster.get<"enabled">()};
}

inline ruvia::task<void> link_release_object(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view release_id, std::int64_t position, std::string_view digest) {
    if (!std::in_range<std::int32_t>(position)) {
        throw std::out_of_range("cluster release object position is out of range");
    }
    dispatch_mapping_entity mapping;
    mapping.set<"tenant_id">(tenant_id);
    mapping.set<"release_id">(release_id);
    mapping.set<"position">(static_cast<std::int32_t>(position));
    mapping.set<"object_digest">(digest);
    (void)co_await transaction.get_repository<dispatch_mapping_entity>().insert(mapping);
}

// The caller holds the cluster lock from allocation through activation and
// commits the release, node targets and resulting task notifications together.
inline ruvia::task<void> activate_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view cluster_id, std::string_view release_id,
    std::string_view manifest_digest, std::string_view manifest_envelope, service::background::queued_work& work) {
    using namespace release_store_detail;
    ruvia::db_query expressions;
    auto eligible = transaction.get_repository<dispatch_node_entity>().create_query_builder("node");
    eligible.select({{"tenant_id", expressions.column("tenant_id", "node")}, {"release_id", expressions.value(release_id)},
                        {"node_id", expressions.column("id", "node")}, {"status", expressions.value("pending")},
                        {"updated_at", expressions.call("now")}})
        .where(registered_node(expressions, tenant_id, cluster_id, "node"))
        .and_where(equal(expressions, "status", "enabled", "node"));
    auto insert = transaction.get_repository<dispatch_target_entity>().create_insert_builder();
    insert.insert_from({"tenant_id", "release_id", "node_id", "status", "updated_at"}, eligible)
        .returning({{"node_id", {}}});
    const auto targets = co_await insert.get_many<target_node_identity>();
    for (const auto& target : targets) {
        work.changed(target.get<"node_id">());
    }
    auto previous = transaction.get_repository<dispatch_target_entity>().create_query_builder("target");
    previous.select({{"node_id", expressions.column("node_id", "target")}})
        .join<dispatch_release_entity>(ruvia::db_join_type::inner, "release",
            expressions.binary(same(expressions, "tenant_id", "release", "target"), ruvia::db_binary_operator::and_value,
                expressions.binary(expressions.column("id", "release"), ruvia::db_binary_operator::equal,
                    expressions.column("release_id", "target"))))
        .where(equal(expressions, "tenant_id", tenant_id, "target"))
        .and_where(equal(expressions, "cluster_id", cluster_id, "release"))
        .and_where(expressions.binary(expressions.column("id", "release"), ruvia::db_binary_operator::not_equal,
            expressions.value(release_id)))
        .and_where(expressions.binary(expressions.column("status", "target"), ruvia::db_binary_operator::in,
            expressions.list({expressions.value("pending"), expressions.value("failed")})))
        .distinct();
    const auto superseded = co_await previous.get_many<target_node_identity>();
    for (const auto& target : superseded) {
        work.changed(target.get<"node_id">());
    }
    auto activate = transaction.get_repository<dispatch_release_entity>().create_update_builder();
    activate.set("manifest_digest", expressions.value(manifest_digest)).set("manifest_envelope", expressions.value(manifest_envelope)).set("status", expressions.value("active")).set("target_node_count", expressions.value(static_cast<std::int64_t>(targets.size()))).set("activated_at", expressions.call("now")).where(equal(expressions, "tenant_id", tenant_id)).and_where(equal(expressions, "id", release_id)).and_where(equal(expressions, "status", "building"));
    (void)co_await activate.execute();
    auto supersede = transaction.get_repository<dispatch_release_entity>().create_update_builder();
    supersede.set("status", expressions.value("superseded")).where(equal(expressions, "tenant_id", tenant_id)).and_where(equal(expressions, "cluster_id", cluster_id)).and_where(equal(expressions, "status", "active")).and_where(expressions.binary(expressions.column("id"), ruvia::db_binary_operator::not_equal, expressions.value(release_id)));
    (void)co_await supersede.execute();
    auto update_cluster = transaction.get_repository<dispatch_cluster_entity>().create_update_builder();
    update_cluster.set("current_release_id", expressions.value(release_id)).set("updated_at", expressions.call("now")).where(equal(expressions, "tenant_id", tenant_id)).and_where(equal(expressions, "id", cluster_id));
    (void)co_await update_cluster.execute();
    auto update_nodes = transaction.get_repository<dispatch_node_entity>().create_update_builder("node");
    update_nodes.set("desired_release_id", expressions.value(release_id)).set("updated_at", expressions.call("now")).where(registered_node(expressions, tenant_id, cluster_id, "node"));
    (void)co_await update_nodes.execute();
}

struct current_release {
    std::string id_;
    std::optional<std::int64_t> schema_version_{};
};

inline ruvia::task<std::optional<current_release>> lock_current_release(
    ruvia::db_transaction& transaction, std::string_view tenant_id, std::string_view cluster_id) {
    using namespace release_store_detail;
    ruvia::db_query expressions;
    auto query = transaction.get_repository<dispatch_cluster_entity>().create_query_builder("cluster");
    query.select({{"id", expressions.column("current_release_id", "cluster")},
                     {"schema_version", expressions.column("schema_version", "release")}})
        .join<dispatch_release_entity>(ruvia::db_join_type::left, "release",
            expressions.binary(same(expressions, "tenant_id", "release", "cluster"), ruvia::db_binary_operator::and_value,
                expressions.binary(expressions.column("id", "release"), ruvia::db_binary_operator::equal,
                    expressions.column("current_release_id", "cluster"))))
        .where(equal(expressions, "tenant_id", tenant_id, "cluster"))
        .and_where(equal(expressions, "id", cluster_id, "cluster"))
        .and_where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "cluster")))
        .take(1)
        .set_lock({.mode_ = ruvia::db_row_lock::update, .tables_ = {"cluster"}});
    const auto release = co_await query.get_one<locked_release>();
    if (!release) {
        throw std::runtime_error("cluster release target does not exist");
    }
    if (release->is_null<"id">()) {
        co_return std::nullopt;
    }
    co_return current_release{.id_ = std::string(release->get<"id">()),
        .schema_version_ = release->is_null<"schema_version">() ? std::nullopt : std::optional<std::int64_t>{release->get<"schema_version">()}};
}

inline ruvia::task<std::vector<std::string>> find_certificate_consumer_clusters(
    ruvia::db_transaction& transaction, std::string_view tenant_id, std::string_view certificate_id) {
    using namespace release_store_detail;
    ruvia::db_query expressions;
    auto query = transaction.get_repository<dispatch_certificate_entity>().create_query_builder("certificate");
    query.select({{"cluster_id", expressions.column("cluster_id", "website")}})
        .join<dispatch_binding_entity>(ruvia::db_join_type::inner, "binding",
            expressions.binary(same(expressions, "tenant_id", "binding", "certificate"), ruvia::db_binary_operator::and_value,
                expressions.binary(expressions.column("certificate_id", "binding"), ruvia::db_binary_operator::equal,
                    expressions.column("id", "certificate"))))
        .join<dispatch_website_entity>(ruvia::db_join_type::inner, "website",
            expressions.binary(same(expressions, "tenant_id", "website", "binding"), ruvia::db_binary_operator::and_value,
                expressions.binary(expressions.column("id", "website"), ruvia::db_binary_operator::equal,
                    expressions.column("website_id", "binding"))))
        .where(equal(expressions, "tenant_id", tenant_id, "certificate"))
        .and_where(equal(expressions, "id", certificate_id, "certificate"))
        .and_where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "website")))
        .order_by(expressions.column("cluster_id", "website"))
        .distinct();
    const auto clusters = co_await query.get_many<consumer_cluster_identity>();
    std::vector<std::string> result;
    result.reserve(clusters.size());
    for (const auto& cluster : clusters) {
        result.emplace_back(cluster.get<"cluster_id">());
    }
    co_return result;
}

}  // namespace service::node_dispatch
