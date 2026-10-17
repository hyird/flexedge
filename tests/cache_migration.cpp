#include <iostream>

#include "service/config/schema.h"
#include "service/features/cache_config/mapper.h"
#include "tests/support/postgres_test.h"
int main() {
    try {
        const auto dto = service::cache_config::to_dto(flexedge::cache::policy{});
        if (!service::cache_config::parse(ruvia::to_json(dto)).rules_.empty()) {
            throw std::runtime_error("empty cache rules must round-trip");
        }
        const auto base = test_support::isolated_postgres_config();
        if (!base) {
            return 77;
        }
        const test_support::postgres_test_database database(*base);
        const ruvia::db_migrator migrator(database.config(), {.table_ = "schema_migrations"});
        const auto first = migrator.migrate(service::config::schema_migrations);
        if (first.applied().size() != service::config::schema_migrations.size()) {
            throw std::runtime_error("migrations not applied");
        }
        const auto second = migrator.migrate(service::config::schema_migrations);
        if (!second.applied().empty()) {
            throw std::runtime_error("migration is not idempotent");
        }
        return database.run([](ruvia::db_client& client) -> ruvia::task<void> {
            co_await client.connect();
            auto db = client.handle();
            auto transaction = co_await db.begin_transaction();
            const auto columns = co_await transaction.query("SELECT cache_policy_id FROM sys_website LIMIT 1");
            (void)columns;
            (void)co_await transaction.query("SELECT cache_status FROM sys_website_access_log LIMIT 1");
            co_await transaction.execute("INSERT INTO sys_tenant(name,slug) VALUES('Cache QA','cache-qa')");
            co_await transaction.execute("INSERT INTO sys_cache_policy(tenant_id,name,config) SELECT id,'Policy','{}' FROM sys_tenant");
            co_await transaction.rollback();
            const auto rows = co_await db.query("SELECT COUNT(*) FROM sys_cache_policy");
            if (rows.front()[0].as<std::int64_t>().value() != 0) {
                throw std::runtime_error("rollback failed");
            }
        });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
