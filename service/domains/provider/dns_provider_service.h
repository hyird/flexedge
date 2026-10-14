#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/provider/dns_provider_error.h"
#include "service/domains/provider/dns_provider_read_service.h"
#include "service/domains/provider/dns_provider_types.h"
#include "service/features/dns/provider_config_mapper.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/provider_verification/queue.h"
#include "service/utils/secret.h"

namespace service::provider {

class dns_provider_service final {
public:
    ruvia::task<dns_provider_page_data_dto> list(ruvia::context& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size,
        std::int64_t skip,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& status) {
        co_return co_await get_dns_provider_read_service().list(c, tenant_id, page, page_size, skip, keyword,
            status);
    }

    ruvia::task<dns_provider_dto> get(ruvia::context& c, const std::string& tenant_id,
        const std::string& id) {
        co_return co_await get_dns_provider_read_service().get(c, tenant_id, id);
    }

    ruvia::task<void> create(ruvia::context& c, const std::string& tenant_id,
        const create_dns_provider_body& body) {
        const auto& name_input = body.get<"name">();
        const auto& provider_input = body.get<"provider">();
        const auto& account_id_input = body.get<"account_id">();
        const auto& token_input = body.get<"api_token">();
        if (!name_input || !provider_input || !account_id_input || !token_input) {
            service::common::throw_app_error(service::common::validation_error_code,
                "DNS 供应商名称、类型、账号和令牌不能为空", 400);
        }
        const auto name = trim(name_input->view());
        const auto provider = normalize_provider(provider_input->view());
        const auto account_id = normalize_account_id(provider, account_id_input->view());
        const auto token = token_input->view();
        validate_credential_shape(provider, account_id, token);
        const auto config = service::dns::serialize_dns_provider_config(
            service::utils::seal_secret(token), secret_hint(token), c.pool());
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto inserted = co_await transaction.query(
                "INSERT INTO sys_provider (tenant_id, kind, provider, name, account_id, "
                "revision, config, runtime, status, created_at, updated_at) VALUES ($1, 'dns', "
                "$2, $3, $4, 1, $5::jsonb, '{}'::jsonb, 'unverified', NOW(), NOW()) RETURNING id",
                tenant_id, std::string_view(provider), std::string_view(name),
                std::string_view(account_id), std::string_view(config));
            co_await transaction.commit();
            service::live_resource::get_hub().publish(tenant_id, service::live_resource::resource::providers,
                inserted.front()[0].value().value_or(""));
            service::live_resource::get_hub().publish(tenant_id, service::live_resource::resource::tasks);
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error, "uk_provider_name")) {
                service::common::throw_app_error(dns_provider_error::name_exists);
            }
            if (service::common::is_unique_constraint_violation(error, "uk_provider_dns_account")) {
                service::common::throw_app_error(dns_provider_error::account_exists);
            }
            throw;
        }
        co_return;
    }

    ruvia::task<void> update(ruvia::context& c, const std::string& tenant_id, const std::string& id,
        std::int64_t expected_revision, const update_dns_provider_body& body) {
        const auto rows = co_await c.db().query(
            "SELECT provider, revision, account_id, config::text FROM sys_provider WHERE id = $1 "
            "AND "
            "tenant_id = $2 AND kind = 'dns' AND deleted_at IS NULL LIMIT 1",
            id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(dns_provider_error::not_found);
        }
        if (rows.front()[1].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(dns_provider_error::revision_conflict);
        }

        const auto provider = std::string(rows.front()[0].value().value_or(""));
        const auto account_id = std::string(rows.front()[2].value().value_or(""));
        const auto current = service::dns::parse_dns_provider_config(
            rows.front()[3].value().value_or("{}"), c.pool());
        const auto& name_input = body.get<"name">();
        if (!name_input) {
            service::common::throw_app_error(service::common::validation_error_code,
                "DNS 供应商名称不能为空", 400);
        }
        const auto name = trim(name_input->view());
        co_await ensure_available(c, tenant_id, provider, name, account_id, id);

        auto envelope = current.credential_envelope_;
        auto hint = current.credential_hint_;
        bool credentials_changed = false;
        if (const auto& token = body.get<"api_token">(); token) {
            validate_token_shape(provider, token->view());
            envelope = service::utils::seal_secret(token->view());
            hint = secret_hint(token->view());
            credentials_changed = true;
        }
        const auto config = service::dns::serialize_dns_provider_config(envelope, hint, c.pool());
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto result = co_await transaction.execute(
                "UPDATE sys_provider SET name = $1, revision = revision + 1, config = $2::jsonb, "
                "verification_generation = verification_generation + 1, runtime = CASE WHEN "
                "$3::BOOLEAN THEN '{}'::jsonb ELSE runtime END, "
                "status = CASE WHEN $3::BOOLEAN THEN 'unverified' ELSE status END, "
                "last_verified_at = CASE WHEN $3::BOOLEAN THEN NULL ELSE last_verified_at END, "
                "last_error = CASE WHEN $3::BOOLEAN THEN NULL ELSE last_error END, updated_at = "
                "NOW() WHERE id = $4 AND tenant_id = $5 AND kind = 'dns' AND revision = $6 "
                "AND deleted_at IS NULL",
                std::string_view(name), std::string_view(config), credentials_changed, id, tenant_id,
                expected_revision);
            if (result.affected_rows() == 0) {
                service::common::throw_app_error(dns_provider_error::revision_conflict);
            }
            if (credentials_changed) {
                co_await service::provider_verification::mark_current(transaction, tenant_id, id);
            } else {
                co_await service::provider_verification::remove(transaction, tenant_id, id);
            }
            co_await transaction.commit();
            service::live_resource::get_hub().publish(tenant_id, service::live_resource::resource::providers, id);
            service::live_resource::get_hub().publish(tenant_id, service::live_resource::resource::tasks);
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error, "uk_provider_name")) {
                service::common::throw_app_error(dns_provider_error::name_exists);
            }
            throw;
        }
        co_return;
    }

    ruvia::task<void> verify_stored(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto transaction = co_await c.db().begin_transaction();
        if (!co_await service::provider_verification::enqueue_dns(transaction, tenant_id, id,
                expected_revision)) {
            const auto current = co_await transaction.query(
                "SELECT revision FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind "
                "= 'dns' AND deleted_at IS NULL",
                id, tenant_id);
            if (current.empty()) {
                service::common::throw_app_error(dns_provider_error::not_found);
            }
            service::common::throw_app_error(dns_provider_error::revision_conflict);
        }
        co_await transaction.commit();
        service::live_resource::get_hub().publish(tenant_id, service::live_resource::resource::providers, id);
        service::live_resource::get_hub().publish(tenant_id, service::live_resource::resource::tasks);
        co_return;
    }

    ruvia::task<void> remove(ruvia::context& c, const std::string& tenant_id, const std::string& id,
        std::int64_t expected_revision) {
        auto transaction = co_await c.db().begin_transaction();
        const auto rows = co_await transaction.query(
            "SELECT revision FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
            "'dns' AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(dns_provider_error::not_found);
        }
        if (rows.front()[0].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(dns_provider_error::revision_conflict);
        }
        const auto references = co_await transaction.query(
            "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND provider_id = $2 AND "
            "(deleted_at IS NULL OR synced_revision < desired_revision OR sync_status <> "
            "'synced') LIMIT 1",
            tenant_id, id);
        if (!references.empty()) {
            service::common::throw_app_error(dns_provider_error::in_use);
        }
        (void)co_await transaction.execute(
            "UPDATE sys_provider SET config = '{}'::jsonb, runtime = '{}'::jsonb, revision = "
            "revision + 1, verification_generation = verification_generation + 1, deleted_at = "
            "NOW(), updated_at = NOW() WHERE id = $1 AND tenant_id = $2 AND revision = $3",
            id, tenant_id, expected_revision);
        co_await service::provider_verification::remove(transaction, tenant_id, id);
        co_await transaction.commit();
        service::live_resource::get_hub().publish(tenant_id, service::live_resource::resource::providers, id);
        service::live_resource::get_hub().publish(tenant_id, service::live_resource::resource::tasks);
        co_return;
    }

private:
    static ruvia::task<void> ensure_available(ruvia::context& c, const std::string& tenant_id,
        std::string_view provider, std::string_view name,
        std::string_view account_id,
        const std::optional<std::string>& excluded_id) {
        const auto rows = co_await c.db().query(
            "SELECT id, name, account_id FROM sys_provider "
            "WHERE tenant_id = $1 AND kind = 'dns' "
            "AND provider = $2 AND deleted_at IS NULL",
            tenant_id, provider);
        for (const auto& row : rows) {
            if (excluded_id && row[0].value().value_or("") == *excluded_id) {
                continue;
            }
            if (row[1].value().value_or("") == name) {
                service::common::throw_app_error(dns_provider_error::name_exists);
            }
            if (row[2].value().value_or("") == account_id) {
                service::common::throw_app_error(dns_provider_error::account_exists);
            }
        }
        co_return;
    }

    static std::string trim(std::string_view input) {
        const auto begin = std::find_if_not(input.begin(), input.end(),
            [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }).base();
        return begin < end ? std::string(begin, end) : std::string{};
    }

    static std::string normalize_provider(std::string_view input) {
        auto result = trim(input);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (result != "cloudflare" && result != "aliyun") {
            service::common::throw_app_error(service::common::validation_error_code,
                "服务商类型不支持", 400);
        }
        return result;
    }

    static std::string normalize_account_id(std::string_view provider, std::string_view input) {
        std::string result(input);
        if (provider == "cloudflare") {
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        }
        return result;
    }

    static void validate_credential_shape(std::string_view provider, std::string_view account_id,
        std::string_view token) {
        if (provider == "cloudflare" && !is_cloudflare_account_id(account_id)) {
            service::common::throw_app_error(service::common::validation_error_code,
                "Cloudflare Account ID 格式不正确", 400);
        }
        validate_token_shape(provider, token);
    }

    static void validate_token_shape(std::string_view provider, std::string_view token) {
        if (provider == "cloudflare" && (token.size() < 20 || token.size() > 200)) {
            service::common::throw_app_error(service::common::validation_error_code,
                "Cloudflare API Token 格式不正确", 400);
        }
        if (provider == "aliyun" && (token.size() < 16 || token.size() > 256)) {
            service::common::throw_app_error(service::common::validation_error_code,
                "阿里云 AccessKey Secret 格式不正确", 400);
        }
    }

    static bool is_cloudflare_account_id(std::string_view value) {
        return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
    }

    static std::string secret_hint(std::string_view value) {
        constexpr std::size_t visible{4};
        return "****" +
               std::string(value.substr(value.size() > visible ? value.size() - visible : 0));
    }
};

inline dns_provider_service& get_dns_provider_service() {
    static dns_provider_service service;
    return service;
}

}  // namespace service::provider
