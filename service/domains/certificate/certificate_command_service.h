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
#include "service/domains/dns_zone/dns_zone_reference.h"
#include "service/domains/provider/provider_reference.h"
#include "service/domains/website/website_reference.h"
#include "service/features/background/queued_work.h"
#include "service/features/certificate/config_mapper.h"
#include "service/features/certificate/queue.h"
#include "service/features/dns/registry.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"

namespace service::certificate {

class certificate_command_service final {
public:
    ruvia::task<std::int64_t> create(ruvia::context& c, const std::string& tenant_id,
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
        if (!co_await service::provider::lock_verified_certificate_provider(transaction, tenant_id, provider_id)) {
            service::common::throw_app_error(certificate_error::provider_unavailable);
        }
        const auto zone = co_await service::dns_zone::lock_dns_zone(transaction, tenant_id, zone_id);
        if (!zone || !zone->synced_) {
            service::common::throw_app_error(certificate_error::domain_unavailable);
        }
        const auto dns_provider = co_await service::provider::lock_dns_provider(transaction, tenant_id, zone->provider_id_);
        if (!dns_provider || !dns_provider->verified_) {
            service::common::throw_app_error(certificate_error::domain_unavailable);
        }
        if (!service::dns::find_dns_provider(dns_provider->provider_)) {
            service::common::throw_app_error(certificate_error::dns_provider_unsupported);
        }
        const auto& zone_domain = zone->domain_;
        const auto plain_domain = domain.starts_with("*.") ? domain.substr(2) : domain;
        if (plain_domain != zone_domain &&
            (plain_domain.size() <= zone_domain.size() || !plain_domain.ends_with("." + zone_domain))) {
            service::common::throw_app_error(certificate_error::domain_mismatch);
        }
        const auto config_json = serialize_certificate_config(c, *config);
        std::string certificate_id;
        std::int64_t revision{};
        try {
            const auto saved = co_await transaction.query(
                "INSERT INTO sys_certificate (tenant_id, provider_id, domain, dns_zone_id, "
                "status, config, material, revision, issuance_revision, issued_revision, "
                "created_at, updated_at) VALUES ($1, $2, $3, $4, 'pending', $5::jsonb, "
                "'{}'::jsonb, 1, 1, 0, NOW(), NOW()) RETURNING id, revision",
                tenant_id, provider_id, std::string_view(domain), zone_id,
                std::string_view(config_json));
            certificate_id = std::string(saved.front()[0].value().value_or(""));
            revision = saved.front()[1].as<std::int64_t>().value_or(1);
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error, "uk_certificate_domain")) {
                service::common::throw_app_error(certificate_error::domain_exists);
            }
            throw;
        }
        service::background::queued_work work;
        (void)co_await service::certificate_issuance::enqueue_certificate_revision(
            transaction, tenant_id, certificate_id, 1, service::sync_runtime::marker_operation::issue, work);
        co_await commit_changes(transaction, c, tenant_id, certificate_id, work);
        co_return revision;
    }

    ruvia::task<std::int64_t> update(
        ruvia::context& c, const std::string& tenant_id, const std::string& id,
        std::int64_t expected_revision,
        const ruvia::validated_json<service::certificate_issuance::certificate_config_input>& config) {
        auto transaction = co_await c.db().begin_transaction();
        const auto certificates = co_await transaction.query(
            "SELECT revision FROM sys_certificate WHERE id = $1 AND tenant_id = $2 AND "
            "deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenant_id);
        service::common::require_current_revision(certificates, expected_revision, revision_errors_);
        const auto normalized_config = service::certificate_issuance::normalize(config.value());
        if (!normalized_config) {
            service::common::throw_app_error(service::common::validation_error_code, "证书配置不正确",
                400);
        }
        const auto config_json = serialize_certificate_config(c, *normalized_config);
        const auto updated = co_await transaction.query(
            "UPDATE sys_certificate SET revision = revision + 1, config = $1::jsonb, updated_at = "
            "NOW() WHERE id = $2 AND tenant_id = $3 RETURNING revision",
            std::string_view(config_json), id, tenant_id);
        co_await commit_changes(transaction, c, tenant_id, id, service::background::queued_work{});
        co_return updated.front()[0].as<std::int64_t>().value_or(0);
    }

    // Renewal leaves the revision unchanged; returns it.
    ruvia::task<std::int64_t> renew(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto transaction = co_await c.db().begin_transaction();
        const auto rows = co_await transaction.query(
            "SELECT revision, issuance_revision, issued_revision FROM sys_certificate WHERE id = "
            "$1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenant_id);
        service::common::require_current_revision(rows, expected_revision, revision_errors_);
        const auto issuance_revision = rows.front()[1].as<std::int64_t>().value_or(0) + 1;
        const auto has_certificate = rows.front()[2].as<std::int64_t>().value_or(0) > 0;
        (void)co_await transaction.execute(
            "UPDATE sys_certificate SET issuance_revision = $1, status = $2, last_error = NULL, "
            "updated_at = NOW() WHERE id = $3 AND tenant_id = $4",
            issuance_revision,
            has_certificate ? std::string_view{"renewing"} : std::string_view{"pending"}, id,
            tenant_id);
        service::background::queued_work work;
        (void)co_await service::certificate_issuance::enqueue_certificate_revision(
            transaction, tenant_id, id, issuance_revision, service::sync_runtime::marker_operation::renew, work);
        co_await commit_changes(transaction, c, tenant_id, id, work);
        co_return expected_revision;
    }

    ruvia::task<std::int64_t> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto transaction = co_await c.db().begin_transaction();
        const auto certificates =
            co_await transaction.query(
                "SELECT revision, dns_zone_id FROM sys_certificate WHERE id "
                "= $1 AND tenant_id = $2 AND "
                "deleted_at IS NULL LIMIT 1 FOR UPDATE",
                id, tenant_id);
        service::common::require_current_revision(certificates, expected_revision, revision_errors_);
        const auto dns_zone_id = certificates.front()[1].value().value_or("");
        if (co_await service::website::websites_bind_certificate(transaction, tenant_id, id)) {
            service::common::throw_app_error(certificate_error::in_use);
        }
        const auto removed = co_await transaction.query(
            "UPDATE sys_certificate SET material = '{}'::jsonb, revision = revision + 1, "
            "deleted_at = NOW(), updated_at = NOW() WHERE id = $1 AND tenant_id = $2 RETURNING revision",
            id, tenant_id);
        service::background::queued_work work;
        (void)co_await service::dns_sync::replace_challenge_records(transaction, tenant_id, dns_zone_id, id, {}, 0,
            work);
        co_await service::sync_runtime::remove_marker(
            transaction, tenant_id, service::sync_runtime::marker_resource_type::certificate, id, work);
        co_await commit_changes(transaction, c, tenant_id, id, work);
        co_return removed.front()[0].as<std::int64_t>().value_or(0);
    }

private:
    static constexpr service::common::revision_errors revision_errors_{certificate_error::not_found,
        certificate_error::revision_conflict};

    static ruvia::task<void> commit_changes(ruvia::db_transaction& transaction, ruvia::context& c, const std::string& tenant_id, const std::string& id,
        const service::background::queued_work& work) {
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.changed(service::live_resource::resource::certificates, id);
        co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
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
