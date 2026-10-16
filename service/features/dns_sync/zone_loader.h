#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "service/features/background/task_context.h"
#include "service/features/dns/driver.h"
#include "service/features/dns/provider_config_mapper.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/task.h"
#include "service/features/sync_runtime/state.h"

namespace service::dns_sync::detail {

struct zone_sync_state final {
    zone_runtime_data runtime_;
    zone_config_data config_;
    zone_config_data desired_;
    // Desired records projected from hosted website domains (their CNAMEs).
    std::unordered_set<std::string> website_record_ids_;
    service::dns::dns_provider_config_data provider_config_;
    std::string domain_;
    std::string provider_;
    std::string account_id_;
    std::int64_t revision_;
    std::int64_t config_revision_;
    bool deleted_;
};

struct remote_zone_data final {
    std::string id_;
    std::vector<service::dns::provider_record> records_;
    std::vector<service::dns::provider_line> lines_;
};

inline ruvia::task<std::optional<zone_sync_state>>
load_zone_sync_state(service::background::task_context& context, const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease) {
    auto db = context.db();
    if (!co_await service::sync_runtime::renew_running_lease(db, lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    const auto rows = co_await db.query(
        "SELECT zone.desired_revision, zone.domain, zone.runtime::text, zone.deleted_at IS NOT "
        "NULL, provider.provider, provider.account_id, provider.config::text, provider.status, "
        "zone.config::text, zone.revision FROM "
        "sys_dns_zone zone INNER "
        "JOIN sys_provider provider ON provider.tenant_id = zone.tenant_id AND provider.id "
        "= zone.provider_id AND provider.kind = 'dns' WHERE zone.id = $1 AND zone.tenant_id = "
        "$2 AND provider.deleted_at IS NULL LIMIT 1",
        task.resource_id_, task.tenant_id_);
    if (rows.empty()) {
        throw std::runtime_error("DNS 聚合根或服务商不存在");
    }
    const auto& row = rows.front();
    const auto revision = row[0].as<std::int64_t>().value_or(task.version_);
    if (revision != task.version_) {
        auto transaction = co_await db.begin_transaction();
        (void)co_await service::sync_runtime::release_running(transaction, lease);
        co_await transaction.commit();
        co_return std::nullopt;
    }

    const auto runtime =
        parse_stored_runtime(row[2].value().value_or("{}"), {.resource_ = context.pool()});
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }
    const bool deleted = row[3].as<bool>().value_or(false) ||
                         task.operation_ == service::sync_runtime::marker_operation::remove;
    const std::string provider = std::string(row[4].value().value_or(""));
    const std::string account_id = std::string(row[5].value().value_or(""));
    auto provider_config =
        service::dns::parse_dns_provider_config(row[6].value().value_or("{}"), context.pool());
    if (row[7].value().value_or("") != "verified") {
        throw std::runtime_error("DNS 服务商账号尚未通过检测");
    }

    const std::optional<zone_config_data> config =
        parse_stored(row[8].value().value_or("{}"), {.resource_ = context.pool()});
    if (!config) {
        throw std::runtime_error("DNS 聚合配置损坏");
    }

    zone_config_data desired = *config;
    std::unordered_set<std::string> website_record_ids;
    const auto projected =
        co_await load_projected_records(db, task.tenant_id_, task.resource_id_);
    for (const auto& record : projected) {
        if (record.source_ == snapshot_record_source::website) {
            website_record_ids.emplace(record.id_);
        }
        desired.records_.push_back({.id_ = record.id_,
            .type_ = record.type_,
            .name_ = record.name_,
            .content_ = record.content_,
            .ttl_ = record.ttl_,
            .priority_ = record.priority_,
            .proxied_ = record.proxied_,
            .line_code_ = record.line_code_});
    }
    for (const auto& challenge : runtime->challenge_records_) {
        desired.records_.push_back({.id_ = challenge.id_,
            .type_ = "TXT",
            .name_ = challenge.name_,
            .content_ = challenge.content_,
            .ttl_ = challenge.ttl_,
            .priority_ = std::nullopt,
            .proxied_ = false,
            .line_code_ = "default"});
    }

    co_return zone_sync_state{
        .runtime_ = std::move(*runtime),
        .config_ = std::move(*config),
        .desired_ = std::move(desired),
        .website_record_ids_ = std::move(website_record_ids),
        .provider_config_ = std::move(provider_config),
        .domain_ = std::string(row[1].value().value_or("")),
        .provider_ = provider,
        .account_id_ = account_id,
        .revision_ = revision,
        .config_revision_ = row[9].as<std::int64_t>().value_or(0),
        .deleted_ = deleted,
    };
}

inline ruvia::task<remote_zone_data>
load_remote_zone_data(service::background::task_context& context, service::attempt_trace::recorder& trace,
    const service::sync_runtime::running_marker_lease& lease,
    const service::dns::dns_provider_driver& driver, std::string_view account_id,
    std::string_view secret, std::string_view domain) {
    using service::attempt_trace::step_code;
    auto find_step = trace.begin_step(step_code::find_zone, domain);
    const auto remote_zone = co_await driver.find_zone(context, trace, account_id, secret, domain);
    auto db = context.db();
    if (!co_await service::sync_runtime::renew_running_lease(db, lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    trace.log(service::attempt_trace::log_level::info, "找到托管域 " + remote_zone.name_);
    find_step.complete();
    auto records_step = trace.begin_step(step_code::list_records, domain);
    auto records = co_await driver.list_records(context, trace, account_id, secret, remote_zone.id_, domain);
    if (!co_await service::sync_runtime::renew_running_lease(db, lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    trace.log(service::attempt_trace::log_level::info, "服务商现有 " + std::to_string(records.size()) + " 条记录");
    records_step.complete();
    auto lines_step = trace.begin_step(step_code::list_lines, domain);
    auto lines = co_await driver.list_lines(context, trace, account_id, secret, domain);
    if (driver.supports_routing_lines()) {
        trace.log(service::attempt_trace::log_level::info, "可用解析线路 " + std::to_string(lines.size()) + " 条");
        lines_step.complete();
    } else {
        lines_step.skip("该服务商只有默认线路，无需查询");
    }
    co_return remote_zone_data{
        .id_ = remote_zone.id_,
        .records_ = std::move(records),
        .lines_ = std::move(lines),
    };
}

}  // namespace service::dns_sync::detail
