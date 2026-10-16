#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/certificate/certificate_reference.h"
#include "service/domains/cluster/cluster_reference.h"
#include "service/domains/dns_zone/dns_zone_error.h"
#include "service/domains/dns_zone/dns_zone_types.h"
#include "service/domains/provider/provider_reference.h"
#include "service/domains/website/website_reference.h"
#include "service/features/background/queued_work.h"
#include "service/features/dns/driver.h"
#include "service/features/dns/provider_config_mapper.h"
#include "service/features/dns_sync/line.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/reconciliation.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::dns_zone {

class dns_zone_command_service final {
public:
    ruvia::task<std::int64_t> create(ruvia::context& c, const std::string& tenant_id,
        const create_dns_zone_body& body) {
        const auto& provider_id_input = body.get<"dns_provider_id">();
        const auto& domain_input = body.get<"domain">();
        if (!provider_id_input || !domain_input) {
            service::common::throw_app_error(service::common::validation_error_code,
                "DNS 服务商和域名不能为空", 400);
        }
        const auto provider_id = std::string(provider_id_input->view());
        const auto domain = normalize_domain(domain_input->view());
        const std::string config_json{R"json({"records":[]})json"};

        service::dns_sync::zone_runtime_dto runtime({.resource_ = c.pool()});
        (void)runtime.ensure<"lines">();
        (void)runtime.ensure<"record_states">();
        const auto runtime_json = ruvia::to_json(runtime, {.resource_ = c.pool()});

        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto provider = co_await service::provider::lock_dns_provider(transaction, tenant_id, provider_id);
            if (!provider) {
                service::common::throw_app_error(dns_zone_error::provider_not_found);
            }
            if (!provider->verified_) {
                service::common::throw_app_error(dns_zone_error::provider_unavailable);
            }

            const auto inserted = co_await transaction.query(
                "INSERT INTO sys_dns_zone (tenant_id, provider_id, domain, config, runtime, "
                "sync_status, revision, desired_revision, synced_revision, created_at, updated_at) "
                "VALUES ($1, $2, $3, $4::jsonb, $5::jsonb, 'pending', 1, 1, 0, NOW(), NOW()) "
                "RETURNING id, desired_revision, revision",
                service::common::db_params(ruvia::db_value{tenant_id}, ruvia::db_value{provider_id},
                    ruvia::db_value{std::string_view(domain)},
                    ruvia::db_value{std::string_view(config_json)},
                    ruvia::db_value{std::string_view(runtime_json)}));
            const auto zone_id = std::string(inserted.front()[0].value().value_or(""));
            service::background::queued_work work;
            co_await service::dns_sync::enqueue_zone_revision(transaction, tenant_id, zone_id,
                inserted.front()[1].as<std::int64_t>().value_or(1), work);
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.changed(service::live_resource::resource::dns_zones, zone_id);
            co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
            co_return inserted.front()[2].as<std::int64_t>().value_or(1);
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error, "uk_dns_zone_domain")) {
                service::common::throw_app_error(dns_zone_error::exists);
            }
            throw;
        }
    }

    ruvia::task<std::int64_t>
    update_config(ruvia::context& c, const std::string& tenant_id, const std::string& id,
        std::int64_t expected_revision, const service::dns_sync::zone_config_input& config) {
        auto transaction = co_await c.db().begin_transaction();
        const auto identity = co_await transaction.query(
            "SELECT provider_id FROM sys_dns_zone WHERE id = $1 AND tenant_id = $2 AND deleted_at IS NULL",
            id, tenant_id);
        if (identity.empty()) {
            service::common::throw_app_error(dns_zone_error::not_found);
        }
        // Provider commands lock the account before its zones. Use the same order.
        const auto provider = co_await service::provider::lock_dns_provider(
            transaction, tenant_id, identity.front()[0].value().value_or(""));
        const auto current = co_await transaction.query(
            "SELECT revision, runtime::text, provider_id, domain, config::text, desired_revision "
            "FROM sys_dns_zone WHERE id = $1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenant_id);
        service::common::require_current_revision(current, expected_revision, revision_errors_);
        const auto& row = current.front();
        const auto previous = service::dns_sync::parse_stored(row[4].value().value_or("{}"));
        auto runtime = service::dns_sync::parse_stored_runtime(row[1].value().value_or("{}"));
        const auto requested = service::dns_sync::normalize(config);
        if (!previous || !runtime) {
            throw_corrupt_config();
        }
        if (!requested) {
            service::common::throw_app_error(service::common::validation_error_code,
                "records 不能为空且每条记录必须完整", 400);
        }
        const auto changes = service::dns_sync::detail::plan_record_changes(*previous, *requested);
        if (changes.writes_.records_.empty() && changes.removals_.records_.empty()) {
            co_await transaction.commit();
            co_return expected_revision;
        }
        const auto provider_id = row[2].value().value_or("");
        if (!provider || !provider->verified_) {
            service::common::throw_app_error(dns_zone_error::provider_unavailable);
        }
        const auto domain = std::string(row[3].value().value_or(""));
        validate_record_lines(c, changes.writes_, row[1].value().value_or("{}"));
        validate_record_ttls(changes.writes_, provider->provider_);
        auto affected = changes.writes_;
        affected.records_.insert(affected.records_.end(), changes.removals_.records_.begin(), changes.removals_.records_.end());
        co_await validate_system_managed_records(transaction, tenant_id, id, domain,
            row[1].value().value_or("{}"), affected);
        const auto credentials = co_await transaction.query(
            "SELECT account_id, config::text FROM sys_provider WHERE tenant_id = $1 AND id = $2", tenant_id, provider_id);
        const auto account = std::string(credentials.front()[0].value().value_or(""));
        const auto provider_config = service::dns::parse_dns_provider_config(
            credentials.front()[1].value().value_or("{}"), c.pool());
        service::utils::sensitive_string secret(service::utils::open_secret(provider_config.credential_envelope_));
        service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::dns_sync, "web",
            std::chrono::steady_clock::now());
        trace.register_secret(secret.view());
        trace.register_secret(account);
        const service::dns::dns_provider_driver driver(provider->provider_, {.tenant_id_ = tenant_id, .account_id_ = account});
        const auto remote_zone = co_await driver.find_zone(c, trace, account, secret.view(), domain);
        auto remote = co_await driver.list_records(c, trace, account, secret.view(), remote_zone.id_, domain);
        auto remote_ids = service::dns_sync::detail::collect_remote_ids_by_local_id(*runtime);
        for (const auto& record : changes.removals_.records_) {
            const auto known = remote_ids.find(record.id_);
            if (known == remote_ids.end() || std::ranges::none_of(remote,
                                                 [&](const auto& item) { return item.id_ == known->second; })) {
                continue;
            }
            co_await driver.delete_record(c, trace, account, secret.view(), remote_zone.id_, known->second,
                record.name_ + " " + record.type_);
            std::erase_if(remote, [&](const auto& item) { return item.id_ == known->second; });
        }
        for (const auto& record : changes.writes_.records_) {
            const auto known = remote_ids.find(record.id_);
            const auto result = co_await driver.reconcile_record(c, trace, account, secret.view(), remote_zone.id_, domain,
                known == remote_ids.end() ? std::string_view{} : std::string_view{known->second},
                record.type_, record.name_, record.content_, record.ttl_, record.priority_, record.proxied_,
                record.line_code_, remote);
            auto observed = service::dns_sync::detail::to_remote_record(record, driver.record_names(), domain);
            observed.id_ = result.remote_id_;
            std::erase_if(remote, [&](const auto& item) { return item.id_ == observed.id_; });
            remote.push_back(std::move(observed));
            // Keep the request's UUID when the provider assigns its own id.
            std::erase_if(runtime->record_states_, [&](const auto& item) { return item.id_ == record.id_; });
            runtime->record_states_.push_back({.id_ = record.id_, .remote_record_id_ = result.remote_id_, .sync_status_ = "synced", .synced_revision_ = row[5].as<std::int64_t>().value_or(1), .last_error_ = {}, .managed_ = false});
        }
        const auto lines = co_await driver.list_lines(c, trace, account, secret.view(), domain);
        std::vector<service::dns_sync::zone_record_runtime_data> managed;
        for (const auto& state : runtime->record_states_) {
            if (state.managed_) {
                managed.push_back(state);
            }
        }
        const auto snapshot = service::dns_sync::capture_zone_snapshot(id, domain, driver.record_names(), remote,
            lines, *runtime, managed, row[5].as<std::int64_t>().value_or(1));
        const auto config_json = serialize_config(c, snapshot.records_);
        const auto runtime_json = ruvia::to_json(service::dns_sync::to_output(snapshot.runtime_), {.resource_ = c.pool()});
        (void)co_await transaction.execute(
            "UPDATE sys_dns_zone SET config = $3::jsonb, runtime = $4::jsonb, revision = revision + 1, "
            "updated_at = NOW() WHERE tenant_id = $1 AND id = $2",
            tenant_id, id, std::string_view(config_json), std::string_view(runtime_json));
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.changed(service::live_resource::resource::dns_zones, id);
        co_await service::live_resource::commit_and_notify(transaction, c, notification);
        co_return expected_revision + 1;
    }

    ruvia::task<void> request_sync(ruvia::context& c, const std::string& tenant_id, const std::string& id) {
        auto transaction = co_await c.db().begin_transaction();
        service::background::queued_work work;
        if (!co_await service::dns_sync::advance_zone_revision(transaction, tenant_id, id, {}, work)) {
            service::common::throw_app_error(dns_zone_error::not_found);
        }
        co_await commit_changes(transaction, c, tenant_id, id, work);
    }

    ruvia::task<std::int64_t> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto transaction = co_await c.db().begin_transaction();
        const auto zone = co_await transaction.query(
            "SELECT revision FROM sys_dns_zone WHERE id = $1 AND tenant_id = $2 AND deleted_at IS NULL "
            "LIMIT 1 FOR UPDATE",
            id, tenant_id);
        service::common::require_current_revision(zone, expected_revision, revision_errors_);
        if (co_await service::certificate::certificates_use_dns_zone(transaction, tenant_id, id) ||
            co_await service::cluster::clusters_use_dns_zone(transaction, tenant_id, id) ||
            co_await service::website::websites_claim_dns_zone(transaction, tenant_id, id)) {
            service::common::throw_app_error(dns_zone_error::in_use);
        }
        service::background::queued_work work;
        const auto revision = co_await service::dns_sync::advance_zone_revision(transaction, tenant_id, id,
            {.operation_ = service::sync_runtime::marker_operation::remove}, work);
        if (!revision) {
            throw std::logic_error("locked DNS zone was not advanced");
        }
        co_await commit_changes(transaction, c, tenant_id, id, work);
        co_return revision->revision_;
    }

private:
    static constexpr service::common::revision_errors revision_errors_{dns_zone_error::not_found,
        dns_zone_error::revision_conflict};

    static ruvia::task<void> commit_changes(ruvia::db_transaction& transaction, ruvia::context& c, const std::string& tenant_id, const std::string& id,
        const service::background::queued_work& work) {
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.changed(service::live_resource::resource::dns_zones, id);
        co_await service::sync_runtime::commit_work(transaction, c, std::move(notification), work);
    }

    static void validate_record_lines(ruvia::context& c,
        const service::dns_sync::zone_config_data& config,
        std::string_view runtime_json) {
        const auto runtime =
            service::dns_sync::parse_stored_runtime(runtime_json, {.resource_ = c.pool()});
        if (!runtime) {
            throw_corrupt_config();
        }
        std::unordered_set<std::string> enabled_lines;
        for (const auto& line : runtime->lines_) {
            if (!line.code_ || !line.status_) {
                throw_corrupt_config();
            }
            if (service::dns_sync::is_enabled_line(line)) {
                enabled_lines.emplace(*line.code_);
            }
        }
        for (const auto& record : config.records_) {
            if (!enabled_lines.contains(record.line_code_)) {
                service::common::throw_app_error(dns_zone_error::dns_line_invalid);
            }
        }
    }

    static void validate_record_ttls(const service::dns_sync::zone_config_data& config,
        std::string_view provider) {
        if (provider != "aliyun") {
            return;
        }
        for (const auto& record : config.records_) {
            if (record.ttl_ < 600) {
                service::common::throw_app_error(service::common::validation_error_code,
                    "阿里云 DNS 的 TTL 必须在 600 到 86400 秒之间", 422);
            }
        }
    }

    static ruvia::task<void>
    validate_system_managed_records(ruvia::db_transaction& transaction, const std::string& tenant_id,
        const std::string& zone_id, std::string_view zone_domain,
        std::string_view runtime_json,
        const service::dns_sync::zone_config_data& config) {
        const auto projected =
            co_await service::dns_sync::load_projected_records(transaction, tenant_id, zone_id);
        std::unordered_set<std::string> system_ids;
        auto traffic_hostnames = co_await service::dns_sync::load_cluster_hostnames(
            transaction, tenant_id, zone_id, zone_domain);
        system_ids.reserve(projected.size());
        traffic_hostnames.reserve(projected.size());
        for (const auto& record : projected) {
            system_ids.emplace(record.id_);
            if (service::dns_sync::is_traffic_record(record.type_)) {
                traffic_hostnames.emplace(
                    service::dns_sync::record_hostname(record.name_, zone_domain));
            }
        }

        const auto runtime = service::dns_sync::parse_stored_runtime(runtime_json);
        if (!runtime) {
            throw_corrupt_config();
        }
        for (const auto& state : runtime->record_states_) {
            if (state.managed_ && state.id_) {
                system_ids.emplace(*state.id_);
            }
        }
        std::unordered_set<std::string> challenge_hostnames;
        challenge_hostnames.reserve(runtime->challenge_records_.size());
        for (const auto& record : runtime->challenge_records_) {
            system_ids.emplace(record.id_);
            challenge_hostnames.emplace(service::dns_sync::record_hostname(record.name_, zone_domain));
        }
        for (const auto& record : config.records_) {
            const auto hostname = service::dns_sync::record_hostname(record.name_, zone_domain);
            if (system_ids.contains(record.id_) ||
                (service::dns_sync::is_traffic_record(record.type_) &&
                    traffic_hostnames.contains(hostname)) ||
                (record.type_ == "TXT" && challenge_hostnames.contains(hostname))) {
                service::common::throw_app_error(dns_zone_error::system_managed_record);
            }
        }
        co_return;
    }

    static std::string serialize_config(ruvia::context& c,
        const service::dns_sync::zone_config_data& input) {
        return std::string{
            ruvia::to_json(service::dns_sync::to_output(input, {.resource_ = c.pool()}),
                {.resource_ = c.pool()})};
    }

    [[noreturn]] static void throw_corrupt_config() {
        service::common::throw_app_error(service::common::server_error_code, "聚合配置损坏", 500);
    }

    static std::string normalize_domain(std::string_view input) {
        const auto begin = std::find_if_not(input.begin(), input.end(),
            [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }).base();
        std::string result = begin < end ? std::string(begin, end) : std::string{};
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }
};

inline dns_zone_command_service& get_dns_zone_command_service() {
    static dns_zone_command_service service;
    return service;
}

}  // namespace service::dns_zone
