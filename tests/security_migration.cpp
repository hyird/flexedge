#include <iostream>

#include "service/config/schema.h"
#include "service/domains/security/security_reference.h"
#include "tests/support/postgres_test.h"

#define FLEXEDGE_REQUIRE(value)               \
    do {                                      \
        if (!(value)) {                       \
            throw std::runtime_error(#value); \
        }                                     \
    } while (false)

int main() {
    try {
        const auto base = test_support::isolated_postgres_config();
        if (!base) {
            return 77;
        }
        const test_support::postgres_test_database database(*base);
        const auto migration = std::ranges::find_if(service::config::schema_migrations,
            [](const auto& value) { return value.id() == "0007_security_policy"; });
        FLEXEDGE_REQUIRE(migration != service::config::schema_migrations.end());
        const auto lists_migration = std::ranges::find_if(service::config::schema_migrations, [](const auto& value) { return value.id() == "0008_security_lists"; });
        FLEXEDGE_REQUIRE(lists_migration != service::config::schema_migrations.end());
        FLEXEDGE_REQUIRE(database.run([&](ruvia::db_client& client) -> ruvia::task<void> {
            co_await client.connect();
            auto db = client.handle();
            (void)co_await db.execute("CREATE TABLE sys_tenant(id uuid PRIMARY KEY)");
            (void)co_await db.execute("CREATE TABLE sys_website(id uuid PRIMARY KEY,tenant_id uuid NOT NULL,config jsonb NOT NULL,deleted_at timestamptz)");
            auto tx = co_await db.begin_transaction();
            (void)co_await tx.execute(migration->sql());
            (void)co_await tx.execute(lists_migration->sql());
            co_await tx.rollback();
            const auto absent = co_await db.query("SELECT to_regclass('public.sys_security_policy') IS NULL AND to_regclass('public.sys_security_list') IS NULL");
            FLEXEDGE_REQUIRE(absent.front()[0].as<bool>().value());
        }) == 0);
        const ruvia::db_migrator migrator(database.config(), {.table_ = "schema_migrations"});
        const std::vector<ruvia::db_migration> migrations{*migration, *lists_migration};
        const auto applied = migrator.migrate(migrations);
        FLEXEDGE_REQUIRE(applied.applied().size() == 2);
        const auto repeated = migrator.migrate(migrations);
        FLEXEDGE_REQUIRE(repeated.applied().empty());
        FLEXEDGE_REQUIRE(database.run([](ruvia::db_client& client) -> ruvia::task<void> {
            co_await client.connect();
            auto db = client.handle();
            const std::string first = "00000000-0000-4000-8000-000000000001", second = "00000000-0000-4000-8000-000000000002";
            (void)co_await db.execute("INSERT INTO sys_tenant(id) VALUES($1),($2)", first, second);
            (void)co_await db.execute("INSERT INTO sys_security_policy(tenant_id,name,config) VALUES($1,'Policy','{}'),($2,'Policy','{}')", first, second);
            const auto rows = co_await db.query("SELECT id FROM sys_security_policy WHERE tenant_id=$1", first);
            const auto id = std::string(rows.front()[0].value().value());
            FLEXEDGE_REQUIRE((co_await service::security::find_policy(db, first, id)).has_value());
            FLEXEDGE_REQUIRE(!(co_await service::security::find_policy(db, second, id)).has_value());
            bool isolated{};
            try {
                (void)co_await db.execute("INSERT INTO sys_website(id,tenant_id,config) VALUES(gen_random_uuid(),$1,jsonb_build_object('security_policy_id',$2::text))", second, id);
            } catch (const ruvia::db_error& error) {
                isolated = error.sql_state() == "23503";
            }
            FLEXEDGE_REQUIRE(isolated);
            (void)co_await db.execute("INSERT INTO sys_website(id,tenant_id,config) VALUES(gen_random_uuid(),$1,jsonb_build_object('security_policy_id',$2::text))", first, id);
            const auto selected = co_await db.query("SELECT security_policy_id FROM sys_website WHERE tenant_id=$1", first);
            FLEXEDGE_REQUIRE(selected.front()[0].value().value() == id);
            (void)co_await db.execute("INSERT INTO sys_security_list(tenant_id,name,kind,members) VALUES($1,'clients','ip','[]'),($2,'clients','ip','[]')", first, second);
            bool duplicate{};
            try {
                (void)co_await db.execute("INSERT INTO sys_security_list(tenant_id,name,kind) VALUES($1,'clients','ip')", first);
            } catch (const ruvia::db_error& error) {
                duplicate = error.sql_state() == "23505";
            }
            FLEXEDGE_REQUIRE(duplicate);
            const auto fields = co_await db.query("SELECT count(*) FROM information_schema.columns WHERE table_name='sys_security_policy' AND column_name IN ('enabled','status')");
            FLEXEDGE_REQUIRE(fields.front()[0].as<std::int64_t>().value() == 0);
        }) == 0);
        std::cout << "Security migration rollback, idempotence and tenant isolation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
