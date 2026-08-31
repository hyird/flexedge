#pragma once

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
#include "service/domains/provider/certificate_provider.error.h"
#include "service/domains/provider/certificate_provider.types.h"
#include "service/features/certificate/provider_config.h"
#include "service/features/provider_verification/queue.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::provider {

class CertificateProviderService final {
  public:
    ruvia::Task<ruvia::Array<CertificateProviderDto>> list(ruvia::Context& c,
                                                           const std::string& tenantId) {
        const auto rows = co_await c.db().query(
            "SELECT id, revision, provider, config::text, status, TO_CHAR(last_verified_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), last_error, TO_CHAR(created_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), TO_CHAR(updated_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_provider WHERE tenant_id = $1 AND "
            "kind = 'certificate' AND deleted_at IS NULL ORDER BY sort DESC",
            tenantId);
        ruvia::Array<CertificateProviderDto> result(c.resource());
        for (const auto& row : rows) {
            fill(c, result.emplace_back(c), row);
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const std::string& tenantId,
                             const CreateCertificateProviderBody& body) {
        const auto& providerInput = body.get<"provider">();
        const auto& credentialModeInput = body.get<"credentialMode">();
        if (!providerInput || !credentialModeInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "证书供应商和接入方式不能为空", 400);
        }
        const auto provider = std::string(providerInput->view());
        const auto credentialMode = std::string(credentialModeInput->view());
        std::optional<std::string> accountEmail;
        if (const auto& value = body.get<"accountEmail">(); value) {
            accountEmail = normalizeEmail(value->view());
        }
        std::optional<std::string_view> accessKey;
        if (const auto& value = body.get<"accessKey">(); value) {
            accessKey = value->view();
        }
        validateCredentials(provider, credentialMode, accountEmail, accessKey);
        const auto config = service::certificate_issuance::serializeCertificateProviderConfig(
            credentialMode, accountEmail,
            accessKey ? std::optional<std::string>{service::utils::sealSecret(*accessKey)}
                      : std::nullopt,
            accessKey ? std::optional<std::string>{secretHint(*accessKey)} : std::nullopt,
            c.resource());
        try {
            (void)co_await c.db().execute(
                "INSERT INTO sys_provider (tenant_id, kind, provider, revision, config, "
                "runtime, status, created_at, updated_at) VALUES ($1, 'certificate', $2, 1, "
                "$3::jsonb, '{}'::jsonb, 'unverified', NOW(), NOW())",
                tenantId, std::string_view(provider), std::string_view(config));
        } catch (const ruvia::DbError& error) {
            if (service::common::isUniqueConstraintViolation(error,
                                                             "uk_provider_certificate_type")) {
                service::common::throwAppError(CertificateProviderError::EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> update(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision,
                             const UpdateCertificateProviderBody& body) {
        const auto rows = co_await c.db().query(
            "SELECT provider, revision, config::text FROM sys_provider WHERE id = $1 AND "
            "tenant_id = $2 AND kind = 'certificate' AND deleted_at IS NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(CertificateProviderError::NOT_FOUND);
        }
        if (rows.front()[1].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(CertificateProviderError::REVISION_CONFLICT);
        }
        const auto provider = std::string(rows.front()[0].value().value_or(""));
        const auto current = service::certificate_issuance::parseCertificateProviderConfig(
            rows.front()[2].value().value_or("{}"), c.resource());
        const auto& credentialModeInput = body.get<"credentialMode">();
        if (!credentialModeInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "证书接入方式不能为空", 400);
        }
        const auto credentialMode = std::string(credentialModeInput->view());
        std::optional<std::string> accountEmail;
        if (const auto& value = body.get<"accountEmail">(); value) {
            accountEmail = normalizeEmail(value->view());
        }
        std::optional<std::string_view> submittedKey;
        if (const auto& value = body.get<"accessKey">(); value) {
            submittedKey = value->view();
        }
        std::optional<service::utils::SensitiveString> storedKey;
        std::optional<std::string_view> plainKey;
        std::optional<std::string> envelope;
        std::optional<std::string> hint;
        if (credentialMode == "access_key") {
            if (submittedKey) {
                plainKey = *submittedKey;
                envelope = service::utils::sealSecret(*submittedKey);
                hint = secretHint(*submittedKey);
            } else if (current.credentialMode == "access_key" && current.envelope) {
                envelope = current.envelope;
                hint = current.hint;
                storedKey.emplace(service::utils::openSecret(*current.envelope));
                plainKey = storedKey->view();
            }
        }
        validateCredentials(provider, credentialMode, accountEmail, plainKey);
        const bool changed = credentialMode != current.credentialMode ||
                             accountEmail != current.accountEmail || submittedKey.has_value();
        const auto config = service::certificate_issuance::serializeCertificateProviderConfig(
            credentialMode, accountEmail, envelope, hint, c.resource());
        auto transaction = co_await c.db().beginTransaction();
        const auto result = co_await transaction.execute(
            "UPDATE sys_provider SET revision = revision + 1, config = $1::jsonb, runtime = CASE "
            "WHEN $2::BOOLEAN THEN '{}'::jsonb ELSE runtime END, status = CASE WHEN $2::BOOLEAN "
            "THEN 'unverified' ELSE status END, last_verified_at = CASE WHEN $2::BOOLEAN THEN "
            "NULL ELSE last_verified_at END, verification_generation = verification_generation "
            "+ 1, last_error = NULL, updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND "
            "revision = $5 AND kind = 'certificate' AND deleted_at IS NULL",
            std::string_view(config), changed, id, tenantId, expectedRevision);
        if (result.affectedRows() == 0) {
            service::common::throwAppError(CertificateProviderError::REVISION_CONFLICT);
        }
        if (changed) {
            co_await service::provider_verification::markCurrent(transaction, tenantId, id);
        } else {
            co_await service::provider_verification::remove(transaction, tenantId, id);
        }
        co_await transaction.commit();
        co_return;
    }

    ruvia::Task<void> verify(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        if (!co_await service::provider_verification::enqueueCertificate(transaction, tenantId, id,
                                                                         expectedRevision)) {
            const auto current = co_await transaction.query(
                "SELECT revision FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind "
                "= 'certificate' AND deleted_at IS NULL",
                id, tenantId);
            if (current.empty()) {
                service::common::throwAppError(CertificateProviderError::NOT_FOUND);
            }
            service::common::throwAppError(CertificateProviderError::REVISION_CONFLICT);
        }
        co_await transaction.commit();
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "SELECT revision FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
            "'certificate' AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(CertificateProviderError::NOT_FOUND);
        }
        if (rows.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(CertificateProviderError::REVISION_CONFLICT);
        }
        const auto used = co_await transaction.query(
            "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND provider_id = $2 AND "
            "deleted_at IS NULL LIMIT 1",
            tenantId, id);
        if (!used.empty()) {
            service::common::throwAppError(CertificateProviderError::IN_USE);
        }
        (void)co_await transaction.execute(
            "UPDATE sys_provider SET config = '{}'::jsonb, runtime = '{}'::jsonb, revision = "
            "revision + 1, verification_generation = verification_generation + 1, deleted_at = "
            "NOW(), updated_at = NOW() WHERE id = $1 AND tenant_id = $2 AND revision = $3",
            id, tenantId, expectedRevision);
        co_await service::provider_verification::remove(transaction, tenantId, id);
        co_await transaction.commit();
        co_return;
    }

  private:
    template <typename Row>
    static void fill(ruvia::Context& c, CertificateProviderDto& item, const Row& row) {
        const auto config = service::certificate_issuance::parseCertificateProviderConfig(
            row[3].value().value_or("{}"), c.resource());
        item.set<"id">(row[0].value().value_or(""));
        item.set<"revision">(row[1].template as<std::int64_t>().value_or(1));
        item.set<"provider">(row[2].value().value_or(""));
        item.set<"credentialMode">(config.credentialMode);
        item.set<"status">(row[4].value().value_or(""));
        item.set<"createdAt">(row[7].value().value_or(""));
        item.set<"updatedAt">(row[8].value().value_or(""));
        if (config.accountEmail) {
            item.set<"accountEmail">(*config.accountEmail);
        }
        if (config.hint) {
            item.set<"accessKeyHint">(*config.hint);
        }
        if (const auto& lastVerifiedAt = row[5].value()) {
            item.set<"lastVerifiedAt">(*lastVerifiedAt);
        }
        if (const auto& lastError = row[6].value()) {
            item.set<"lastError">(*lastError);
        }
    }

    static std::string trim(std::string_view input) {
        const auto begin = std::find_if_not(input.begin(), input.end(),
                                            [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
                             return std::isspace(ch) != 0;
                         }).base();
        return begin < end ? std::string(begin, end) : std::string{};
    }

    static std::string normalizeEmail(std::string_view input) {
        auto result = trim(input);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    static std::string secretHint(std::string_view value) {
        constexpr std::size_t visible{4};
        return "****" +
               std::string(value.substr(value.size() > visible ? value.size() - visible : 0));
    }

    static void validateCredentials(std::string_view provider, std::string_view credentialMode,
                                    const std::optional<std::string>& accountEmail,
                                    const std::optional<std::string_view>& accessKey) {
        if (credentialMode == "email") {
            if (!accountEmail || accountEmail->empty()) {
                service::common::throwAppError(CertificateProviderError::EMAIL_REQUIRED);
            }
            if (accessKey) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "邮箱接入方式不能提交 API Access Key", 422);
            }
            return;
        }
        if (provider != "zerossl") {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "Let's Encrypt 只支持邮箱接入方式", 422);
        }
        if (!accessKey || accessKey->empty()) {
            service::common::throwAppError(CertificateProviderError::ACCESS_KEY_REQUIRED);
        }
        if (accountEmail) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "API Access Key 接入方式不能提交账户邮箱", 422);
        }
    }
};

inline CertificateProviderService& certificateProviderService() {
    static CertificateProviderService service;
    return service;
}

} // namespace service::provider
