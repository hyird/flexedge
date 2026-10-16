#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/dns_zone/dns_zone_error.h"
#include "service/domains/dns_zone/dns_zone_types.h"
#include "service/features/dns_sync/line.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/live_resource/notification_outbox.h"

namespace service::dns_zone {

class dns_zone_command_service final {
public:
    ruvia::task<std::optional<std::string>> create(ruvia::context& c, const std::string& tenant_id,
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
        runtime.set<"records_imported">(false);
        (void)runtime.ensure<"lines">();
        (void)runtime.ensure<"record_states">();
        (void)runtime.ensure<"conflicts">();
        const auto runtime_json = ruvia::to_json(runtime, {.resource_ = c.pool()});

        try {
            auto transaction = co_await c.db().begin_transaction();
            const auto provider = co_await transaction.query(
                "SELECT status FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
                "'dns' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
                provider_id, tenant_id);
            if (provider.empty()) {
                service::common::throw_app_error(dns_zone_error::provider_not_found);
            }
            if (provider.front()[0].value().value_or("") != "verified") {
                service::common::throw_app_error(dns_zone_error::provider_unavailable);
            }

            const auto inserted = co_await transaction.query(
                "INSERT INTO sys_dns_zone (tenant_id, provider_id, domain, config, runtime, "
                "sync_status, revision, desired_revision, synced_revision, created_at, updated_at) "
                "VALUES ($1, $2, $3, $4::jsonb, $5::jsonb, 'pending', 1, 1, 0, NOW(), NOW()) "
                "RETURNING id, desired_revision",
                service::common::db_params(ruvia::db_value{tenant_id}, ruvia::db_value{provider_id},
                    ruvia::db_value{std::string_view(domain)},
                    ruvia::db_value{std::string_view(config_json)},
                    ruvia::db_value{std::string_view(runtime_json)}));
            const auto zone_id = std::string(inserted.front()[0].value().value_or(""));
            auto origin = start(c, service::operation::action::dns_zone_create, zone_id);
            (void)co_await origin.require_operation(transaction, tenant_id);
            co_await service::dns_sync::enqueue_zone_revision(transaction, tenant_id, zone_id,
                inserted.front()[1].as<std::int64_t>().value_or(1), origin);
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.changed(service::live_resource::resource::dns_zones, zone_id);
            origin.add_changes(notification);
            co_await service::live_resource::commit_and_notify(transaction, c, notification);
            co_return origin.operation_id();
        } catch (const ruvia::db_error& error) {
            if (service::common::is_unique_constraint_violation(error, "uk_dns_zone_domain")) {
                service::common::throw_app_error(dns_zone_error::exists);
            }
            throw;
        }
    }

    ruvia::task<std::optional<std::string>>
    update_config(ruvia::context& c, const std::string& tenant_id, const std::string& id,
        std::int64_t expected_revision,
        const service::dns_sync::zone_config_input& config) {
        auto transaction = co_await c.db().begin_transaction();
        const auto current = co_await transaction.query(
            "SELECT zone.revision, zone.runtime::text, provider.provider, zone.domain, zone.config::text FROM "
            "sys_dns_zone zone "
            "INNER JOIN sys_provider provider ON provider.tenant_id = zone.tenant_id AND "
            "provider.id = zone.provider_id WHERE zone.id = $1 AND zone.tenant_id = $2 AND "
            "zone.deleted_at IS NULL AND provider.deleted_at IS NULL LIMIT 1 FOR UPDATE OF zone",
            id, tenant_id);
        if (current.empty()) {
            service::common::throw_app_error(dns_zone_error::not_found);
        }
        if (current.front()[0].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(dns_zone_error::revision_conflict);
        }
        const auto normalized_config = service::dns_sync::normalize(config);
        if (!normalized_config) {
            service::common::throw_app_error(service::common::validation_error_code,
                "records 不能为空且每条记录必须完整", 400);
        }
        validate_record_lines(c, *normalized_config, current.front()[1].value().value_or("{}"));
        validate_record_ttls(*normalized_config, current.front()[2].value().value_or(""));
        co_await validate_system_managed_records(
            transaction, tenant_id, id, current.front()[3].value().value_or(""),
            current.front()[1].value().value_or("{}"), *normalized_config);
        const auto config_json = serialize_config(c, *normalized_config);
        auto origin = start(c, service::operation::action::dns_zone_update_records, id,
            co_await record_counts_summary(transaction, current.front()[4].value().value_or("{}"), config_json));
        const auto rows = co_await transaction.query(
            "UPDATE sys_dns_zone SET config = $1::jsonb, revision = revision + 1, "
            "desired_revision = desired_revision + 1, sync_status = 'pending', last_error = NULL, "
            "updated_at = NOW() WHERE id = $2 AND tenant_id = $3 AND revision = $4 AND "
            "deleted_at IS NULL RETURNING revision, desired_revision",
            service::common::db_params(ruvia::db_value{std::string_view(config_json)},
                ruvia::db_value{id}, ruvia::db_value{tenant_id},
                ruvia::db_value{expected_revision}));
        if (rows.empty()) {
            service::common::throw_app_error(dns_zone_error::revision_conflict);
        }
        const auto desired_revision = rows.front()[1].as<std::int64_t>().value_or(1);
        (void)co_await origin.require_operation(transaction, tenant_id);
        co_await service::dns_sync::enqueue_zone_revision(transaction, tenant_id, id, desired_revision, origin);
        co_await commit_changes(transaction, c, tenant_id, id, origin);
        co_return origin.operation_id();
    }

    ruvia::task<std::optional<std::string>> request_sync(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::string_view conflict_policy) {
        auto origin = start(c,
            conflict_policy == "remote"  ? service::operation::action::dns_zone_sync_remote
            : conflict_policy == "local" ? service::operation::action::dns_zone_sync_local
                                         : service::operation::action::dns_zone_sync,
            id,
            conflict_policy == "remote"  ? R"({"conflict_policy":"remote"})"
            : conflict_policy == "local" ? R"({"conflict_policy":"local"})"
                                         : "{}");
        auto transaction = co_await c.db().begin_transaction();
        const auto rows = co_await transaction.query(
            "UPDATE sys_dns_zone SET desired_revision = desired_revision + 1, sync_status = "
            "'pending', last_error = NULL, updated_at = NOW() WHERE id = $1 AND tenant_id = $2 "
            "AND deleted_at IS NULL RETURNING desired_revision",
            id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(dns_zone_error::not_found);
        }
        (void)co_await origin.require_operation(transaction, tenant_id);
        co_await service::dns_sync::enqueue_zone_revision(transaction, tenant_id, id,
            rows.front()[0].as<std::int64_t>().value_or(1), origin,
            conflict_policy == "remote"  ? service::sync_runtime::marker_operation::sync_remote
            : conflict_policy == "local" ? service::sync_runtime::marker_operation::sync_local
                                         : service::sync_runtime::marker_operation::sync);
        co_await commit_changes(transaction, c, tenant_id, id, origin);
        co_return origin.operation_id();
    }

    ruvia::task<std::optional<std::string>> remove(ruvia::context& c, const std::string& tenant_id,
        const std::string& id, std::int64_t expected_revision) {
        auto origin = start(c, service::operation::action::dns_zone_delete, id);
        auto transaction = co_await c.db().begin_transaction();
        const auto zone = co_await transaction.query(
            "SELECT revision, desired_revision FROM sys_dns_zone WHERE id = $1 AND tenant_id = "
            "$2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenant_id);
        if (zone.empty()) {
            service::common::throw_app_error(dns_zone_error::not_found);
        }
        if (zone.front()[0].as<std::int64_t>().value_or(0) != expected_revision) {
            service::common::throw_app_error(dns_zone_error::revision_conflict);
        }
        const auto references = co_await transaction.query(
            "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND dns_zone_id = $2 AND "
            "deleted_at IS NULL UNION ALL SELECT id FROM sys_cluster WHERE tenant_id = $1 AND "
            "dns_zone_id = $2 AND deleted_at IS NULL UNION ALL SELECT website_id FROM "
            "sys_website_domain_claim WHERE tenant_id = $1 AND dns_zone_id = $2 LIMIT 1",
            tenant_id, id);
        if (!references.empty()) {
            service::common::throw_app_error(dns_zone_error::in_use);
        }

        const auto revision = expected_revision + 1;
        const auto desired_revision = zone.front()[1].as<std::int64_t>().value_or(1) + 1;
        (void)co_await transaction.execute(
            "UPDATE sys_dns_zone SET revision = $1, desired_revision = $2, sync_status = "
            "'pending', "
            "deleted_at = NOW(), updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND "
            "revision = $5",
            revision, desired_revision, id, tenant_id, expected_revision);
        (void)co_await origin.require_operation(transaction, tenant_id);
        (void)co_await service::dns_sync::enqueue_zone_deletion(transaction, tenant_id, id, desired_revision,
            origin);
        co_await commit_changes(transaction, c, tenant_id, id, origin);
        co_return origin.operation_id();
    }

private:
    static service::operation::work_origin start(ruvia::context& c, service::operation::action action,
        const std::string& id, std::string summary = "{}") {
        return service::operation::work_origin::start({.action_ = action,
            .actor_id_ = service::middleware::current_principal(c).admin_id_,
            .target_type_ = "dns_zone",
            .target_id_ = id,
            .summary_ = std::move(summary)});
    }

    static ruvia::task<void> commit_changes(ruvia::db_transaction& transaction, ruvia::context& c, const std::string& tenant_id, const std::string& id,
        const service::operation::work_origin& origin) {
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.changed(service::live_resource::resource::dns_zones, id);
        origin.add_changes(notification);
        co_await service::live_resource::commit_and_notify(transaction, c, notification);
    }

    // `{"record_counts":{...}}`: records added, changed and removed by the
    // saved configuration, matched by record id.
    static ruvia::task<std::string> record_counts_summary(ruvia::db_transaction& transaction,
        std::string_view previous_json, std::string_view next_json) {
        const auto rows = co_await transaction.query(
            "WITH previous AS (SELECT record->>'id' AS id, record FROM "
            "jsonb_array_elements(COALESCE($1::jsonb->'records', '[]'::jsonb)) record), next AS (SELECT "
            "record->>'id' AS id, record FROM jsonb_array_elements(COALESCE($2::jsonb->'records', "
            "'[]'::jsonb)) record) SELECT count(*) FILTER (WHERE previous.id IS NULL), count(*) FILTER (WHERE "
            "previous.id IS NOT NULL AND next.id IS NOT NULL AND previous.record <> next.record), count(*) "
            "FILTER (WHERE next.id IS NULL) FROM previous FULL JOIN next ON previous.id = next.id",
            previous_json, next_json);
        const auto count = [&](std::size_t column) {
            return std::to_string(rows.front()[column].as<std::int64_t>().value_or(0));
        };
        co_return "{\"record_counts\":{\"created\":" + count(0) + ",\"updated\":" + count(1) +
            ",\"deleted\":" + count(2) + "}}";
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
        std::unordered_set<std::string> traffic_hostnames;
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
