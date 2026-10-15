#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/db/db_transaction.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/certificate/certificate_error.h"
#include "service/domains/certificate/certificate_types.h"
#include "service/features/certificate/config_mapper.h"
#include "service/features/certificate/dns_challenge.h"
#include "service/features/certificate/queue.h"
#include "service/features/dns/registry.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/sync_runtime/state.h"

namespace service::certificate {

class certificate_command_service final {
public:
    ruvia::task<std::optional<std::string>> create(ruvia::context& c, const std::string& tenant_id,
        const create_certificate_body& body) {
        const auto& domain_input = body.get<"domain">();
        const auto& provider_id_input = body.get<"certificate_provider_id">();
        const auto& zone_id_input = body.get<"dns_zone_id">();
        const auto& config_input = body.get<"config">();
        if (!domain_input || !provider_id_input || !zone_id_input || !config_input) {
            service::common::throw_app_error(service::common::validation_error_code,
                "证书域名、供应商、托管域名和配置不能为空", 400);
        }
        const auto config = service::certificate_issuance::normalize(*config_input);
        if (!config) {
            service::common::throw_app_error(service::common::validation_error_code, "证书配置不正确",
                400);
        }
        const auto domain = normalize_domain(domain_input->view());
        const auto provider_id = std::string(provider_id_input->view());
        const auto zone_id = std::string(zone_id_input->view());
        auto transaction = co_await c.db().begin_transaction();
        const auto exists = co_await transaction.query(
            "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND domain = $2 AND "
            "deleted_at IS NULL LIMIT 1",
            tenant_id, std::string_view(domain));
        if (!exists.empty()) {
            service::common::throw_app_error(certificate_error::domain_exists);
        }
        const auto provider_rows = co_await transaction.query(
            "SELECT id FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
            "'certificate' AND status = 'verified' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
            provider_id, tenant_id);
        if (provider_rows.empty()) {
            service::common::throw_app_error(certificate_error::provider_unavailable);
        }
        const auto zone_rows = co_await transaction.query(
            "SELECT zone.domain, dns_provider.provider FROM sys_dns_zone zone INNER JOIN "
            "sys_provider dns_provider ON dns_provider.tenant_id = zone.tenant_id AND "
            "dns_provider.id = zone.provider_id AND dns_provider.kind = 'dns' WHERE zone.id = $1 "
            "AND zone.tenant_id = $2 AND zone.sync_status = 'synced' AND "
            "dns_provider.status = 'verified' AND zone.deleted_at IS NULL AND "
            "dns_provider.deleted_at IS NULL LIMIT 1 FOR SHARE OF zone, dns_provider",
            zone_id, tenant_id);
        if (zone_rows.empty()) {
            service::common::throw_app_error(certificate_error::domain_unavailable);
        }
        if (!service::dns::find_dns_provider(zone_rows.front()[1].value().value_or(""))) {
            service::common::throw_app_error(certificate_error::dns_provider_unsupported);
        }
        const auto zone_domain = std::string(zone_rows.front()[0].value().value_or(""));
        const auto plain_domain = domain.starts_with("*.") ? domain.substr(2) : domain;
        if (plain_domain != zone_domain &&
            (plain_domain.size() <= zone_domain.size() || !plain_domain.ends_with("." + zone_domain))) {
            service::common::throw_app_error(certificate_error::domain_mismatch);
        }
        const auto config_json = serialize_certificate_config(c, *config);
        std::string certificate_id;
        try {
            const auto saved = co_await transaction.query(
                "INSERT INTO sys_certificate (tenant_id, provider_id, domain, dns_zone_id, "
                "status, config, material, revision, issuance_revision, issued_revision, "
                "created_at, updated_at) VALUES ($1, $2, $3, $4, 'pending', $5::jsonb, "
                "'{}'::jsonb, 1, 1, 0, NOW(), NOW()) RETURNING id",
                tenant_id, provider_id, std::string_view(domain), zone_id,
                std::string_view(config_json));
            certificate_id = std::string(saved.front()[0].value().value_or(""));
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error, "uk_certificate_domain")) {
                service::common::throw_app_error(certificate_error::domain_exists);
            }
            throw;
        }
        auto origin = start(c, service::operation::action::certificate_create, certificate_id);
        (void)co_await service::certificate_issuance::enqueue_certificate_revision(
            transaction, tenant_id, certificate_id, 1, service::sync_runtime::marker_operation::issue, origin);
        co_await transaction.commit();
        co_await notify(c, tenant_id, certificate_id, origin);
        co_return origin.operation_id();
    }

    ruvia::task<void> update(
        ruvia::context& c, const std::string& tenant_id, const std::string& id,
        std::int64_t expected_revision,
        const ruvia::validated_json<service::certificate_issuance::certificate_config_input>& config) {
        auto transaction = co_await c.db().begin_transaction();
        const auto certificates = co_await transaction.query(
            "SELECT revision FROM sys_certificate WHERE id = $1 AND tenant_id = $2 AND "
            "deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenant_id);
        if (certificates.empty()) {
            service::common::throw_app_error(certificate_error::not_found);
        }
        if (certificates.front()[0].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(certificate_error::revision_conflict);
        }
        const auto normalized_config = service::certificate_issuance::normalize(config.value());
        if (!normalized_config) {
            service::common::throw_app_error(service::common::validation_error_code, "证书配置不正确",
                400);
        }
        const auto config_json = serialize_certificate_config(c, *normalized_config);
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET revision = revision + 1, config = $1::jsonb, updated_at = "
            "NOW() WHERE id = $2 AND tenant_id = $3 AND revision = $4 AND deleted_at IS NULL",
            std::string_view(config_json), id, tenant_id, expected_revision);
        if (result.affected_rows() == 0) {
            service::common::throw_app_error(certificate_error::revision_conflict);
        }
        co_await transaction.commit();
        co_await service::live_resource::notify(c,
            service::live_resource::notifications::resource_notification(tenant_id)
                .changed(service::live_resource::resource::certificates, id));
        co_return;
    }

    ruvia::task<std::optional<std::string>> renew(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto origin = start(c, service::operation::action::certificate_renew, id);
        auto transaction = co_await c.db().begin_transaction();
        const auto rows = co_await transaction.query(
            "SELECT revision, issuance_revision, issued_revision FROM sys_certificate WHERE id = "
            "$1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(certificate_error::not_found);
        }
        if (rows.front()[0].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(certificate_error::revision_conflict);
        }
        const auto issuance_revision = rows.front()[1].as<std::int64_t>().value_or(0) + 1;
        const auto has_certificate = rows.front()[2].as<std::int64_t>().value_or(0) > 0;
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET issuance_revision = $1, status = $2, last_error = NULL, "
            "updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND revision = $5 AND "
            "deleted_at IS NULL",
            issuance_revision,
            has_certificate ? std::string_view{"renewing"} : std::string_view{"pending"}, id,
            tenant_id, expected_revision);
        if (result.affected_rows() == 0) {
            service::common::throw_app_error(certificate_error::revision_conflict);
        }
        (void)co_await service::certificate_issuance::enqueue_certificate_revision(
            transaction, tenant_id, id, issuance_revision, service::sync_runtime::marker_operation::renew, origin);
        co_await transaction.commit();
        co_await notify(c, tenant_id, id, origin);
        co_return origin.operation_id();
    }

    ruvia::task<std::optional<std::string>> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto origin = start(c, service::operation::action::certificate_delete, id);
        auto transaction = co_await c.db().begin_transaction();
        const auto certificates =
            co_await transaction.query(
                "SELECT revision, dns_zone_id FROM sys_certificate WHERE id "
                "= $1 AND tenant_id = $2 AND "
                "deleted_at IS NULL LIMIT 1 FOR UPDATE",
                id, tenant_id);
        if (certificates.empty()) {
            service::common::throw_app_error(certificate_error::not_found);
        }
        if (certificates.front()[0].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(certificate_error::revision_conflict);
        }
        const auto dns_zone_id = certificates.front()[1].value().value_or("");
        const auto consumers = co_await transaction.query(
            "SELECT website_id FROM sys_website_certificate_binding WHERE tenant_id = $1 AND "
            "certificate_id = $2 LIMIT 1",
            tenant_id, id);
        if (!consumers.empty()) {
            service::common::throw_app_error(certificate_error::in_use);
        }
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET material = '{}'::jsonb, revision = revision + 1, "
            "deleted_at = NOW(), updated_at = NOW() WHERE id = $1 AND tenant_id = $2 AND "
            "revision = $3 AND deleted_at IS NULL",
            id, tenant_id, expected_revision);
        if (result.affected_rows() == 0) {
            service::common::throw_app_error(certificate_error::not_found);
        }
        co_await service::certificate_issuance::retire_dns_challenges_for_deleted_certificate(
            transaction, tenant_id, dns_zone_id, id, origin);
        co_await service::sync_runtime::remove_marker(
            transaction, tenant_id, service::sync_runtime::marker_resource_type::certificate, id, origin);
        co_await transaction.commit();
        co_await notify(c, tenant_id, id, origin);
        co_return origin.operation_id();
    }

private:
    static service::operation::work_origin start(ruvia::context& c, service::operation::action action,
        const std::string& id) {
        return service::operation::work_origin::start({.action_ = action,
            .actor_id_ = service::middleware::current_principal(c).admin_id_,
            .target_type_ = "certificate",
            .target_id_ = id,
            .summary_ = "{}"});
    }

    static ruvia::task<void> notify(ruvia::context& c, const std::string& tenant_id, const std::string& id,
        const service::operation::work_origin& origin) {
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.changed(service::live_resource::resource::certificates, id);
        origin.add_changes(notification);
        co_await service::live_resource::notify(c, notification);
    }

    static std::string
    serialize_certificate_config(ruvia::context& c,
        const service::certificate_issuance::certificate_config_data& config) {
        const auto output =
            service::certificate_issuance::to_output(config, {.resource_ = c.pool()});
        const auto json = ruvia::to_json(output, {.resource_ = c.pool()});
        return std::string(json.data(), json.size());
    }

    static std::string normalize_domain(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }
};

inline certificate_command_service& get_certificate_command_service() {
    static certificate_command_service service;
    return service;
}

}  // namespace service::certificate
