#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/db/db_transaction.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "service/features/background/task_context.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/challenge_dns_entity.h"
#include "service/features/certificate/issuance_state.h"
#include "service/features/certificate/provider_config_mapper.h"
#include "service/features/certificate/task.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/mapper.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate_issuance::worker_detail {

struct challenge_dns_sync final {
    std::int64_t synced_revision_{};
    std::string task_id_{};
    std::int64_t version_{};
    bool done_{};
    bool ok_{};
    bool challenge_records_synced_{};
};

struct certificate_work final {
    std::string tenant_id_;
    std::string provider_id_;
    std::string provider_;
    std::int64_t provider_revision_;
    std::optional<std::string> account_email_;
    std::optional<eab_credentials> eab_;
    std::vector<std::string> domains_;
    std::string dns_zone_id_;
    std::string zone_domain_;
    std::int64_t dns_minimum_record_ttl_;
    // Progress a previous attempt of this issuance revision left to resume.
    std::optional<issuance_state> progress_;
};

// Read after locking the zone: its result writer locks the same row before
// transitioning the DNS marker and resuming waiters. A later park therefore
// cannot miss an already committed dependency result.
inline ruvia::task<challenge_dns_sync> load_challenge_dns(ruvia::db_transaction& transaction,
    const certificate_task& task, const certificate_work& work, std::pmr::memory_resource* resource) {
    ruvia::db_query expressions(resource);
    const auto equal = [&](ruvia::db_expression left, ruvia::db_expression right) {
        return expressions.binary(left, ruvia::db_binary_operator::equal, right);
    };
    const auto all = [&](ruvia::db_expression left, ruvia::db_expression right) {
        return expressions.binary(left, ruvia::db_binary_operator::and_value, right);
    };
    const auto tenant = equal(expressions.column("tenant_id", "zone"),
        expressions.cast(expressions.value(task.tenant_id_), ruvia::db_data_type::uuid));
    const auto id = equal(expressions.column("id", "zone"),
        expressions.cast(expressions.value(work.dns_zone_id_), ruvia::db_data_type::uuid));
    expressions.select(expressions.column("id", "zone"))
        .from("sys_dns_zone", "zone")
        .where(all(tenant, id))
        .and_where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "zone")))
        .lock({.mode_ = ruvia::db_row_lock::update});
    if ((co_await transaction.query(expressions)).empty()) {
        throw acme_error("DNS-01 托管域名不存在", true);
    }
    auto query = transaction.get_repository<challenge_zone_entity>().create_query_builder("zone");
    query.join<challenge_task_entity>(ruvia::db_join_type::left, "task",
             all(all(equal(expressions.column("tenant_id", "task"), expressions.column("tenant_id", "zone")),
                     equal(expressions.column("resource_type", "task"), expressions.value(std::string_view{"dns_zone"}))),
                 equal(expressions.column("resource_id", "task"), expressions.column("id", "zone"))))
        .where(all(tenant, id))
        .select({{"synced_revision", expressions.column("synced_revision", "zone")},
            {"runtime", expressions.cast(expressions.column("runtime", "zone"), ruvia::db_data_type::text)},
            {"task_id", expressions.cast(expressions.column("id", "task"), ruvia::db_data_type::text)},
            {"version", expressions.column("version", "task")},
            {"is_done", expressions.column("is_done", "task")}, {"is_ok", expressions.column("is_ok", "task")}});
    const auto row = co_await query.get_one<challenge_dns_projection>();
    if (!row) {
        throw acme_error("DNS-01 托管域名不存在", true);
    }
    const auto runtime = service::dns_sync::parse_stored_runtime(row->get<"runtime">(), {.resource_ = resource});
    if (!runtime) {
        throw acme_error("DNS-01 托管域名运行状态损坏", true);
    }
    std::size_t matched = 0;
    bool synced = true;
    for (const auto& challenge : runtime->challenge_records_) {
        if (challenge.certificate_id_ != task.certificate_id_) {
            continue;
        }
        ++matched;
        const auto state = std::ranges::find_if(runtime->record_states_, [&](const auto& value) {
            return value.id_ && *value.id_ == challenge.id_;
        });
        if (state == runtime->record_states_.end() || state->sync_status_ != "synced" ||
            state->synced_revision_.value_or(0) < work.progress_->dns_revision_) {
            synced = false;
        }
    }
    co_return challenge_dns_sync{.synced_revision_ = row->get<"synced_revision">(),
        .task_id_ = row->is_null<"task_id">() ? std::string{} : std::string(row->get<"task_id">()),
        .version_ = row->is_null<"version">() ? 0 : row->get<"version">(),
        .done_ = !row->is_null<"is_done">() && row->get<"is_done">(),
        .ok_ = !row->is_null<"is_ok">() && row->get<"is_ok">(),
        .challenge_records_synced_ = synced && matched != 0 && matched == work.progress_->authorizations_.size()};
}

inline ruvia::task<certificate_work> load_work(service::background::task_context& context,
    const certificate_task& task) {
    const auto rows = co_await context.db().query(
        "SELECT certificate.tenant_id, certificate.subject_alt_names[1], "
        "certificate.subject_alt_names[2], certificate.provider_id, certificate.dns_zone_id, "
        "provider.provider, provider.status, provider.revision, provider.config::text, "
        "provider.runtime::text, "
        "zone.domain, dns_provider.provider, dns_provider.status, certificate.issuance_state::text FROM "
        "sys_certificate certificate "
        "INNER JOIN "
        "sys_provider provider ON provider.tenant_id = certificate.tenant_id AND "
        "provider.id = certificate.provider_id AND provider.kind = 'certificate' INNER JOIN "
        "sys_dns_zone zone ON zone.tenant_id = certificate.tenant_id AND zone.id = "
        "certificate.dns_zone_id INNER JOIN sys_provider dns_provider ON "
        "dns_provider.tenant_id = zone.tenant_id AND dns_provider.id = zone.provider_id AND "
        "dns_provider.kind = 'dns' "
        "WHERE certificate.id = $1 AND certificate.issuance_revision = "
        "$2 AND certificate.tenant_id = $3 AND certificate.deleted_at IS NULL AND "
        "provider.deleted_at IS NULL AND zone.deleted_at IS "
        "NULL AND dns_provider.deleted_at IS NULL LIMIT 1",
        task.certificate_id_, task.version_, task.tenant_id_);
    if (rows.empty()) {
        throw acme_error("证书任务对应的资源不存在或版本已变化", true);
    }
    const auto& row = rows.front();
    if (row[6].value().value_or("") != "verified") {
        throw acme_error("证书供应商尚未通过检测", false);
    }
    if (row[12].value().value_or("") != "verified") {
        throw acme_error("DNS 服务商尚未通过检测", false);
    }

    certificate_provider_config_data provider_config;
    try {
        provider_config =
            parse_certificate_provider_config(row[8].value().value_or("{}"), context.pool());
    } catch (const std::runtime_error&) {
        throw acme_error("证书供应商配置损坏", true);
    }
    std::optional<std::string> account_email = provider_config.account_email_;

    std::optional<eab_credentials> eab;
    const auto provider = std::string(row[5].value().value_or(""));
    if (provider == "zerossl") {
        const auto runtime = parse_certificate_provider_runtime(row[9].value().value_or("{}"),
            {.resource_ = context.pool()});
        if (!runtime) {
            throw acme_error("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        const auto& eab_input = runtime->eab_;
        if (!eab_input) {
            throw acme_error("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        const auto& key_id = eab_input->kid_;
        const auto& hmac_key_envelope = eab_input->hmac_key_envelope_;
        if (!key_id || !hmac_key_envelope) {
            throw acme_error("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        eab = eab_credentials{
            *key_id,
            service::utils::sensitive_string(service::utils::open_secret(*hmac_key_envelope)),
        };
    }

    std::vector<std::string> domains;
    domains.reserve(2);
    // The generated SAN array already includes the certificate's primary domain.
    for (const auto index : {1U, 2U}) {
        const auto& value = row[index];
        if (const auto domain = value.value(); domain && !domain->empty()) {
            domains.emplace_back(*domain);
        }
    }
    if (domains.empty()) {
        throw acme_error("证书域名为空", true);
    }
    co_return certificate_work{
        .tenant_id_ = std::string(row[0].value().value_or("")),
        .provider_id_ = std::string(row[3].value().value_or("")),
        .provider_ = provider,
        .provider_revision_ = row[7].as<std::int64_t>().value_or(0),
        .account_email_ = std::move(account_email),
        .eab_ = std::move(eab),
        .domains_ = std::move(domains),
        .dns_zone_id_ = std::string(row[4].value().value_or("")),
        .zone_domain_ = std::string(row[10].value().value_or("")),
        .dns_minimum_record_ttl_ =
            service::dns::dns_provider_driver(row[11].value().value_or("")).minimum_record_ttl(),
        .progress_ = parse_issuance_state(row[13].value().value_or(""), task.version_, context.pool()),
    };
}

}  // namespace service::certificate_issuance::worker_detail
