#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>

#include "service/features/certificate/acme.h"
#include "service/features/certificate/provider_config_mapper.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance {

template <typename Runtime>
ruvia::Task<AcmeAccount>
ensureAcmeAccount(Runtime& context, std::string_view tenantId, std::string_view providerId,
                  std::int64_t expectedRevision, std::optional<std::string_view> accountEmail,
                  const AcmeClient& client, const std::optional<EabCredentials>& eab) {
    const auto rows = co_await context.db().query(
        "SELECT revision, runtime::text FROM sys_provider WHERE tenant_id = $1 AND id = $2 AND "
        "kind = 'certificate' AND deleted_at IS NULL LIMIT 1",
        tenantId, providerId);
    if (rows.empty()) {
        throw AcmeError("证书供应商不存在", true);
    }
    if (rows.front()[0].template as<std::int64_t>().value_or(0) != expectedRevision) {
        throw AcmeError("证书供应商配置已变更", false);
    }
    const auto runtime = parseCertificateProviderRuntime(rows.front()[1].value().value_or("{}"),
                                                         {.resource = context.resource()});
    if (!runtime) {
        throw AcmeError("证书供应商运行状态损坏", true);
    }
    AcmeAccount account;
    std::string privateKeyEnvelope;
    bool hasStoredPrivateKey = false;
    if (runtime->acmeAccount) {
        const auto& acmeAccount = *runtime->acmeAccount;
        if (acmeAccount.privateKeyEnvelope) {
            hasStoredPrivateKey = true;
            privateKeyEnvelope = *acmeAccount.privateKeyEnvelope;
            account.privateKeyPem =
                service::utils::SensitiveString(service::utils::openSecret(privateKeyEnvelope));
            if (acmeAccount.accountUrl) {
                account.accountUrl = *acmeAccount.accountUrl;
            }
        }
    }
    if (!hasStoredPrivateKey) {
        auto generatedPrivateKey = client.generateAccountKey();
        privateKeyEnvelope = service::utils::sealSecret(generatedPrivateKey.view());
        account.privateKeyPem = std::move(generatedPrivateKey);
    }
    if (account.accountUrl.empty()) {
        account.accountUrl = co_await client.registerAccount(context, account.privateKeyPem.view(),
                                                             accountEmail, eab);
    }

    auto transaction = co_await context.db().beginTransaction();
    const auto currentRows = co_await transaction.query(
        "SELECT revision, runtime::text FROM sys_provider WHERE tenant_id = $1 AND id = $2 "
        "AND kind = 'certificate' AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenantId, providerId);
    if (currentRows.empty() ||
        currentRows.front()[0].template as<std::int64_t>().value_or(0) != expectedRevision) {
        throw AcmeError("证书供应商配置已变更", false);
    }
    const auto current = parseCertificateProviderRuntime(
        currentRows.front()[1].value().value_or("{}"), {.resource = context.resource()});
    if (!current) {
        throw AcmeError("证书供应商运行状态损坏", true);
    }
    if (current->acmeAccount && current->acmeAccount->privateKeyEnvelope &&
        current->acmeAccount->accountUrl) {
        AcmeAccount persisted{
            .privateKeyPem = service::utils::SensitiveString(
                service::utils::openSecret(*current->acmeAccount->privateKeyEnvelope)),
            .accountUrl = *current->acmeAccount->accountUrl,
        };
        co_await transaction.commit();
        co_return persisted;
    }
    CertificateProviderRuntimeOutput output = toOutput(*current, {.resource = context.resource()});
    auto& acmeAccount = output.ensure<"acmeAccount">();
    acmeAccount.set<"privateKeyEnvelope">(privateKeyEnvelope);
    acmeAccount.set<"accountUrl">(account.accountUrl);
    const auto runtimeJson = ruvia::toJson(output, {.resource = context.resource()});
    const auto updated = co_await transaction.execute(
        "UPDATE sys_provider SET runtime = $1::jsonb, updated_at = NOW() WHERE tenant_id = "
        "$2 AND id = $3 AND kind = 'certificate' AND revision = $4 AND deleted_at IS NULL",
        std::string_view(runtimeJson), tenantId, providerId, expectedRevision);
    if (updated.affectedRows() == 0) {
        throw AcmeError("证书供应商配置已变更", false);
    }
    co_await transaction.commit();
    co_return account;
}

} // namespace service::certificate_issuance
