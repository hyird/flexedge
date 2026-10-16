#include <ruvia/web/db/db_migration.h>

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "service/config/schema.h"
#include "service/features/dns_sync/maintenance.h"
#include "service/features/dns_sync/zone_persistence.h"
#include "support/postgres_test.h"
#include "support/recorded_notifications.h"

namespace {
constexpr std::string_view tenant{"01000000-0000-0000-0000-000000000001"};
constexpr std::string_view provider{"01000000-0000-0000-0000-000000000002"};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ruvia::task<void> verify(ruvia::db_client& client, ruvia::db_client& peer) {
    co_await client.connect();
    co_await peer.connect();
    auto db = client.handle();
    test_support::recorded_notifications first{.handle_ = db, .lease_owner_ = "first"};
    test_support::recorded_notifications second{.handle_ = peer.handle(), .lease_owner_ = "second"};
    (void)co_await db.execute("INSERT INTO sys_tenant(id, name, slug) VALUES ($1, 'DNS QA', 'dns-qa')", tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, kind, provider, name, account_id, status) "
        "VALUES ($1, $2, 'dns', 'cloudflare', 'DNS', 'qa', 'verified')",
        provider, tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(tenant_id, provider_id, domain, sync_status, synced_revision, config, runtime, next_check_at) "
        "SELECT $1::uuid, $2::uuid, 'zone-' || n || '.example.com', 'synced', 1, '{\"records\":[]}'::jsonb, "
        "'{\"lines\":[],\"record_states\":[]}'::jsonb, NOW() - n * INTERVAL '1 minute' FROM generate_series(1, 150) n",
        tenant, provider);
    (void)co_await service::dns_sync::detail::reconcile_tasks(first);
    require(!first.own_claims_.empty(), "missing tasks wake only the creating worker");
    // Drain the bounded missing-task recovery beyond the old 128-zone limit.
    for (int batch = 0; batch < 4; ++batch) {
        co_await service::dns_sync::detail::reconcile_tasks(first);
    }
    const auto all = co_await db.query("SELECT count(*) FROM sys_sync_task WHERE resource_type = 'dns_zone'");
    require(all.front()[0].as<int>().value_or(0) == 150, "all zones recover their one task");
    (void)co_await db.execute("UPDATE sys_sync_task SET is_done = true, is_ok = true, processed_version = version");
    const auto a = co_await service::dns_sync::detail::claim_due_check(first);
    const auto b = co_await service::dns_sync::detail::claim_due_check(second);
    require(a && b && a->resource_id_ != b->resource_id_, "concurrent workers never share an inspection");
    const auto selected = co_await db.query("SELECT domain FROM sys_dns_zone WHERE id = $1", a->resource_id_);
    require(selected.front()[0].value().value_or("") == "zone-150.example.com", "oldest due zone wins beyond 128");
    const auto unchanged = co_await db.query("SELECT is_done, is_ok FROM sys_sync_task WHERE id = $1", a->id_);
    require(unchanged.front()[0].as<bool>().value_or(false) && unchanged.front()[1].as<bool>().value_or(false),
        "inspection does not reset a settled task");
    service::dns_sync::zone_snapshot stable;
    const auto stable_runtime = ruvia::to_json(service::dns_sync::to_output(stable.runtime_));
    (void)co_await db.execute("UPDATE sys_dns_zone SET runtime = $2::jsonb WHERE id = $1", a->resource_id_,
        std::string_view(stable_runtime));
    const auto notified = first.sent_.size();
    service::attempt_trace::recorder quiet_trace(service::attempt_trace::trace_kind::dns_sync, "web", a->claimed_at_);
    const auto lease_a = service::sync_runtime::make_running_lease(tenant, a->id_, 1, first.lease_owner(), a->claimed_at_,
        service::sync_runtime::marker_resource_type::dns_zone, a->resource_id_);
    co_await service::dns_sync::detail::persist_synced_zone(first, quiet_trace, *a, lease_a, stable, 1, 1, "",
        service::sync_runtime::detail_none_json, false);
    const auto quiet = co_await db.query("SELECT (SELECT count(*) FROM sys_sync_event), next_check_at > NOW() + INTERVAL '9 minutes' FROM sys_dns_zone WHERE id = $1", a->resource_id_);
    require(quiet.front()[0].as<int>().value_or(-1) == 0 && quiet.front()[1].as<bool>().value_or(false) &&
                first.sent_.size() == notified,
        "unchanged inspection only advances its schedule, with no history or notification");
    stable.records_.records_.push_back({.id_ = "01000000-0000-0000-0000-000000000003", .type_ = "A", .name_ = "external", .content_ = "192.0.2.5", .ttl_ = 600, .priority_ = {}, .proxied_ = false, .line_code_ = "default"});
    service::attempt_trace::recorder changed_trace(service::attempt_trace::trace_kind::dns_sync, "web", b->claimed_at_);
    const auto lease_b = service::sync_runtime::make_running_lease(tenant, b->id_, 1, second.lease_owner(), b->claimed_at_,
        service::sync_runtime::marker_resource_type::dns_zone, b->resource_id_);
    co_await service::dns_sync::detail::persist_synced_zone(second, changed_trace, *b, lease_b, stable, 1, 1, "",
        service::sync_runtime::detail_none_json, false);
    const auto changed = co_await db.query("SELECT (SELECT count(*) FROM sys_sync_event), revision FROM sys_dns_zone WHERE id = $1", b->resource_id_);
    require(changed.front()[0].as<int>().value_or(0) == 1 && changed.front()[1].as<int>().value_or(0) == 2,
        "changed snapshot advances its edit revision and records one result");
    std::unordered_set<std::string> inspected{a->resource_id_, b->resource_id_};
    for (int index = 0; index < 148; ++index) {
        const auto next = co_await service::dns_sync::detail::claim_due_check(first);
        require(next && inspected.emplace(next->resource_id_).second, "every due zone is claimed once");
    }
    require(!co_await service::dns_sync::detail::claim_due_check(second), "no duplicate claim while all leases are held");
    // A provider snapshot loaded before a direct edit must never replace that edit.
    (void)co_await db.execute("UPDATE sys_dns_zone SET next_check_at = NOW() - INTERVAL '1 minute' WHERE id = $1", b->resource_id_);
    const auto stale = co_await service::dns_sync::detail::claim_due_check(second);
    require(stale.has_value(), "the settled zone can be checked again");
    (void)co_await db.execute("UPDATE sys_dns_zone SET revision = revision + 1, config = '{\"records\":[]}'::jsonb WHERE id = $1", b->resource_id_);
    const auto stale_lease = service::sync_runtime::make_running_lease(tenant, stale->id_, 1, second.lease_owner(), stale->claimed_at_,
        service::sync_runtime::marker_resource_type::dns_zone, stale->resource_id_);
    co_await service::dns_sync::detail::persist_synced_zone(second, changed_trace, *stale, stale_lease, stable, 1, 2, "",
        service::sync_runtime::detail_none_json, false);
    const auto protected_edit = co_await db.query(
        "SELECT revision, jsonb_array_length(config->'records'), (SELECT count(*) FROM sys_sync_event), "
        "(SELECT lease_until IS NULL FROM sys_sync_task WHERE id = $2) FROM sys_dns_zone WHERE id = $1",
        b->resource_id_, stale->id_);
    require(protected_edit.front()[0].as<int>().value_or(0) == 3 && protected_edit.front()[1].as<int>().value_or(-1) == 0 &&
                protected_edit.front()[2].as<int>().value_or(0) == 1 && protected_edit.front()[3].as<bool>().value_or(false),
        "a stale inspection releases its lease without overwriting a direct edit or adding history");
    const auto fresh = co_await service::dns_sync::detail::claim_due_check(second);
    require(fresh && fresh->resource_id_ == b->resource_id_, "a discarded snapshot can be checked again immediately");
    (void)co_await db.execute("UPDATE sys_sync_task SET lease_owner = NULL, lease_until = NULL, is_done = false, is_ok = false WHERE id = $1", a->id_);
    const auto live = co_await service::dns_sync::detail::claim_due_check(first);
    require(!live, "a normal queued task is never reset by inspection");
    (void)co_await db.execute("DELETE FROM sys_sync_task WHERE id = $1", a->id_);
    (void)co_await db.execute("UPDATE sys_dns_zone SET deleted_at = NOW(), desired_revision = 2 WHERE id = $1", a->resource_id_);
    co_await service::dns_sync::detail::reconcile_tasks(first);
    const auto deletion = co_await db.query("SELECT operation, version FROM sys_sync_task WHERE resource_id = $1", a->resource_id_);
    require(deletion.front()[0].value().value_or("") == "delete" && deletion.front()[1].as<int>().value_or(0) == 2,
        "lost deletion tasks are recovered with their own operation");
}

ruvia::task<void> verify_migration_rollback(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    auto transaction = co_await db.begin_transaction();
    (void)co_await transaction.execute(service::config::schema_migrations.back().sql());
    co_await transaction.rollback();
    const auto absent = co_await db.query("SELECT count(*) FROM information_schema.columns WHERE table_name = 'sys_dns_zone' AND column_name = 'next_check_at'");
    require(absent.front()[0].as<int>().value_or(-1) == 0, "candidate DDL rolls back atomically");
}
}  // namespace

int main(int argc, char** argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "--print-migration") {
        std::cout << service::config::schema_migrations.back().sql() << '\n';
        return 0;
    }
    const auto base = test_support::isolated_postgres_config();
    if (!base) {
        return 77;
    }
    const test_support::postgres_test_database database(*base);
    const ruvia::db_migrator migrator(database.config(), {.table_ = "schema_migrations"});
    const auto& migrations = service::config::schema_migrations;
    const auto baseline = migrator.migrate(std::span(migrations).first(migrations.size() - 1));
    require(baseline.applied().size() == migrations.size() - 1,
        "fresh baseline applies");
    if (database.run(verify_migration_rollback) != 0) {
        return 1;
    }
    const auto candidate = migrator.migrate(migrations);
    require(candidate.applied().size() == 1, "candidate migration applies after rollback");
    const auto repeated = migrator.migrate(migrations);
    require(repeated.applied().empty(), "migration is idempotent");
    return database.run(verify);
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
