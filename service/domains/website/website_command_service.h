#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/model_json.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/certificate/certificate_reference.h"
#include "service/domains/cluster/cluster_reference.h"
#include "service/domains/dns_zone/dns_zone_reference.h"
#include "service/domains/website/aggregate_store.h"
#include "service/domains/website/certificate_coverage.h"
#include "service/domains/website/domain_claim.h"
#include "service/domains/website/relation_store.h"
#include "service/domains/website/website_error.h"
#include "service/domains/website/website_types.h"
#include "service/features/background/queued_work.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/features/website_config/mapper.h"
#include "service/features/website_dns/projection.h"

namespace service::website {

class website_command_service final {
public:
    // 「检查网站域名」: re-checks the domains at the current revision.
    ruvia::task<void> request_dns_probe(ruvia::context& c, const std::string& tenant_id,
        const std::string& id) {
        auto transaction = co_await c.db().begin_transaction();
        const auto revision = co_await lock_website_revision(transaction, tenant_id, id);
        if (!revision) {
            service::common::throw_app_error(website_error::not_found);
        }
        service::background::queued_work work;
        (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
            service::sync_runtime::marker_resource_type::website, id, service::sync_runtime::marker_operation::apply,
            *revision, work);
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.node_config().changed(service::live_resource::resource::websites, id);
        co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
    }

    ruvia::task<website_created_dto> create(ruvia::context& c, const std::string& tenant_id,
        const std::string& cluster_id,
        const website_save_input& body) {
        const auto normalized = normalize(body);
        if (!normalized) {
            throw_corrupt_config();
        }
        const auto& status = normalized->status_;
        const auto& config = normalized->config_;
        const auto config_json = serialize_config(c, config);
        website_created_dto result({.resource_ = c.pool()});
        try {
            auto transaction = co_await c.db().begin_transaction();
            co_await require_cluster(transaction, tenant_id, cluster_id);
            const auto domain_claims =
                co_await build_domain_claims(transaction, tenant_id, std::nullopt, config);
            co_await validate_certificates(transaction, tenant_id, config);
            const auto website_id = co_await insert_website(transaction, tenant_id, cluster_id, status, config_json);
            service::background::queued_work work;
            co_await replace_relation_projections(transaction, tenant_id, website_id, domain_claims,
                config.certificate_ids_);
            co_await service::website_dns::reconcile_claim_change(transaction, tenant_id, website_id, {}, work);
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, cluster_id, work);
            (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
                service::sync_runtime::marker_resource_type::website, website_id,
                service::sync_runtime::marker_operation::apply, 1, work);
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config().changed(service::live_resource::resource::websites, website_id);
            co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
            result.set<"id">(website_id);
            result.set<"revision">(1);
        } catch (const ruvia::db_error& error) {
            if (is_domain_claim_conflict(error)) {
                service::common::throw_app_error(website_error::domain_exists);
            }
            throw;
        }
        co_return result;
    }

    ruvia::task<std::int64_t> update(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, const std::string& cluster_id, std::int64_t expected_revision,
        const website_save_input& body) {
        const auto normalized = normalize(body);
        if (!normalized) {
            throw_corrupt_config();
        }
        const auto& status = normalized->status_;
        const auto& config = normalized->config_;
        const auto config_json = serialize_config(c, config);
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto previous = co_await lock_website_snapshot(transaction, tenant_id, id);
            service::common::require_current_revision(
                previous ? std::optional<std::int64_t>{previous->revision_} : std::nullopt, expected_revision,
                revision_errors_);
            co_await require_cluster(transaction, tenant_id, cluster_id);
            const auto domain_claims = co_await build_domain_claims(transaction, tenant_id, id, config);
            co_await validate_certificates(transaction, tenant_id, config);
            const auto& previous_cluster_id = previous->cluster_id_;
            const auto revision = co_await update_website(transaction, tenant_id, id, expected_revision,
                cluster_id, status, config_json);
            service::background::queued_work work;
            const auto previous_zone_ids = co_await service::website_dns::managed_zone_ids(transaction, tenant_id, id);
            co_await replace_relation_projections(transaction, tenant_id, id, domain_claims, config.certificate_ids_);
            co_await service::website_dns::reconcile_claim_change(transaction, tenant_id, id, previous_zone_ids, work);
            if (previous_cluster_id != cluster_id) {
                co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, previous_cluster_id,
                    work);
            }
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, cluster_id, work);
            (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
                service::sync_runtime::marker_resource_type::website, id,
                service::sync_runtime::marker_operation::apply, *revision, work);
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config().changed(service::live_resource::resource::websites, id);
            co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
            co_return *revision;
        } catch (const ruvia::db_error& error) {
            if (is_domain_claim_conflict(error)) {
                service::common::throw_app_error(website_error::domain_exists);
            }
            throw;
        }
    }

    ruvia::task<std::int64_t> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto transaction = co_await c.db().begin_transaction();
        const auto previous = co_await soft_delete_website(transaction, tenant_id, id, expected_revision);
        co_await service::common::require_revision_written(transaction, previous.has_value(), "sys_website",
            tenant_id, id, revision_errors_);
        const auto& cluster_id = previous->cluster_id_;
        service::background::queued_work work;
        const auto previous_zone_ids = co_await service::website_dns::managed_zone_ids(transaction, tenant_id, id);
        co_await clear_relation_projections(transaction, tenant_id, id);
        co_await service::website_dns::reconcile_claim_change(transaction, tenant_id, id, previous_zone_ids, work);
        co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, cluster_id, work);
        co_await service::sync_runtime::remove_marker(transaction, tenant_id,
            service::sync_runtime::marker_resource_type::website, id, work);
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.node_config()
            .changed(service::live_resource::resource::websites, id)
            .changed(service::live_resource::resource::access_history, id);
        co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
        co_return previous->revision_;
    }

private:
    static constexpr service::common::revision_errors revision_errors_{website_error::not_found,
        website_error::revision_conflict};

    static std::string serialize_config(ruvia::context& c,
        const service::website_config::website_config_data& input) {
        const auto output = service::website_config::to_output(input, {.resource_ = c.pool()});
        const auto json = ruvia::to_json(output, {.resource_ = c.pool()});
        return std::string(json.data(), json.size());
    }

    static ruvia::task<void> require_cluster(ruvia::db_transaction& db, const std::string& tenant_id,
        const std::string& cluster_id) {
        if (!(co_await service::cluster::lock_enabled_cluster(db, tenant_id, cluster_id))) {
            service::common::throw_app_error(website_error::cluster_unavailable);
        }
        co_return;
    }

    static ruvia::task<std::vector<domain_claim>>
    build_domain_claims(ruvia::db_transaction& db, const std::string& tenant_id,
        const std::optional<std::string>& excluded_website_id,
        const service::website_config::website_config_data& config) {
        const auto zones = co_await service::dns_zone::lock_live_dns_zones(db, tenant_id);
        auto planned = plan_domain_claims(config.domains_, zones);
        if (const auto* error = std::get_if<domain_claim_error>(&planned)) {
            switch (*error) {
                case domain_claim_error::managed_zone_not_found:
                    service::common::throw_app_error(website_error::managed_zone_not_found);
            }
        }
        auto claims = std::get<std::vector<domain_claim>>(std::move(planned));
        if (co_await has_conflicting_domain_claims(db, tenant_id, claims, excluded_website_id)) {
            service::common::throw_app_error(website_error::domain_exists);
        }
        co_return claims;
    }

    static ruvia::task<void>
    validate_certificates(ruvia::db_transaction& db, const std::string& tenant_id,
        const service::website_config::website_config_data& config) {
        const auto https_enabled = config.https_enabled_;
        const auto& certificate_ids = config.certificate_ids_;
        if ((https_enabled && certificate_ids.empty()) ||
            (!https_enabled && !certificate_ids.empty())) {
            service::common::throw_app_error(website_error::https_certificate_selection_invalid);
        }
        if (certificate_ids.empty()) {
            co_return;
        }

        const auto certificates = co_await service::certificate::lock_usable_certificates(db, tenant_id, certificate_ids);
        if (!certificates) {
            service::common::throw_app_error(website_error::certificate_unavailable);
        }
        if (find_unmatched_certificate(*certificates, config.domains_)) {
            service::common::throw_app_error(website_error::certificate_domain_mismatch);
        }
        co_return;
    }

    [[noreturn]] static void throw_corrupt_config() {
        service::common::throw_app_error(service::common::server_error_code, "聚合配置损坏", 500);
    }
};

inline website_command_service& get_website_command_service() {
    static website_command_service service;
    return service;
}

}  // namespace service::website
