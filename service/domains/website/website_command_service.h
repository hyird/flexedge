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

#include "service/common/http.h"
#include "service/domains/website/aggregate_store.h"
#include "service/domains/website/certificate_coverage.h"
#include "service/domains/website/domain_claim.h"
#include "service/domains/website/reference_store.h"
#include "service/domains/website/relation_store.h"
#include "service/domains/website/website_error.h"
#include "service/domains/website/website_types.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_config/mapper.h"
#include "service/features/website_dns/projection.h"

namespace service::website {

class website_command_service final {
public:
    // 「检查网站域名」: re-checks the domains at the current revision.
    ruvia::task<std::optional<std::string>> request_dns_probe(ruvia::context& c, const std::string& tenant_id,
        const std::string& id) {
        auto origin = start(c, service::operation::action::website_check_domains, id);
        auto transaction = co_await c.db().begin_transaction();
        const auto revision = co_await lock_website_revision(transaction, tenant_id, id);
        if (!revision) {
            service::common::throw_app_error(website_error::not_found);
        }
        (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
            service::sync_runtime::marker_resource_type::website, id, service::sync_runtime::marker_operation::apply,
            *revision, origin);
        co_await transaction.commit();
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.node_config().changed(service::live_resource::resource::websites, id);
        origin.add_changes(notification);
        co_await service::live_resource::notify(c, notification);
        co_return origin.operation_id();
    }

    ruvia::task<website_created_dto> create(ruvia::context& c, const std::string& tenant_id,
        const std::string& cluster_id,
        const ruvia::validated_json<website_save_input>& body) {
        const auto normalized = normalize(body.value());
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
            auto origin = start(c, service::operation::action::website_create, website_id);
            co_await replace_relation_projections(transaction, tenant_id, website_id, domain_claims,
                config.certificate_ids_);
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, cluster_id, origin);
            (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
                service::sync_runtime::marker_resource_type::website, website_id,
                service::sync_runtime::marker_operation::apply, 1, origin);
            co_await service::website_dns::reconcile_config_change(transaction, tenant_id, std::nullopt, config_json,
                origin);
            co_await transaction.commit();
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config().changed(service::live_resource::resource::websites, website_id);
            origin.add_changes(notification);
            co_await service::live_resource::notify(c, notification);
            result.set<"id">(website_id);
            result.set<"revision">(1);
            if (const auto operation_id = origin.operation_id()) {
                result.set<"operation_id">(*operation_id);
            }
        } catch (const ruvia::db_error& error) {
            if (is_domain_claim_conflict(error)) {
                service::common::throw_app_error(website_error::domain_exists);
            }
            throw;
        }
        co_return result;
    }

    ruvia::task<std::optional<std::string>> update(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, const std::string& cluster_id, std::int64_t expected_revision,
        const ruvia::validated_json<website_save_input>& body) {
        const auto normalized = normalize(body.value());
        if (!normalized) {
            throw_corrupt_config();
        }
        const auto& status = normalized->status_;
        const auto& config = normalized->config_;
        const auto config_json = serialize_config(c, config);
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto previous = co_await lock_website_snapshot(transaction, tenant_id, id);
            if (!previous) {
                service::common::throw_app_error(website_error::not_found);
            }
            if (previous->revision_ != expected_revision) {
                service::common::throw_app_error(website_error::revision_conflict);
            }
            co_await require_cluster(transaction, tenant_id, cluster_id);
            const auto domain_claims = co_await build_domain_claims(transaction, tenant_id, id, config);
            co_await validate_certificates(transaction, tenant_id, config);
            const auto& previous_config = previous->config_json_;
            const auto& previous_cluster_id = previous->cluster_id_;
            const auto previous_status = co_await find_website_status(transaction, tenant_id, id);
            auto sections = config_sections(
                co_await service::operation::changed_keys(transaction, previous_config, config_json));
            if (previous_cluster_id != cluster_id) {
                service::operation::add_section(sections, "cluster");
            }
            if (previous_status != status) {
                service::operation::add_section(sections, "status");
            }
            const auto action = sections.size() == 1 && sections.front() == "status"
                                    ? (status == "enabled" ? service::operation::action::website_enable
                                                           : service::operation::action::website_disable)
                                    : service::operation::action::website_update;
            auto origin = start(c, action, id, service::operation::sections_summary(sections));
            const auto revision = co_await update_website(transaction, tenant_id, id, expected_revision,
                cluster_id, status, config_json);
            if (!revision) {
                service::common::throw_app_error(website_error::revision_conflict);
            }
            co_await replace_relation_projections(transaction, tenant_id, id, domain_claims, config.certificate_ids_);
            if (previous_cluster_id != cluster_id) {
                co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, previous_cluster_id,
                    origin);
            }
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, cluster_id, origin);
            (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
                service::sync_runtime::marker_resource_type::website, id,
                service::sync_runtime::marker_operation::apply, *revision, origin);
            co_await service::website_dns::reconcile_config_change(transaction, tenant_id, previous_config,
                config_json, origin);
            co_await transaction.commit();
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config().changed(service::live_resource::resource::websites, id);
            origin.add_changes(notification);
            co_await service::live_resource::notify(c, notification);
            co_return origin.operation_id();
        } catch (const ruvia::db_error& error) {
            if (is_domain_claim_conflict(error)) {
                service::common::throw_app_error(website_error::domain_exists);
            }
            throw;
        }
    }

    ruvia::task<std::optional<std::string>> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto origin = start(c, service::operation::action::website_delete, id);
        auto transaction = co_await c.db().begin_transaction();
        const auto previous = co_await soft_delete_website(transaction, tenant_id, id, expected_revision);
        if (!previous) {
            if (!(co_await find_website_revision(transaction, tenant_id, id))) {
                service::common::throw_app_error(website_error::not_found);
            }
            service::common::throw_app_error(website_error::revision_conflict);
        }
        const auto& previous_config = previous->config_json_;
        const auto& cluster_id = previous->cluster_id_;
        co_await clear_relation_projections(transaction, tenant_id, id);
        co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id, cluster_id, origin);
        co_await service::website_dns::reconcile_config_change(transaction, tenant_id, previous_config,
            std::nullopt, origin);
        co_await service::sync_runtime::remove_marker(transaction, tenant_id,
            service::sync_runtime::marker_resource_type::website, id, origin);
        co_await transaction.commit();
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.node_config()
            .changed(service::live_resource::resource::websites, id)
            .changed(service::live_resource::resource::access_history, id);
        origin.add_changes(notification);
        co_await service::live_resource::notify(c, notification);
        co_return origin.operation_id();
    }

private:
    static service::operation::work_origin start(ruvia::context& c, service::operation::action action,
        const std::string& id, std::string summary = "{}") {
        return service::operation::work_origin::start({.action_ = action,
            .actor_id_ = service::middleware::current_principal(c).admin_id_,
            .target_type_ = "website",
            .target_id_ = id,
            .summary_ = std::move(summary)});
    }

    // Section names (docs/task-center.md) of changed top-level config keys.
    static std::vector<std::string_view> config_sections(const std::vector<std::string>& keys) {
        std::vector<std::string_view> sections;
        for (const std::string_view key : keys) {
            if (key == "name") {
                service::operation::add_section(sections, "basic");
            } else if (key == "domains") {
                service::operation::add_section(sections, "domains");
            } else if (key.starts_with("origin") || key.starts_with("default_origin") || key == "pass_client_ip") {
                service::operation::add_section(sections, "origins");
            } else if (key.starts_with("health_check") || key.ends_with("healthy_threshold")) {
                service::operation::add_section(sections, "health_check");
            } else if (key.starts_with("access_log")) {
                service::operation::add_section(sections, "access_log");
            } else if (key == "https_enabled" || key == "certificate_ids" || key == "minimum_tls_version" ||
                       key == "force_https" || key == "hsts_enabled") {
                service::operation::add_section(sections, "https");
            } else if (key.starts_with("response_compression")) {
                service::operation::add_section(sections, "compression");
            } else {
                service::operation::add_section(sections, "rules");
            }
        }
        return sections;
    }

    static ruvia::task<std::string> find_website_status(ruvia::db_transaction& db, const std::string& tenant_id,
        const std::string& id) {
        const auto rows = co_await db.query(
            "SELECT status FROM sys_website WHERE tenant_id = $1 AND id = $2 LIMIT 1", tenant_id, id);
        co_return rows.empty() ? std::string{} : std::string(rows.front()[0].value().value_or(""));
    }

    static std::string serialize_config(ruvia::context& c,
        const service::website_config::website_config_data& input) {
        const auto output = service::website_config::to_output(input, {.resource_ = c.pool()});
        const auto json = ruvia::to_json(output, {.resource_ = c.pool()});
        return std::string(json.data(), json.size());
    }

    static ruvia::task<void> require_cluster(ruvia::db_transaction& db, const std::string& tenant_id,
        const std::string& cluster_id) {
        if (!(co_await lock_available_cluster(db, tenant_id, cluster_id))) {
            service::common::throw_app_error(website_error::cluster_unavailable);
        }
        co_return;
    }

    static ruvia::task<std::vector<domain_claim>>
    build_domain_claims(ruvia::db_transaction& db, const std::string& tenant_id,
        const std::optional<std::string>& excluded_website_id,
        const service::website_config::website_config_data& config) {
        const auto zones = co_await lock_dns_zone_references(db, tenant_id);
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

        const auto certificates = co_await lock_available_certificates(db, tenant_id, certificate_ids);
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
