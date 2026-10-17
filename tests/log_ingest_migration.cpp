#include <ruvia/web/db/db_migration.h>

#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/config/schema.h"
#include "support/postgres_test.h"

namespace {

constexpr std::string_view tenant{"00000000-0000-4000-8000-000000000b01"};
constexpr std::string_view provider{"00000000-0000-4000-8000-000000000b02"};
constexpr std::string_view zone{"00000000-0000-4000-8000-000000000b03"};
constexpr std::string_view cluster{"00000000-0000-4000-8000-000000000b04"};
constexpr std::string_view node{"00000000-0000-4000-8000-000000000b05"};
constexpr std::string_view peer_node{"00000000-0000-4000-8000-000000000b06"};
constexpr std::string_view website{"00000000-0000-4000-8000-000000000b07"};
constexpr std::string_view event{"00000000-0000-4000-8000-000000000b08"};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("log ingest migration: " + std::string(message));
    }
}

void migrate(const test_support::postgres_test_database& database,
    std::span<const ruvia::db_migration> migrations, std::size_t applied) {
    const ruvia::db_migrator migrator(database.config(), {.table_ = "schema_migrations"});
    const auto report = migrator.migrate(migrations);
    require(report.applied().size() == applied, "expected number of migrations actually applied");
}

void require_rejected(const test_support::postgres_test_database& database,
    std::span<const ruvia::db_migration> migrations) {
    bool rejected{};
    try {
        migrate(database, migrations, 0);
    } catch (const ruvia::db_error&) {
        rejected = true;
    }
    require(rejected, "conflicting schema is rejected instead of silently accepted");
}

std::string preserved_logs;

ruvia::task<std::string> log_fingerprint(ruvia::db_handle db) {
    const auto rows = co_await db.query(
        "SELECT string_agg(value, ',' ORDER BY value) FROM ("
        "SELECT 'access:' || row_to_json(log)::text AS value FROM sys_website_access_log log "
        "UNION ALL SELECT 'node:' || row_to_json(log)::text FROM sys_node_log log) logs");
    co_return std::string(rows[0][0].value().value_or(""));
}

ruvia::task<void> insert_logs(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    (void)co_await db.execute("INSERT INTO sys_tenant(id,name,slug) VALUES($1,'Log migration QA','log-migration-qa')", tenant);
    (void)co_await db.execute("INSERT INTO sys_provider(id,tenant_id,kind,provider,name,account_id) VALUES($1,$2,'dns','cloudflare','QA','qa')", provider, tenant);
    (void)co_await db.execute("INSERT INTO sys_dns_zone(id,tenant_id,provider_id,domain) VALUES($1,$2,$3,'qa.example')", zone, tenant, provider);
    (void)co_await db.execute("INSERT INTO sys_cluster(id,tenant_id,dns_zone_id,name,hostname_prefix) VALUES($1,$2,$3,'QA','edge')", cluster, tenant, zone);
    (void)co_await db.execute(
        "INSERT INTO sys_node(id,tenant_id,cluster_id,name,agent_id,node_secret_hash,node_secret_envelope) "
        "VALUES($1,$3,$4,'QA one',repeat('a',32),repeat('b',64),'qa-only'),"
        "($2,$3,$4,'QA two',repeat('c',32),repeat('d',64),'qa-only')",
        node, peer_node, tenant, cluster);
    (void)co_await db.execute("INSERT INTO sys_website(id,tenant_id,cluster_id,config) VALUES($1,$2,$3,'{}')", website, tenant, cluster);
    // Nonunique identity indexes accept repeated events at distinct normalized
    // timestamps; rerunning the baseline must preserve all rows verbatim.
    for (const auto days : {30, 16, 0}) {
        const auto owner = days == 0 ? peer_node : node;
        (void)co_await db.execute(
            "INSERT INTO sys_website_access_log(tenant_id,occurred_at,id,node_id,website_id,protocol,method,host,target,status_code) "
            "VALUES($1,NOW()-make_interval(days=>$5),$2,$3,$4,'HTTP/2','GET','qa.example','/',200)",
            tenant, event, owner, website, days);
        (void)co_await db.execute(
            "INSERT INTO sys_node_log(tenant_id,occurred_at,id,node_id,level,category,message) "
            "VALUES($1,NOW()-make_interval(days=>$4),$2,$3,'info','qa','preserve existing log')",
            tenant, event, owner, days);
    }
    preserved_logs = co_await log_fingerprint(db);
    require(!preserved_logs.empty(), "logs exist in multiple time chunks");
}

ruvia::task<void> verify_indexes(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto indexes = co_await db.query(
        "SELECT relation.relname,indexed.indisunique,indexed.indisvalid,indexed.indisready,"
        "array_to_string(ARRAY(SELECT attribute.attname FROM unnest(indexed.indkey) WITH ORDINALITY key(attnum,position) "
        "JOIN pg_attribute attribute ON attribute.attrelid=indexed.indrelid AND attribute.attnum=key.attnum "
        "ORDER BY key.position),','),owner.relname "
        "FROM pg_index indexed JOIN pg_class relation ON relation.oid=indexed.indexrelid "
        "JOIN pg_class owner ON owner.oid=indexed.indrelid "
        "WHERE relation.relnamespace='public'::regnamespace AND relation.relname IN "
        "('idx_website_access_log_event','idx_node_log_event') ORDER BY relation.relname");
    require(indexes.size() == 2, "both stable identity indexes exist");
    for (const auto& row : indexes) {
        require(row[1].as<bool>() == false && row[2].as<bool>() == true && row[3].as<bool>() == true,
            "lookup indexes are nonunique, valid and ready");
        require(row[4].value() == "tenant_id,node_id,id", "stable identity column order is exact");
        require(row[5].value() == (row[0].value() == "idx_node_log_event" ? "sys_node_log" : "sys_website_access_log"),
            "index belongs to the intended table");
    }
    const auto primary = co_await db.query(
        "SELECT count(*) FROM pg_constraint WHERE contype='p' AND conrelid IN "
        "('sys_node_log'::regclass,'sys_website_access_log'::regclass) "
        "AND pg_get_constraintdef(oid)='PRIMARY KEY (tenant_id, occurred_at, id)'");
    require(primary[0][0].as<int>() == 2, "Timescale primary keys remain unchanged");
    const auto hypertables = co_await db.query(
        "SELECT count(*) FROM timescaledb_information.dimensions WHERE hypertable_schema='public' "
        "AND hypertable_name IN ('sys_node_log','sys_website_access_log') "
        "AND column_name='occurred_at' AND time_interval=INTERVAL '7 days'");
    require(hypertables[0][0].as<int>() == 2, "hypertables keep their original time dimension and interval");
    const auto records = co_await db.query("SELECT count(*) FROM schema_migrations WHERE migration_id='0001_initial'");
    require(records[0][0].as<int>() == 1, "the installation baseline is recorded exactly once");
    const auto shape = co_await db.query(
        "SELECT (SELECT count(*) FROM information_schema.columns WHERE table_schema='public' "
        "AND table_name='sys_dns_zone' AND column_name='next_check_at'), "
        "(SELECT count(*) FROM information_schema.columns WHERE table_schema='public' "
        "AND table_name='sys_sync_event' AND column_name='next_attempt_at'), "
        "(SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname='ck_sync_task_operation')");
    require(shape[0][0].as<int>() == 1 && shape[0][1].as<int>() == 0 &&
                shape[0][2].value().value().find("sync_local") == std::string_view::npos &&
                shape[0][2].value().value().find("sync_remote") == std::string_view::npos,
        "the baseline directly creates the final task and DNS schema");
}

ruvia::task<void> verify_preserved(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    require(co_await log_fingerprint(db) == preserved_logs, "rerunning the baseline preserves log fields and timestamps");
    const auto chunks = co_await db.query(
        "SELECT count(*) FROM timescaledb_information.chunks WHERE hypertable_schema='public' "
        "AND hypertable_name IN ('sys_node_log','sys_website_access_log')");
    require(chunks[0][0].as<int>().value_or(0) >= 6, "upgrade covers existing chunks of both tables");
    const auto missing = co_await db.query(
        "SELECT count(*) FROM timescaledb_information.chunks chunk WHERE hypertable_schema='public' "
        "AND hypertable_name IN ('sys_node_log','sys_website_access_log') AND NOT EXISTS "
        "(SELECT 1 FROM pg_indexes indexed WHERE indexed.schemaname=chunk.chunk_schema "
        "AND indexed.tablename=chunk.chunk_name AND indexed.indexdef LIKE '%(tenant_id, node_id, id)%')");
    require(missing[0][0].as<int>() == 0, "stable identity indexes also cover every existing time chunk");
}

ruvia::task<void> create_conflict(ruvia::db_client& client) {
    co_await client.connect();
    (void)co_await client.handle().execute("CREATE TABLE qa_existing (value text)");
    (void)co_await client.handle().execute("INSERT INTO qa_existing VALUES ('preserve me')");
    (void)co_await client.handle().execute("CREATE INDEX idx_node_log_event ON qa_existing(value)");
}

ruvia::task<void> verify_rollback(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto state = co_await db.query(
        "SELECT to_regclass('public.idx_website_access_log_event') IS NULL,"
        "(SELECT indrelid='qa_existing'::regclass FROM pg_index WHERE indexrelid='idx_node_log_event'::regclass),"
        "(SELECT count(*) FROM schema_migrations WHERE migration_id='0001_initial'),"
        "to_regclass('public.sys_tenant') IS NULL, (SELECT value FROM qa_existing)");
    require(state[0][0].as<bool>() == true && state[0][1].as<bool>() == true && state[0][2].as<int>() == 0,
        "index conflict rolls back the initial schema and its migration receipt");
    require(state[0][3].as<bool>() == true && state[0][4].value() == "preserve me",
        "failed baseline leaves the preexisting object and its data unchanged");
    (void)co_await db.execute("DROP INDEX idx_node_log_event");
}

}  // namespace

int main() {
    try {
        const auto config = test_support::isolated_postgres_config();
        if (!config) {
            return 77;
        }
        const auto& migrations = service::config::schema_migrations;
        require(!migrations.empty() && migrations[0].id() == "0001_initial",
            "fresh installations start with the final-schema baseline");
        const test_support::postgres_test_database fresh(*config), conflict(*config);
        migrate(fresh, migrations, migrations.size());
        require(fresh.run(verify_indexes) == 0, "fresh install");
        require(fresh.run(insert_logs) == 0 && fresh.run(verify_preserved) == 0,
            "chunks created after installation inherit the identity indexes");
        migrate(fresh, migrations, 0);
        require(fresh.run(verify_indexes) == 0 && fresh.run(verify_preserved) == 0, "repeat installation preserves data");
        require(conflict.run(create_conflict) == 0, "prepare an isolated conflicting object");
        require_rejected(conflict, migrations);
        require(conflict.run(verify_rollback) == 0, "failed installation rolls back atomically");
        migrate(conflict, migrations, migrations.size());
        require(conflict.run(verify_indexes) == 0, "retry the same baseline after removing conflict");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Final installation baseline: log indexes, task schema, repeat, conflict rejection and rollback passed\n";
    return 0;
}
