#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/cluster/cluster_error.h"
#include "service/domains/cluster/cluster_types.h"
#include "service/domains/dns_zone/dns_zone_reference.h"
#include "service/domains/node/node_reference.h"
#include "service/domains/provider/provider_reference.h"
#include "service/domains/website/website_reference.h"
#include "service/features/background/queued_work.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/features/website_dns/projection.h"
#include "service/utils/text.h"

namespace service::cluster {

class cluster_command_service final {
public:
    ruvia::task<std::int64_t> create(ruvia::context& c, const std::string& tenant_id,
        const save_cluster_body& body) {
        const auto normalized = normalize(body);
        if (!normalized) {
            service::common::throw_app_error(service::common::validation_error_code,
                "集群配置不能为空", 400);
        }
        const auto name = service::utils::trim_ascii(normalized->name_);
        const auto& dns_zone_id = normalized->dns_zone_id_;
        const auto hostname_prefix = service::utils::to_lower_ascii(normalized->hostname_prefix_);
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
                "NOW(), NOW()) RETURNING id, revision",
                service::common::db_params(ruvia::db_value{tenant_id}, ruvia::db_value{dns_zone_id},
                    ruvia::db_value{name},
                    ruvia::db_value{std::string_view(hostname_prefix)},
                    ruvia::db_value{status}));
            service::background::queued_work work;
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id,
                rows.front()[0].value().value_or(""), work);
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config()
                .changed(service::live_resource::resource::clusters, rows.front()[0].value().value_or(""))
                .changed(service::live_resource::resource::nodes);
            co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
            co_return rows.front()[1].as<std::int64_t>().value_or(1);
        } catch (const ruvia::db_error& error) {
            if (is_identity_conflict(error)) {
                service::common::throw_app_error(cluster_error::exists);
            }
            throw;
        }
    }

    ruvia::task<std::int64_t> update(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision, const save_cluster_body& body) {
        const auto normalized = normalize(body);
        if (!normalized) {
            service::common::throw_app_error(service::common::validation_error_code,
                "集群配置不能为空", 400);
        }
        const auto name = service::utils::trim_ascii(normalized->name_);
        const auto& dns_zone_id = normalized->dns_zone_id_;
        const auto hostname_prefix = service::utils::to_lower_ascii(normalized->hostname_prefix_);
        const auto& status = normalized->status_;
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto zone_domain = co_await require_zone(transaction, tenant_id, dns_zone_id);
            if ((hostname_prefix + "." + zone_domain).size() > 253) {
                service::common::throw_app_error(cluster_error::dns_zone_unavailable);
            }
            const auto rows = co_await transaction.query(
                "WITH current AS (SELECT id, dns_zone_id AS previous_dns_zone_id FROM "
                "sys_cluster WHERE id = $5 AND tenant_id = $6 AND revision = $7 AND "
                "deleted_at IS NULL FOR UPDATE) UPDATE sys_cluster cluster SET dns_zone_id = "
                "$1, name = $2, hostname_prefix = $3, status = $4, revision = cluster.revision + "
                "1, updated_at = NOW() FROM current WHERE cluster.id = current.id RETURNING "
                "current.previous_dns_zone_id, cluster.dns_zone_id, cluster.revision",
                service::common::db_params(
                    ruvia::db_value{dns_zone_id}, ruvia::db_value{name},
                    ruvia::db_value{std::string_view(hostname_prefix)}, ruvia::db_value{status},
                    ruvia::db_value{id}, ruvia::db_value{tenant_id},
                    ruvia::db_value{expected_revision}));
            co_await service::common::require_revision_written(transaction, !rows.empty(), "sys_cluster", tenant_id, id,
                revision_errors_);
            service::background::queued_work work;
            const auto previous_zone_id = rows.front()[0].value().value_or("");
            const auto next_zone_id = rows.front()[1].value().value_or("");
            co_await service::dns_sync::mark_zone_dirty(transaction, tenant_id, previous_zone_id, work);
            if (next_zone_id != previous_zone_id) {
                co_await service::dns_sync::mark_zone_dirty(transaction, tenant_id, next_zone_id, work);
            }
            co_await service::website_dns::reconcile_cluster_consumers(transaction, tenant_id, id, work);
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, id, work);
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config()
                .changed(service::live_resource::resource::clusters, id)
                .changed(service::live_resource::resource::nodes);
            co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
            co_return rows.front()[2].as<std::int64_t>().value_or(0);
        } catch (const ruvia::db_error& error) {
            if (is_identity_conflict(error)) {
                service::common::throw_app_error(cluster_error::exists);
            }
            throw;
        }
    }

    ruvia::task<std::int64_t> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto transaction = co_await c.db().begin_transaction();
        const auto clusters = co_await transaction.query(
            "SELECT revision FROM sys_cluster WHERE id = $1 AND tenant_id = $2 AND deleted_at IS NULL "
            "LIMIT 1 FOR UPDATE",
            id, tenant_id);
        service::common::require_current_revision(clusters, expected_revision, revision_errors_);
        if (co_await service::node::nodes_use_cluster(transaction, tenant_id, id)) {
            service::common::throw_app_error(cluster_error::has_nodes);
        }
        const auto websites = co_await service::website::websites_use_cluster(transaction, tenant_id, id);
        if (websites.live_) {
            service::common::throw_app_error(cluster_error::has_websites);
        }
        if (websites.cleanup_active_) {
            service::common::throw_app_error(cluster_error::website_cleanup_active);
        }
        const auto removed = co_await transaction.query(
            "UPDATE sys_cluster SET revision = revision + 1, deleted_at = NOW(), updated_at = "
            "NOW() WHERE id = $1 AND tenant_id = $2 RETURNING revision",
            id, tenant_id);
        service::background::queued_work work;
        co_await service::dns_sync::mark_cluster_zone_dirty(transaction, tenant_id, id, work);
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.node_config()
            .changed(service::live_resource::resource::clusters, id)
            .changed(service::live_resource::resource::nodes);
        co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
        co_return removed.front()[0].as<std::int64_t>().value_or(0);
    }

private:
    static constexpr service::common::revision_errors revision_errors_{cluster_error::not_found,
        cluster_error::revision_conflict};

    // Locks the zone and its DNS provider; returns the zone domain.
    static ruvia::task<std::string> require_zone(ruvia::db_transaction& transaction,
        const std::string& tenant_id,
        const std::string& dns_zone_id) {
        auto zone = co_await service::dns_zone::lock_dns_zone(transaction, tenant_id, dns_zone_id);
        if (!zone || !(co_await service::provider::lock_dns_provider(transaction, tenant_id, zone->provider_id_))) {
            service::common::throw_app_error(cluster_error::dns_zone_unavailable);
        }
        co_return std::move(zone->domain_);
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
