#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/db/db_transaction.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/cluster/cluster_error.h"
#include "service/domains/cluster/cluster_types.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/website_dns/projection.h"

namespace service::cluster {

class cluster_command_service final {
public:
    ruvia::task<std::optional<std::string>> create(ruvia::context& c, const std::string& tenant_id,
        const save_cluster_body& body) {
        const auto normalized = normalize(body);
        if (!normalized) {
            service::common::throw_app_error(service::common::validation_error_code,
                "集群配置不能为空", 400);
        }
        const auto name = trim(normalized->name_);
        const auto& dns_zone_id = normalized->dns_zone_id_;
        const auto hostname_prefix = normalize_prefix(normalized->hostname_prefix_);
        const auto& status = normalized->status_;
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto zone_domain = co_await require_zone(transaction, tenant_id, dns_zone_id);
            if ((hostname_prefix + "." + zone_domain).size() > 253) {
                service::common::throw_app_error(cluster_error::dns_zone_unavailable);
            }
            const auto rows = co_await transaction.query(
                "INSERT INTO sys_cluster (tenant_id, dns_zone_id, name, hostname_prefix, "
                "status, revision, created_at, updated_at) VALUES ($1, $2, $3, $4, $5, 1, "
                "NOW(), NOW()) RETURNING id",
                service::common::db_params(ruvia::db_value{tenant_id}, ruvia::db_value{dns_zone_id},
                    ruvia::db_value{std::string_view(name)},
                    ruvia::db_value{std::string_view(hostname_prefix)},
                    ruvia::db_value{status}));
            // The user operation is retained even though a new cluster's
            // first release has no node targets.
            auto origin = start(c, service::operation::action::cluster_create,
                std::string(rows.front()[0].value().value_or("")));
            (void)co_await origin.require_operation(transaction, tenant_id);
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id,
                rows.front()[0].value().value_or(""), origin);
            co_await transaction.commit();
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config()
                .changed(service::live_resource::resource::clusters, rows.front()[0].value().value_or(""))
                .changed(service::live_resource::resource::nodes);
            origin.add_changes(notification);
            co_await service::live_resource::notify(c, notification);
            co_return origin.operation_id();
        } catch (const ruvia::db_error& error) {
            if (is_identity_conflict(error)) {
                service::common::throw_app_error(cluster_error::exists);
            }
            throw;
        }
    }

    ruvia::task<std::optional<std::string>> update(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision, const save_cluster_body& body) {
        const auto normalized = normalize(body);
        if (!normalized) {
            service::common::throw_app_error(service::common::validation_error_code,
                "集群配置不能为空", 400);
        }
        const auto name = trim(normalized->name_);
        const auto& dns_zone_id = normalized->dns_zone_id_;
        const auto hostname_prefix = normalize_prefix(normalized->hostname_prefix_);
        const auto& status = normalized->status_;
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto zone_domain = co_await require_zone(transaction, tenant_id, dns_zone_id);
            if ((hostname_prefix + "." + zone_domain).size() > 253) {
                service::common::throw_app_error(cluster_error::dns_zone_unavailable);
            }
            const auto rows = co_await transaction.query(
                "WITH current AS (SELECT id, dns_zone_id AS previous_dns_zone_id, name AS previous_name, "
                "hostname_prefix AS previous_prefix, status AS previous_status FROM "
                "sys_cluster WHERE id = $5 AND tenant_id = $6 AND revision = $7 AND "
                "deleted_at IS NULL FOR UPDATE) UPDATE sys_cluster cluster SET dns_zone_id = "
                "$1, name = $2, hostname_prefix = $3, status = $4, revision = cluster.revision + "
                "1, updated_at = NOW() FROM current WHERE cluster.id = current.id RETURNING "
                "current.previous_dns_zone_id, cluster.dns_zone_id, current.previous_name <> cluster.name, "
                "current.previous_dns_zone_id <> cluster.dns_zone_id OR current.previous_prefix <> "
                "cluster.hostname_prefix, current.previous_status <> cluster.status",
                service::common::db_params(
                    ruvia::db_value{dns_zone_id}, ruvia::db_value{std::string_view(name)},
                    ruvia::db_value{std::string_view(hostname_prefix)}, ruvia::db_value{status},
                    ruvia::db_value{id}, ruvia::db_value{tenant_id},
                    ruvia::db_value{expected_revision}));
            if (rows.empty()) {
                const auto current = co_await transaction.query(
                    "SELECT revision FROM sys_cluster WHERE id = $1 AND tenant_id = $2 AND "
                    "deleted_at IS NULL",
                    id, tenant_id);
                if (current.empty()) {
                    service::common::throw_app_error(cluster_error::not_found);
                }
                service::common::throw_app_error(cluster_error::revision_conflict);
            }
            std::vector<std::string_view> sections;
            if (rows.front()[2].as<bool>().value_or(false)) {
                service::operation::add_section(sections, "basic");
            }
            if (rows.front()[3].as<bool>().value_or(false)) {
                service::operation::add_section(sections, "dns");
            }
            if (rows.front()[4].as<bool>().value_or(false)) {
                service::operation::add_section(sections, "status");
            }
            auto origin = start(c, service::operation::action::cluster_update, id,
                service::operation::sections_summary(sections));
            (void)co_await origin.require_operation(transaction, tenant_id);
            co_await service::cluster_dns::reconcile_zone_transition(transaction, tenant_id,
                std::string(rows.front()[0].value().value_or("")), std::string(rows.front()[1].value().value_or("")),
                origin);
            co_await service::website_dns::reconcile_cluster_consumers(transaction, tenant_id, id, origin);
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, id, origin);
            co_await transaction.commit();
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config()
                .changed(service::live_resource::resource::clusters, id)
                .changed(service::live_resource::resource::nodes);
            origin.add_changes(notification);
            co_await service::live_resource::notify(c, notification);
            co_return origin.operation_id();
        } catch (const ruvia::db_error& error) {
            if (is_identity_conflict(error)) {
                service::common::throw_app_error(cluster_error::exists);
            }
            throw;
        }
    }

    ruvia::task<std::optional<std::string>> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto origin = start(c, service::operation::action::cluster_delete, id);
        auto transaction = co_await c.db().begin_transaction();
        const auto clusters = co_await transaction.query(
            "SELECT cluster.revision, EXISTS(SELECT 1 FROM sys_node node WHERE node.tenant_id = "
            "cluster.tenant_id AND node.cluster_id = cluster.id AND node.deleted_at IS NULL), "
            "EXISTS(SELECT 1 FROM sys_website website WHERE website.tenant_id = "
            "cluster.tenant_id AND website.cluster_id = cluster.id AND website.deleted_at IS "
            "NULL), EXISTS(SELECT 1 FROM sys_website website INNER JOIN sys_sync_task task ON "
            "task.tenant_id = website.tenant_id AND task.resource_type = 'website' AND "
            "task.resource_id = website.id AND task.operation = 'delete' AND NOT task.is_done "
            "WHERE "
            "website.tenant_id = cluster.tenant_id AND website.cluster_id = cluster.id AND "
            "website.deleted_at IS NOT NULL) FROM "
            "sys_cluster cluster WHERE cluster.id = $1 AND cluster.tenant_id = $2 AND "
            "cluster.deleted_at IS NULL LIMIT 1 FOR UPDATE OF cluster",
            service::common::db_params(ruvia::db_value{id}, ruvia::db_value{tenant_id}));
        if (clusters.empty()) {
            service::common::throw_app_error(cluster_error::not_found);
        }
        if (clusters.front()[0].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(cluster_error::revision_conflict);
        }
        if (clusters.front()[1].as<bool>().value_or(false)) {
            service::common::throw_app_error(cluster_error::has_nodes);
        }
        if (clusters.front()[2].as<bool>().value_or(false)) {
            service::common::throw_app_error(cluster_error::has_websites);
        }
        if (clusters.front()[3].as<bool>().value_or(false)) {
            service::common::throw_app_error(cluster_error::website_cleanup_active);
        }
        (void)co_await origin.require_operation(transaction, tenant_id);
        const auto rows = co_await transaction.query(
            "UPDATE sys_cluster SET revision = revision + 1, deleted_at = NOW(), updated_at = "
            "NOW() WHERE id = $1 AND tenant_id = $2 AND revision = $3 AND deleted_at IS NULL "
            "RETURNING id",
            service::common::db_params(ruvia::db_value{id}, ruvia::db_value{tenant_id},
                ruvia::db_value{expected_revision}));
        if (rows.empty()) {
            service::common::throw_app_error(cluster_error::not_found);
        }
        co_await service::cluster_dns::reconcile_cluster(transaction, tenant_id, id, origin);
        co_await transaction.commit();
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.node_config()
            .changed(service::live_resource::resource::clusters, id)
            .changed(service::live_resource::resource::nodes);
        origin.add_changes(notification);
        co_await service::live_resource::notify(c, notification);
        co_return origin.operation_id();
    }

private:
    static service::operation::work_origin start(ruvia::context& c, service::operation::action action,
        std::string id, std::string summary = "{}") {
        return service::operation::work_origin::start({.action_ = action,
            .actor_id_ = service::middleware::current_principal(c).admin_id_,
            .target_type_ = "cluster",
            .target_id_ = std::move(id),
            .summary_ = std::move(summary)});
    }

    static ruvia::task<std::string> require_zone(ruvia::db_transaction& transaction,
        const std::string& tenant_id,
        const std::string& dns_zone_id) {
        const auto rows = co_await transaction.query(
            "SELECT zone.domain FROM sys_dns_zone zone INNER JOIN sys_provider provider ON "
            "provider.tenant_id = zone.tenant_id AND provider.id = zone.provider_id AND "
            "provider.kind = 'dns' WHERE zone.id = $1 AND zone.tenant_id = $2 AND "
            "zone.deleted_at IS NULL AND provider.deleted_at IS NULL LIMIT 1 FOR SHARE OF zone, "
            "provider",
            service::common::db_params(ruvia::db_value{dns_zone_id}, ruvia::db_value{tenant_id}));
        if (rows.empty()) {
            service::common::throw_app_error(cluster_error::dns_zone_unavailable);
        }
        co_return std::string(rows.front()[0].value().value_or(""));
    }

    static std::string trim(std::string_view input) {
        const auto begin = std::find_if_not(input.begin(), input.end(),
            [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }).base();
        return begin < end ? std::string(begin, end) : std::string{};
    }

    static std::string normalize_prefix(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    static bool is_identity_conflict(const ruvia::db_error& error) {
        return service::common::is_unique_constraint_violation(error, "uk_cluster_name") ||
               service::common::is_unique_constraint_violation(error, "uk_cluster_hostname");
    }
};

inline cluster_command_service& get_cluster_command_service() {
    static cluster_command_service service;
    return service;
}

}  // namespace service::cluster
