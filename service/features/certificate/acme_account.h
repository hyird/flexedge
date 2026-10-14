#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/certificate/acme.h"
#include "service/features/certificate/provider_config_mapper.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance {

template <typename runtime_type>
ruvia::task<acme_account>
ensure_acme_account(runtime_type& context, std::string_view tenant_id, std::string_view provider_id,
    std::int64_t expected_revision, std::optional<std::string_view> account_email,
    const acme_client& client, const std::optional<eab_credentials>& eab) {
    const auto rows = co_await context.db().query(
        "SELECT revision, runtime::text FROM sys_provider WHERE tenant_id = $1 AND id = $2 AND "
        "kind = 'certificate' AND deleted_at IS NULL LIMIT 1",
        tenant_id, provider_id);
    if (rows.empty()) {
        throw acme_error("证书供应商不存在", true);
    }
    if (rows.front()[0].template as<std::int64_t>().value_or(0) != expected_revision) {
        throw acme_error("证书供应商配置已变更", false);
    }
    const auto runtime = parse_certificate_provider_runtime(rows.front()[1].value().value_or("{}"),
        {.resource_ = context.pool()});
    if (!runtime) {
        throw acme_error("证书供应商运行状态损坏", true);
    }
    acme_account account;
    std::string private_key_envelope;
    bool has_stored_private_key = false;
    if (runtime->acme_account_) {
        const auto& acme_account = *runtime->acme_account_;
        if (acme_account.private_key_envelope_) {
            has_stored_private_key = true;
            private_key_envelope = *acme_account.private_key_envelope_;
            account.private_key_pem_ =
                service::utils::sensitive_string(service::utils::open_secret(private_key_envelope));
            if (acme_account.account_url_) {
                account.account_url_ = *acme_account.account_url_;
            }
        }
    }
    if (!has_stored_private_key) {
        auto generated_private_key = client.generate_account_key();
        private_key_envelope = service::utils::seal_secret(generated_private_key.view());
        account.private_key_pem_ = std::move(generated_private_key);
    }
    if (account.account_url_.empty()) {
        account.account_url_ = co_await client.register_account(context, account.private_key_pem_.view(),
            account_email, eab);
    }

    auto transaction = co_await context.db().begin_transaction();
    const auto current_rows = co_await transaction.query(
        "SELECT revision, runtime::text FROM sys_provider WHERE tenant_id = $1 AND id = $2 "
        "AND kind = 'certificate' AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenant_id, provider_id);
    if (current_rows.empty() ||
        current_rows.front()[0].template as<std::int64_t>().value_or(0) != expected_revision) {
        throw acme_error("证书供应商配置已变更", false);
    }
    const auto current = parse_certificate_provider_runtime(
        current_rows.front()[1].value().value_or("{}"), {.resource_ = context.pool()});
    if (!current) {
        throw acme_error("证书供应商运行状态损坏", true);
    }
    if (current->acme_account_ && current->acme_account_->private_key_envelope_ &&
        current->acme_account_->account_url_) {
        acme_account persisted{
            .private_key_pem_ = service::utils::sensitive_string(
                service::utils::open_secret(*current->acme_account_->private_key_envelope_)),
            .account_url_ = *current->acme_account_->account_url_,
        };
        co_await transaction.commit();
        co_return persisted;
    }
    certificate_provider_runtime_output output = to_output(*current, {.resource_ = context.pool()});
    auto& acme_account = output.ensure<"acme_account">();
    acme_account.set<"private_key_envelope">(private_key_envelope);
    acme_account.set<"account_url">(account.account_url_);
    const auto runtime_json = ruvia::to_json(output, {.resource_ = context.pool()});
    const auto updated = co_await transaction.execute(
        "UPDATE sys_provider SET runtime = $1::jsonb, updated_at = NOW() WHERE tenant_id = "
        "$2 AND id = $3 AND kind = 'certificate' AND revision = $4 AND deleted_at IS NULL",
        std::string_view(runtime_json), tenant_id, provider_id, expected_revision);
    if (updated.affected_rows() == 0) {
        throw acme_error("证书供应商配置已变更", false);
    }
    co_await transaction.commit();
    co_return account;
}

}  // namespace service::certificate_issuance
