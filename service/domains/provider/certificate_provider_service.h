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
#include "service/domains/certificate/certificate_reference.h"
#include "service/domains/provider/certificate_provider_error.h"
#include "service/domains/provider/certificate_provider_types.h"
#include "service/features/background/queued_work.h"
#include "service/features/certificate/provider_config_mapper.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/provider_verification/queue.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::provider {

class certificate_provider_service final {
public:
    ruvia::task<std::int64_t> create(ruvia::context& c, const std::string& tenant_id,
        const create_certificate_provider_body& body) {
        const auto& provider_input = body.get<"provider">();
        const auto& credential_mode_input = body.get<"credential_mode">();
        if (!provider_input || !credential_mode_input) {
            service::common::throw_app_error(service::common::validation_error_code,
                "证书供应商和接入方式不能为空", 400);
        }
        const auto provider = std::string(provider_input->view());
        const auto credential_mode = std::string(credential_mode_input->view());
        std::optional<std::string> account_email;
        if (const auto& value = body.get<"account_email">(); value) {
            account_email = normalize_email(value->view());
        }
        std::optional<std::string_view> access_key;
        if (const auto& value = body.get<"access_key">(); value) {
            access_key = value->view();
        }
        validate_credentials(provider, credential_mode, account_email, access_key);
        const auto config = service::certificate_issuance::serialize_certificate_provider_config(
            credential_mode, account_email,
            access_key ? std::optional<std::string>{service::utils::seal_secret(*access_key)}
                       : std::nullopt,
            access_key ? std::optional<std::string>{secret_hint(*access_key)} : std::nullopt,
            c.pool());
        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto inserted = co_await transaction.query(
                "INSERT INTO sys_provider (tenant_id, kind, provider, revision, config, "
                "runtime, status, created_at, updated_at) VALUES ($1, 'certificate', $2, 1, "
                "$3::jsonb, '{}'::jsonb, 'unverified', NOW(), NOW()) RETURNING id, revision",
                tenant_id, std::string_view(provider), std::string_view(config));
            const auto id = std::string(inserted.front()[0].value().value_or(""));
            co_await commit_changes(transaction, c, tenant_id, id, service::background::queued_work{});
            co_return inserted.front()[1].as<std::int64_t>().value_or(1);
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error,
                    "uk_provider_certificate_type")) {
                service::common::throw_app_error(certificate_provider_error::exists);
            }
            throw;
        }
    }

    ruvia::task<std::int64_t> update(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision, const update_certificate_provider_body& body) {
        const auto rows = co_await c.db().query(
            "SELECT revision, provider, config::text FROM sys_provider WHERE id = $1 AND "
            "tenant_id = $2 AND kind = 'certificate' AND deleted_at IS NULL LIMIT 1",
            id, tenant_id);
        service::common::require_current_revision(rows, expected_revision, revision_errors_);
        const auto provider = std::string(rows.front()[1].value().value_or(""));
        const auto current = service::certificate_issuance::parse_certificate_provider_config(
            rows.front()[2].value().value_or("{}"), c.pool());
        const auto& credential_mode_input = body.get<"credential_mode">();
        if (!credential_mode_input) {
            service::common::throw_app_error(service::common::validation_error_code,
                "证书接入方式不能为空", 400);
        }
        const auto credential_mode = std::string(credential_mode_input->view());
        std::optional<std::string> account_email;
        if (const auto& value = body.get<"account_email">(); value) {
            account_email = normalize_email(value->view());
        }
        std::optional<std::string_view> submitted_key;
        if (const auto& value = body.get<"access_key">(); value) {
            submitted_key = value->view();
        }
        std::optional<service::utils::sensitive_string> stored_key;
        std::optional<std::string_view> plain_key;
        std::optional<std::string> envelope;
        std::optional<std::string> hint;
        if (credential_mode == "access_key") {
            if (submitted_key) {
                plain_key = *submitted_key;
                envelope = service::utils::seal_secret(*submitted_key);
                hint = secret_hint(*submitted_key);
            } else if (current.credential_mode_ == "access_key" && current.envelope_) {
                envelope = current.envelope_;
                hint = current.hint_;
                stored_key.emplace(service::utils::open_secret(*current.envelope_));
                plain_key = stored_key->view();
            }
        }
        validate_credentials(provider, credential_mode, account_email, plain_key);
        const bool changed = credential_mode != current.credential_mode_ ||
                             account_email != current.account_email_ || submitted_key.has_value();
        const auto config = service::certificate_issuance::serialize_certificate_provider_config(
            credential_mode, account_email, envelope, hint, c.pool());
        auto transaction = co_await c.db().begin_transaction();
        const auto updated = co_await transaction.query(
            "UPDATE sys_provider SET revision = revision + 1, config = $1::jsonb, runtime = CASE "
            "WHEN $2::BOOLEAN THEN '{}'::jsonb ELSE runtime END, status = CASE WHEN $2::BOOLEAN "
            "THEN 'unverified' ELSE status END, last_verified_at = CASE WHEN $2::BOOLEAN THEN "
            "NULL ELSE last_verified_at END, verification_generation = verification_generation "
            "+ 1, last_error = NULL, updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND "
            "revision = $5 AND kind = 'certificate' AND deleted_at IS NULL RETURNING revision",
            std::string_view(config), changed, id, tenant_id, expected_revision);
        co_await service::common::require_revision_written(transaction, !updated.empty(), "sys_provider", tenant_id,
            id, revision_errors_);
        service::background::queued_work work;
        if (changed) {
            co_await service::provider_verification::mark_current(transaction, tenant_id, id, work);
        } else {
            co_await service::provider_verification::remove(transaction, tenant_id, id, work);
        }
        co_await commit_changes(transaction, c, tenant_id, id, work);
        co_return updated.front()[0].as<std::int64_t>().value_or(0);
    }

    // Verification leaves the revision unchanged; returns it.
    ruvia::task<std::int64_t> verify(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        service::background::queued_work work;
        auto transaction = co_await c.db().begin_transaction();
        const auto enqueued = co_await service::provider_verification::enqueue_certificate(transaction, tenant_id,
            id, expected_revision, work);
        co_await service::common::require_revision_written(transaction, enqueued, "sys_provider", tenant_id, id,
            revision_errors_);
        co_await commit_changes(transaction, c, tenant_id, id, work);
        co_return expected_revision;
    }

    ruvia::task<std::int64_t> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto transaction = co_await c.db().begin_transaction();
        const auto rows = co_await transaction.query(
            "SELECT revision FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
            "'certificate' AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenant_id);
        service::common::require_current_revision(rows, expected_revision, revision_errors_);
        if (co_await service::certificate::certificates_use_provider(transaction, tenant_id, id)) {
            service::common::throw_app_error(certificate_provider_error::in_use);
        }
        const auto removed = co_await transaction.query(
            "UPDATE sys_provider SET config = '{}'::jsonb, runtime = '{}'::jsonb, revision = "
            "revision + 1, verification_generation = verification_generation + 1, deleted_at = "
            "NOW(), updated_at = NOW() WHERE id = $1 AND tenant_id = $2 RETURNING revision",
            id, tenant_id);
        service::background::queued_work work;
        co_await service::provider_verification::remove(transaction, tenant_id, id, work);
        co_await commit_changes(transaction, c, tenant_id, id, work);
        co_return removed.front()[0].as<std::int64_t>().value_or(0);
    }

private:
    static constexpr service::common::revision_errors revision_errors_{certificate_provider_error::not_found,
        certificate_provider_error::revision_conflict};

    static ruvia::task<void> commit_changes(ruvia::db_transaction& transaction, ruvia::context& c, const std::string& tenant_id, const std::string& id,
        const service::background::queued_work& work) {
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.changed(service::live_resource::resource::providers, id);
        co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
    }

    static std::string trim(std::string_view input) {
        const auto begin = std::find_if_not(input.begin(), input.end(),
            [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }).base();
        return begin < end ? std::string(begin, end) : std::string{};
    }

    static std::string normalize_email(std::string_view input) {
        auto result = trim(input);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    static std::string secret_hint(std::string_view value) {
        constexpr std::size_t visible{4};
        return "****" +
               std::string(value.substr(value.size() > visible ? value.size() - visible : 0));
    }

    static void validate_credentials(std::string_view provider, std::string_view credential_mode,
        const std::optional<std::string>& account_email,
        const std::optional<std::string_view>& access_key) {
        if (credential_mode == "email") {
            if (!account_email || account_email->empty()) {
                service::common::throw_app_error(certificate_provider_error::email_required);
            }
            if (access_key) {
                service::common::throw_app_error(service::common::validation_error_code,
                    "邮箱接入方式不能提交 API Access Key", 422);
            }
            return;
        }
        if (provider != "zerossl") {
            service::common::throw_app_error(service::common::validation_error_code,
                "Let's Encrypt 只支持邮箱接入方式", 422);
        }
        if (!access_key || access_key->empty()) {
            service::common::throw_app_error(certificate_provider_error::access_key_required);
        }
        if (account_email) {
            service::common::throw_app_error(service::common::validation_error_code,
                "API Access Key 接入方式不能提交账户邮箱", 422);
        }
    }
};

inline certificate_provider_service& get_certificate_provider_service() {
    static certificate_provider_service service;
    return service;
}

}  // namespace service::provider
