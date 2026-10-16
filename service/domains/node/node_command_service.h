#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db_transaction.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/node/endpoint_claim_store.h"
#include "service/domains/node/node_error.h"
#include "service/domains/node/node_mapper.h"
#include "service/domains/node/node_types.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/dns_sync/line.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/node_config/model.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_dispatch/target_store.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::node {

class node_command_service final {
public:
    ruvia::task<node_credentials_dto> create(ruvia::context& c, const std::string& tenant_id,
        const ruvia::validated_json<node_save_input>& config) {
        const auto normalized = normalize(config.value());
        if (!normalized) {
            throw_corrupt_node_config();
        }
        const auto& cluster_id = normalized->cluster_id_;
        const auto& name = normalized->name_;
        const auto& status = normalized->status_;
        const auto& stored_config = normalized->config_;
        const auto config_json = serialize_node_config(c, stored_config);

        node_credentials_dto result(ruvia::model_options{.resource_ = c.pool()});
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto agent_id = service::utils::random_token().substr(0, 32);
            service::utils::sensitive_string secret(service::utils::random_token());
            const auto secret_hash = service::utils::token_hash(secret.view());
            const auto secret_envelope = service::utils::seal_secret(secret.view());
            co_await require_cluster(transaction, tenant_id, cluster_id, stored_config);
            const auto rows = co_await transaction.query(
                "INSERT INTO sys_node (tenant_id, cluster_id, name, status, "
                "registration_status, revision, node_spec_revision, applied_node_spec_revision, "
                "agent_id, node_secret_hash, node_secret_envelope, config, runtime, created_at, "
                "updated_at) "
                "VALUES ($1, $2, $3, $4, 'pending', 1, 1, 0, $5, $6, $7, $8::jsonb, "
                "'{}'::jsonb, "
                "NOW(), NOW()) RETURNING id",
                service::common::db_params(
                    ruvia::db_value{tenant_id}, ruvia::db_value{cluster_id}, ruvia::db_value{name},
                    ruvia::db_value{status}, ruvia::db_value{agent_id}, ruvia::db_value{secret_hash},
                    ruvia::db_value{secret_envelope}, ruvia::db_value{std::string_view(config_json)}));
            const auto node_id = std::string(rows.front()[0].value().value_or(""));
            auto origin = start(c, service::operation::action::node_create, node_id);
            (void)co_await origin.require_operation(transaction, tenant_id);
            co_await service::node_dispatch::ensure_cluster_release(transaction, tenant_id, cluster_id, origin);
            co_await replace_endpoint_claims(transaction, tenant_id, node_id, stored_config.endpoints_);
            co_await service::cluster_dns::reconcile_cluster(transaction, tenant_id, cluster_id, origin);
            co_await transaction.commit();
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config()
                .changed(service::live_resource::resource::nodes, node_id)
                .changed(service::live_resource::resource::clusters, cluster_id);
            origin.add_changes(notification);
            co_await service::live_resource::notify(c, notification);
            result.set<"id">(node_id);
            result.set<"node_id">(agent_id);
            result.set<"secret">(secret.view());
            result.set<"revision">(1);
            if (const auto operation_id = origin.operation_id()) {
                result.set<"operation_id">(*operation_id);
            }
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error, "uk_node_name") ||
                service::common::is_unique_constraint_violation(error, "uq_node_endpoint_claim")) {
                service::common::throw_app_error(node_error::exists);
            }
            throw;
        }
        co_return result;
    }

    ruvia::task<std::optional<std::string>> update(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision,
        const ruvia::validated_json<node_save_input>& config) {
        const auto normalized = normalize(config.value());
        if (!normalized) {
            throw_corrupt_node_config();
        }
        const auto& cluster_id = normalized->cluster_id_;
        const auto& name = normalized->name_;
        const auto& status = normalized->status_;
        const auto& stored_config = normalized->config_;
        const auto config_json = serialize_node_config(c, stored_config);

        try {
            auto transaction = co_await c.db().begin_transaction();
            co_await require_cluster(transaction, tenant_id, cluster_id, stored_config);
            const auto current_rows = co_await transaction.query(
                "SELECT cluster_id, name, status, config::text FROM sys_node WHERE id = $1 AND tenant_id = $2 AND "
                "deleted_at IS NULL LIMIT 1 FOR UPDATE",
                id, tenant_id);
            if (current_rows.empty()) {
                service::common::throw_app_error(node_error::not_found);
            }
            const auto previous_cluster_id = std::string(current_rows.front()[0].value().value_or(""));
            std::vector<std::string_view> sections;
            if (current_rows.front()[1].value().value_or("") != name) {
                service::operation::add_section(sections, "basic");
            }
            if (previous_cluster_id != cluster_id) {
                service::operation::add_section(sections, "cluster");
            }
            if (!(co_await service::operation::changed_keys(transaction, current_rows.front()[3].value().value_or("{}"),
                      config_json))
                    .empty()) {
                service::operation::add_section(sections, "endpoints");
            }
            if (current_rows.front()[2].value().value_or("") != status) {
                service::operation::add_section(sections, "status");
            }
            auto origin = service::operation::work_origin::start({.action_ = service::operation::action::node_update,
                .actor_id_ = service::middleware::current_principal(c).admin_id_,
                .target_type_ = "node",
                .target_id_ = id,
                .summary_ = service::operation::sections_summary(sections)});
            (void)co_await origin.require_operation(transaction, tenant_id);
            co_await service::node_dispatch::ensure_cluster_release(transaction, tenant_id, cluster_id, origin);
            const auto rows = co_await transaction.query(
                "UPDATE sys_node SET cluster_id = $1, name = $2, status = $3, config = "
                "$4::jsonb, revision = revision + 1, node_spec_revision = node_spec_revision + 1, "
                "node_spec_digest = NULL, desired_release_id = (SELECT current_release_id FROM "
                "sys_cluster WHERE tenant_id = $6 AND id = $1), "
                "updated_at = NOW() WHERE id = $5 AND tenant_id = $6 AND revision = $7 AND "
                "deleted_at IS "
                "NULL RETURNING revision, node_spec_revision",
                service::common::db_params(
                    ruvia::db_value{cluster_id}, ruvia::db_value{name}, ruvia::db_value{status},
                    ruvia::db_value{std::string_view(config_json)}, ruvia::db_value{id},
                    ruvia::db_value{tenant_id}, ruvia::db_value{expected_revision}));
            if (rows.empty()) {
                const auto current = co_await transaction.query(
                    "SELECT revision FROM sys_node WHERE id = $1 AND tenant_id = $2 AND "
                    "deleted_at IS NULL",
                    id, tenant_id);
                if (current.empty()) {
                    service::common::throw_app_error(node_error::not_found);
                }
                service::common::throw_app_error(node_error::revision_conflict);
            }
            // The node applies its new configuration: 「节点配置下发」.
            co_await service::node_dispatch::track_node_config(transaction, tenant_id, id,
                rows.front()[1].as<std::int64_t>().value_or(1), origin);
            co_await replace_endpoint_claims(transaction, tenant_id, id, stored_config.endpoints_);
            co_await service::cluster_dns::reconcile_cluster(transaction, tenant_id, cluster_id, origin);
            if (previous_cluster_id != cluster_id) {
                co_await service::node_dispatch::exclude_pending_node_cluster_targets(transaction, tenant_id, id,
                    previous_cluster_id, origin);
                co_await service::cluster_dns::reconcile_cluster(transaction, tenant_id, previous_cluster_id, origin);
            }
            if (status == "disabled") {
                co_await service::node_dispatch::exclude_pending_node_targets(transaction, tenant_id, id, origin);
            }
            co_await transaction.commit();
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config()
                .changed(service::live_resource::resource::nodes, id)
                .changed(service::live_resource::resource::clusters, cluster_id);
            if (previous_cluster_id != cluster_id) {
                notification.changed(service::live_resource::resource::clusters, previous_cluster_id);
            }
            origin.add_changes(notification);
            co_await service::live_resource::notify(c, notification);
            co_return origin.operation_id();
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error, "uk_node_name") ||
                service::common::is_unique_constraint_violation(error, "uq_node_endpoint_claim")) {
                service::common::throw_app_error(node_error::exists);
            }
            throw;
        }
    }

    ruvia::task<std::optional<std::string>> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto origin = start(c, service::operation::action::node_delete, id);
        auto transaction = co_await c.db().begin_transaction();
        const auto rows = co_await transaction.query(
            "UPDATE sys_node SET deleted_at = NOW(), revision = revision + 1, "
            "registration_status = 'pending', node_secret_hash = NULL, "
            "node_secret_envelope = NULL, agent_id = NULL, "
            "registered_at = NULL, last_heartbeat_at = NULL, updated_at = NOW() WHERE id = $1 "
            "AND tenant_id = $2 AND revision = $3 AND deleted_at IS NULL RETURNING cluster_id",
            id, tenant_id, expected_revision);
        if (rows.empty()) {
            const auto current = co_await transaction.query(
                "SELECT revision FROM sys_node WHERE id = $1 AND tenant_id = $2 AND deleted_at "
                "IS NULL",
                id, tenant_id);
            if (current.empty()) {
                service::common::throw_app_error(node_error::not_found);
            }
            service::common::throw_app_error(node_error::revision_conflict);
        }
        const auto cluster_id = std::string(rows.front()[0].value().value_or(""));
        (void)co_await origin.require_operation(transaction, tenant_id);
        co_await clear_endpoint_claims(transaction, tenant_id, id);
        co_await service::cluster_dns::reconcile_cluster(transaction, tenant_id, cluster_id, origin);
        co_await service::node_dispatch::exclude_pending_node_targets(transaction, tenant_id, id, origin);
        co_await service::node_dispatch::cancel_node_config(transaction, tenant_id, id, origin);
        co_await transaction.commit();
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.node_config()
            .changed(service::live_resource::resource::nodes, id)
            .changed(service::live_resource::resource::clusters, cluster_id);
        origin.add_changes(notification);
        co_await service::live_resource::notify(c, notification);
        co_return origin.operation_id();
    }

    ruvia::task<node_credentials_dto> reset_credentials(ruvia::context& c, const std::string& tenant_id,
        const std::string& id,
        std::int64_t expected_revision) {
        auto origin = start(c, service::operation::action::node_reset_credentials, id);
        auto transaction = co_await c.db().begin_transaction();
        const auto current_rows = co_await transaction.query(
            "SELECT revision, cluster_id FROM sys_node WHERE id = $1 AND tenant_id = $2 AND "
            "deleted_at IS NULL LIMIT 1",
            id, tenant_id);
        if (current_rows.empty()) {
            service::common::throw_app_error(node_error::not_found);
        }
        if (current_rows.front()[0].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(node_error::revision_conflict);
        }
        const auto current_cluster_id = current_rows.front()[1].value().value_or("");
        (void)co_await origin.require_operation(transaction, tenant_id);
        co_await service::node_dispatch::ensure_cluster_release(transaction, tenant_id, current_cluster_id, origin);
        const auto agent_id = service::utils::random_token().substr(0, 32);
        service::utils::sensitive_string secret(service::utils::random_token());
        const auto hash = service::utils::token_hash(secret.view());
        const auto envelope = service::utils::seal_secret(secret.view());
        const auto rows = co_await transaction.query(
            "UPDATE sys_node SET agent_id = $3, node_secret_hash = $4, node_secret_envelope = $5, "
            "registration_status = 'pending', registered_at = NULL, last_heartbeat_at = NULL, "
            "applied_node_spec_revision = 0, node_spec_digest = NULL, desired_release_id = "
            "(SELECT current_release_id FROM sys_cluster WHERE tenant_id = $2 AND id = "
            "sys_node.cluster_id), active_release_id = NULL, active_manifest_digest = NULL, "
            "last_apply_phase = NULL, last_apply_error_code = NULL, last_apply_error = NULL, "
            "last_apply_retryable = NULL, "
            "runtime = '{}'::jsonb, revision = "
            "revision + 1, updated_at = "
            "NOW() WHERE id = $1 AND "
            "tenant_id = $2 AND revision = $6 AND deleted_at IS NULL RETURNING revision, "
            "cluster_id",
            id, tenant_id, agent_id, std::string_view(hash), std::string_view(envelope),
            expected_revision);
        if (rows.empty()) {
            const auto current = co_await transaction.query(
                "SELECT revision FROM sys_node WHERE id = $1 AND tenant_id = $2 AND deleted_at "
                "IS NULL",
                id, tenant_id);
            if (current.empty()) {
                service::common::throw_app_error(node_error::not_found);
            }
            service::common::throw_app_error(node_error::revision_conflict);
        }
        const auto cluster_id = std::string(rows.front()[1].value().value_or(""));
        co_await service::cluster_dns::reconcile_cluster(transaction, tenant_id, cluster_id, origin);
        co_await service::node_dispatch::exclude_pending_node_targets(transaction, tenant_id, id, origin);
        co_await service::node_dispatch::cancel_node_config(transaction, tenant_id, id, origin);
        co_await transaction.commit();
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.node_config()
            .changed(service::live_resource::resource::nodes, id)
            .changed(service::live_resource::resource::clusters, cluster_id);
        origin.add_changes(notification);
        co_await service::live_resource::notify(c, notification);
        node_credentials_dto result(ruvia::model_options{.resource_ = c.pool()});
        result.set<"id">(id);
        result.set<"node_id">(agent_id);
        result.set<"secret">(secret.view());
        result.set<"revision">(rows.front()[0].as<std::int64_t>().value_or(expected_revision + 1));
        if (const auto operation_id = origin.operation_id()) {
            result.set<"operation_id">(*operation_id);
        }
        co_return result;
    }

private:
    static service::operation::work_origin start(ruvia::context& c, service::operation::action action,
        const std::string& id) {
        return service::operation::work_origin::start({.action_ = action,
            .actor_id_ = service::middleware::current_principal(c).admin_id_,
            .target_type_ = "node",
            .target_id_ = id,
            .summary_ = "{}"});
    }

    static ruvia::task<void> require_cluster(ruvia::db_transaction& transaction,
        const std::string& tenant_id,
        const std::string& cluster_id,
        const service::node_config::node_config_data& config) {
        const auto rows = co_await transaction.query(
            "SELECT zone.runtime::text FROM sys_cluster cluster INNER JOIN sys_dns_zone zone ON "
            "zone.tenant_id = cluster.tenant_id AND zone.id = cluster.dns_zone_id WHERE "
            "cluster.id = $1 AND cluster.tenant_id = $2 AND cluster.status = 'enabled' AND "
            "cluster.deleted_at IS NULL AND zone.deleted_at IS NULL LIMIT 1 FOR SHARE OF cluster, "
            "zone",
            cluster_id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(node_error::cluster_unavailable);
        }
        const auto runtime =
            service::dns_sync::parse_stored_runtime(rows.front()[0].value().value_or("{}"));
        if (!runtime) {
            throw_corrupt_node_config();
        }
        for (const auto& endpoint : config.endpoints_) {
            const auto& line_code = endpoint.line_code_;
            const auto line = std::ranges::find_if(
                runtime->lines_, [&](const service::dns_sync::zone_line_runtime_data& item) {
                    return service::dns_sync::is_enabled_line(item) && *item.code_ == line_code;
                });
            if (line == runtime->lines_.end()) {
                service::common::throw_app_error(node_error::dns_line_invalid);
            }
        }
        co_return;
    }
};

inline node_command_service& get_node_command_service() {
    static node_command_service service;
    return service;
}

}  // namespace service::node
