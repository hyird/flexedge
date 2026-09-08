#pragma once

#include "service/features/sync_event/fanout.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/provider/dns_provider.error.h"
#include "service/domains/provider/dns_provider_read.service.h"
#include "service/domains/provider/dns_provider.types.h"
#include "service/features/dns/provider_config.h"
#include "service/features/provider_verification/queue.h"
#include "service/utils/secret.h"

namespace service::provider {

class DnsProviderService final {
  public:
    ruvia::Task<DnsProviderPageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                             std::int64_t page, std::int64_t pageSize,
                                             std::int64_t skip,
                                             const std::optional<std::string>& keyword,
                                             const std::optional<std::string>& status) {
        co_return co_await dnsProviderReadService().list(c, tenantId, page, pageSize, skip, keyword,
                                                         status);
    }

    ruvia::Task<DnsProviderDto> get(ruvia::Context& c, const std::string& tenantId,
                                    const std::string& id) {
        co_return co_await dnsProviderReadService().get(c, tenantId, id);
    }

    ruvia::Task<void> create(ruvia::Context& c, const std::string& tenantId,
                             const CreateDnsProviderBody& body) {
        const auto& nameInput = body.get<"name">();
        const auto& providerInput = body.get<"provider">();
        const auto& accountIdInput = body.get<"accountId">();
        const auto& tokenInput = body.get<"apiToken">();
        if (!nameInput || !providerInput || !accountIdInput || !tokenInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "DNS 供应商名称、类型、账号和令牌不能为空", 400);
        }
        const auto name = trim(nameInput->view());
        const auto provider = normalizeProvider(providerInput->view());
        const auto accountId = normalizeAccountId(provider, accountIdInput->view());
        const auto token = tokenInput->view();
        validateCredentialShape(provider, accountId, token);
        const auto config = service::dns::serializeDnsProviderConfig(
            service::utils::sealSecret(token), secretHint(token), c.resource());
        try {
            auto transaction = co_await c.db().beginTransaction();
            (void)co_await transaction.execute(
                "INSERT INTO sys_provider (tenant_id, kind, provider, name, account_id, "
                "revision, config, runtime, status, created_at, updated_at) VALUES ($1, 'dns', "
                "$2, $3, $4, 1, $5::jsonb, '{}'::jsonb, 'unverified', NOW(), NOW())",
                tenantId, std::string_view(provider), std::string_view(name),
                std::string_view(accountId), std::string_view(config));
            co_await transaction.commit();
            service::sync_event::fanout::hub().publish(tenantId);
        } catch (const ruvia::DbError& error) {
            if (service::common::isUniqueConstraintViolation(error, "uk_provider_name")) {
                service::common::throwAppError(DnsProviderError::NAME_EXISTS);
            }
            if (service::common::isUniqueConstraintViolation(error, "uk_provider_dns_account")) {
                service::common::throwAppError(DnsProviderError::ACCOUNT_EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> update(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision, const UpdateDnsProviderBody& body) {
        const auto rows = co_await c.db().query(
            "SELECT provider, revision, account_id, config::text FROM sys_provider WHERE id = $1 "
            "AND "
            "tenant_id = $2 AND kind = 'dns' AND deleted_at IS NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(DnsProviderError::NOT_FOUND);
        }
        if (rows.front()[1].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(DnsProviderError::REVISION_CONFLICT);
        }

        const auto provider = std::string(rows.front()[0].value().value_or(""));
        const auto accountId = std::string(rows.front()[2].value().value_or(""));
        const auto current = service::dns::parseDnsProviderConfig(
            rows.front()[3].value().value_or("{}"), c.resource());
        const auto& nameInput = body.get<"name">();
        if (!nameInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "DNS 供应商名称不能为空", 400);
        }
        const auto name = trim(nameInput->view());
        co_await ensureAvailable(c, tenantId, provider, name, accountId, id);

        auto envelope = current.credentialEnvelope;
        auto hint = current.credentialHint;
        bool credentialsChanged = false;
        if (const auto& token = body.get<"apiToken">(); token) {
            validateTokenShape(provider, token->view());
            envelope = service::utils::sealSecret(token->view());
            hint = secretHint(token->view());
            credentialsChanged = true;
        }
        const auto config = service::dns::serializeDnsProviderConfig(envelope, hint, c.resource());
        try {
            auto transaction = co_await c.db().beginTransaction();
            const auto result = co_await transaction.execute(
                "UPDATE sys_provider SET name = $1, revision = revision + 1, config = $2::jsonb, "
                "verification_generation = verification_generation + 1, runtime = CASE WHEN "
                "$3::BOOLEAN THEN '{}'::jsonb ELSE runtime END, "
                "status = CASE WHEN $3::BOOLEAN THEN 'unverified' ELSE status END, "
                "last_verified_at = CASE WHEN $3::BOOLEAN THEN NULL ELSE last_verified_at END, "
                "last_error = CASE WHEN $3::BOOLEAN THEN NULL ELSE last_error END, updated_at = "
                "NOW() WHERE id = $4 AND tenant_id = $5 AND kind = 'dns' AND revision = $6 "
                "AND deleted_at IS NULL",
                std::string_view(name), std::string_view(config), credentialsChanged, id, tenantId,
                expectedRevision);
            if (result.affectedRows() == 0) {
                service::common::throwAppError(DnsProviderError::REVISION_CONFLICT);
            }
            if (credentialsChanged) {
                co_await service::provider_verification::markCurrent(transaction, tenantId, id);
            } else {
                co_await service::provider_verification::remove(transaction, tenantId, id);
            }
            co_await transaction.commit();
            service::sync_event::fanout::hub().publish(tenantId);
        } catch (const ruvia::DbError& error) {
            if (service::common::isUniqueConstraintViolation(error, "uk_provider_name")) {
                service::common::throwAppError(DnsProviderError::NAME_EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> verifyStored(ruvia::Context& c, const std::string& tenantId,
                                   const std::string& id, std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        if (!co_await service::provider_verification::enqueueDns(transaction, tenantId, id,
                                                                 expectedRevision)) {
            const auto current = co_await transaction.query(
                "SELECT revision FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind "
                "= 'dns' AND deleted_at IS NULL",
                id, tenantId);
            if (current.empty()) {
                service::common::throwAppError(DnsProviderError::NOT_FOUND);
            }
            service::common::throwAppError(DnsProviderError::REVISION_CONFLICT);
        }
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "SELECT revision FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
            "'dns' AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(DnsProviderError::NOT_FOUND);
        }
        if (rows.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(DnsProviderError::REVISION_CONFLICT);
        }
        const auto references = co_await transaction.query(
            "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND provider_id = $2 AND "
            "(deleted_at IS NULL OR synced_revision < desired_revision OR sync_status <> "
            "'synced') LIMIT 1",
            tenantId, id);
        if (!references.empty()) {
            service::common::throwAppError(DnsProviderError::IN_USE);
        }
        (void)co_await transaction.execute(
            "UPDATE sys_provider SET config = '{}'::jsonb, runtime = '{}'::jsonb, revision = "
            "revision + 1, verification_generation = verification_generation + 1, deleted_at = "
            "NOW(), updated_at = NOW() WHERE id = $1 AND tenant_id = $2 AND revision = $3",
            id, tenantId, expectedRevision);
        co_await service::provider_verification::remove(transaction, tenantId, id);
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

  private:
    static ruvia::Task<void> ensureAvailable(ruvia::Context& c, const std::string& tenantId,
                                             std::string_view provider, std::string_view name,
                                             std::string_view accountId,
                                             const std::optional<std::string>& excludedId) {
        const auto rows = co_await c.db().query("SELECT id, name, account_id FROM sys_provider "
                                                "WHERE tenant_id = $1 AND kind = 'dns' "
                                                "AND provider = $2 AND deleted_at IS NULL",
                                                tenantId, provider);
        for (const auto& row : rows) {
            if (excludedId && row[0].value().value_or("") == *excludedId) {
                continue;
            }
            if (row[1].value().value_or("") == name) {
                service::common::throwAppError(DnsProviderError::NAME_EXISTS);
            }
            if (row[2].value().value_or("") == accountId) {
                service::common::throwAppError(DnsProviderError::ACCOUNT_EXISTS);
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

    static std::string normalizeProvider(std::string_view input) {
        auto result = trim(input);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (result != "cloudflare" && result != "aliyun") {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "服务商类型不支持", 400);
        }
        return result;
    }

    static std::string normalizeAccountId(std::string_view provider, std::string_view input) {
        std::string result(input);
        if (provider == "cloudflare") {
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        }
        return result;
    }

    static void validateCredentialShape(std::string_view provider, std::string_view accountId,
                                        std::string_view token) {
        if (provider == "cloudflare" && !isCloudflareAccountId(accountId)) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "Cloudflare Account ID 格式不正确", 400);
        }
        validateTokenShape(provider, token);
    }

    static void validateTokenShape(std::string_view provider, std::string_view token) {
        if (provider == "cloudflare" && (token.size() < 20 || token.size() > 200)) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "Cloudflare API Token 格式不正确", 400);
        }
        if (provider == "aliyun" && (token.size() < 16 || token.size() > 256)) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "阿里云 AccessKey Secret 格式不正确", 400);
        }
    }

    static bool isCloudflareAccountId(std::string_view value) {
        return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                   return std::isxdigit(ch) != 0;
               });
    }

    static std::string secretHint(std::string_view value) {
        constexpr std::size_t visible{4};
        return "****" +
               std::string(value.substr(value.size() > visible ? value.size() - visible : 0));
    }
};

inline DnsProviderService& dnsProviderService() {
    static DnsProviderService service;
    return service;
}

} // namespace service::provider
