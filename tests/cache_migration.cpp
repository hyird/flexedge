#include <iostream>

#include "service/config/schema.h"
#include "service/features/cache_config/mapper.h"
#include "service/features/initial_admin/bootstrap.h"
#include "tests/support/postgres_test.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

ruvia::task<void> verify_protected_default(ruvia::db_handle& db) {
    for (const auto* sql : {
             "UPDATE sys_cache_policy SET name='Renamed' WHERE is_builtin",
             "UPDATE sys_cache_policy SET deleted_at=NOW() WHERE is_builtin"}) {
        bool rejected{};
        try {
            (void)co_await db.execute(sql);
        } catch (const ruvia::db_error& error) {
            rejected = error.sql_state() == "23514";
        }
        require(rejected, "built-in policy cannot be renamed or deleted");
    }
}

void verify_existing_tenants(const ruvia::db_config& base) {
    const test_support::postgres_test_database database(base);
    const ruvia::db_migrator migrator(database.config(), {.table_ = "schema_migrations"});
    auto migrations = service::config::schema_migrations;
    std::erase_if(migrations, [](const auto& migration) { return migration.id() >= "0004_default_cache_policy"; });
    (void)migrator.migrate(migrations);
    const auto custom = [] {
        auto config = service::cache_config::default_policy();
        config.memory_percent_ = 27;
        config.rules_[0].ttl_seconds_ = 3600;
        return ruvia::to_json(service::cache_config::to_dto(config));
    }();
    require(database.run([&](ruvia::db_client& client) -> ruvia::task<void> {
        co_await client.connect();
        auto db = client.handle();
        (void)co_await db.execute(
            "INSERT INTO sys_tenant(name,slug,deleted_at) VALUES "
            "('Existing','existing',NULL),('Conflict','conflict',NULL),('Deleted','deleted',NOW())");
        (void)co_await db.execute(
            "INSERT INTO sys_cache_policy(tenant_id,name,status,config) "
            "SELECT id,CASE WHEN slug='existing' THEN '自定义策略' ELSE $1 END,'disabled',$2::jsonb "
            "FROM sys_tenant WHERE slug IN ('existing','conflict')",
            service::cache_config::default_policy_name, custom);
    }) == 0,
        "prepare existing tenants");
    const auto applied = migrator.migrate(service::config::schema_migrations);
    require(applied.applied().size() == service::config::schema_migrations.size() - migrations.size(),
        "remaining migrations apply to existing installations");
    require(database.run([&](ruvia::db_client& client) -> ruvia::task<void> {
        co_await client.connect();
        auto db = client.handle();
        const auto rows = co_await db.query(
            "SELECT tenant.slug,policy.name,policy.status,policy.config::text,policy.is_builtin "
            "FROM sys_tenant tenant JOIN sys_cache_policy policy ON policy.tenant_id=tenant.id "
            "ORDER BY tenant.slug,policy.name");
        require(rows.size() == 3, "one default per live tenant, no duplicate name and no deleted tenant seed");
        unsigned seeded{};
        for (const auto& row : rows) {
            require(row[4].as<bool>().value() == (row[1].value().value() == service::cache_config::default_policy_name),
                "only the default policy is built-in, including an existing same-name policy");
            if (row[0].value().value() == "existing" && row[1].value().value() == service::cache_config::default_policy_name) {
                auto policy = service::cache_config::parse(row[3].value().value());
                require(policy.rules_.size() == 1 && policy.rules_[0].match_type_ == "extension" &&
                            policy.rules_[0].action_ == "cache" && policy.rules_[0].ttl_seconds_ == 86400,
                    "seeded policy only caches static resources for one day");
                require(policy.memory_percent_ == 10 && policy.disk_percent_ == 50,
                    "seeded policy uses percentage capacity limits");
                require(policy.rules_[0].skip_no_cache_ && !policy.rules_[0].respect_request_no_cache_,
                    "omitted request refresh flags follow the CF default");
                require(row[2].value().value() == "enabled", "default policy is available for website selection");
                ++seeded;
            } else {
                auto policy = service::cache_config::parse(row[3].value().value());
                require(policy.memory_percent_ == 27 && policy.rules_[0].ttl_seconds_ == 3600 &&
                            row[2].value().value() == "disabled",
                    "existing policies are unchanged");
            }
        }
        require(seeded == 1, "missing default policy was created");
        co_await verify_protected_default(db);
        (void)co_await db.execute(
            "UPDATE sys_cache_policy SET description='调整后的默认策略',status='disabled',revision=revision+1,config=$1::jsonb "
            "WHERE tenant_id=(SELECT id FROM sys_tenant WHERE slug='existing') AND name=$2",
            custom, service::cache_config::default_policy_name);
        (void)co_await db.execute(
            "UPDATE sys_cache_policy SET deleted_at=NOW() WHERE NOT is_builtin");
    }) == 0,
        "verify and edit seeded policies");
    const auto repeated_migration = migrator.migrate(service::config::schema_migrations);
    require(repeated_migration.applied().empty(), "repeat migration is a no-op");
    require(database.run([&](ruvia::db_client& client) -> ruvia::task<void> {
        co_await client.connect();
        auto db = client.handle();
        const auto rows = co_await db.query("SELECT name,status,revision,config::text FROM sys_cache_policy WHERE description='调整后的默认策略'");
        require(rows.size() == 1 && rows[0][1].value().value() == "disabled" && rows[0][2].as<std::int64_t>().value() == 2,
            "edits to the default policy survive repeat startup migration");
        require(service::cache_config::parse(rows[0][3].value().value()).memory_percent_ == 27,
            "edited default configuration is preserved");
        const auto count = co_await db.query("SELECT count(*) FROM sys_cache_policy");
        require(count.front()[0].as<std::int64_t>().value() == 3, "default policies are not duplicated");
        const auto deleted = co_await db.query("SELECT count(*) FROM sys_cache_policy WHERE NOT is_builtin AND deleted_at IS NOT NULL");
        require(deleted.front()[0].as<std::int64_t>().value() == 1, "deleted custom policies are not recreated");
        auto transaction = co_await db.begin_transaction();
        const auto tenants = co_await transaction.query("INSERT INTO sys_tenant(name,slug) VALUES('Rollback','rollback') RETURNING id");
        const auto tenant = std::string(tenants.front()[0].value().value());
        co_await service::cache::create_default_policy(transaction, tenant);
        co_await service::cache::create_default_policy(transaction, tenant);
        const auto policies = co_await transaction.query("SELECT count(*) FROM sys_cache_policy WHERE tenant_id=$1", tenant);
        require(policies.front()[0].as<std::int64_t>().value() == 1, "initial policy creation is idempotent");
        co_await transaction.rollback();
        const auto tenants_after = co_await db.query("SELECT count(*) FROM sys_tenant WHERE slug='rollback'");
        require(tenants_after.front()[0].as<std::int64_t>().value() == 0, "tenant and default policy roll back together");
    }) == 0,
        "verify repeat and transaction rollback");
}

void verify_fresh_install(const ruvia::db_config& base) {
    const test_support::postgres_test_database database(base);
    const ruvia::db_migrator migrator(database.config(), {.table_ = "schema_migrations"});
    (void)migrator.migrate(service::config::schema_migrations);
    require(database.run([](ruvia::db_client& client) -> ruvia::task<void> {
        co_await service::initial_admin::initialize(client,
            service::initial_admin::initial_admin_config(service::utils::sensitive_string("cache-qa-password")));
        auto db = client.handle();
        const auto rows = co_await db.query("SELECT tenant_id,name,config::text,is_builtin FROM sys_cache_policy");
        require(rows.size() == 1 && rows.front()[1].value().value() == service::cache_config::default_policy_name,
            "fresh installation creates a default policy with its tenant");
        require(service::cache_config::parse(rows.front()[2].value().value()).rules_.size() == 1,
            "fresh installation policy has default rules");
        require(service::cache_config::parse(rows.front()[2].value().value()).max_object_bytes_ == 512 * 1024 * 1024,
            "fresh installation uses the Cloudflare 512MiB object limit");
        require(service::cache_config::parse(rows.front()[2].value().value()).memory_percent_ == 50,
            "fresh installation defaults to 50 percent L1 memory");
        require(rows.front()[3].as<bool>().value(), "fresh installation policy is built-in");
        co_await verify_protected_default(db);
        (void)co_await db.execute("UPDATE sys_cache_policy SET description='保留用户修改',status='disabled'");
    }) == 0,
        "verify first-start bootstrap");
    require(database.run([](ruvia::db_client& client) -> ruvia::task<void> {
        co_await service::initial_admin::initialize(client, std::nullopt);
        auto db = client.handle();
        const auto repeated = co_await db.query("SELECT description,status='disabled' FROM sys_cache_policy");
        require(repeated.size() == 1 && repeated.front()[0].value().value() == "保留用户修改" &&
                    repeated.front()[1].as<bool>().value(),
            "startup preserves edits to the default policy");
    }) == 0,
        "verify repeat startup preserves edits");
}

void verify_cache_reason_rollback(const ruvia::db_config& base) {
    const test_support::postgres_test_database database(base);
    const ruvia::db_migrator migrator(database.config(), {.table_ = "schema_migrations"});
    auto previous = service::config::schema_migrations;
    std::erase_if(previous, [](const auto& migration) { return migration.id() > "0005_access_log_cache_reason"; });
    const auto migration = previous.back();
    previous.pop_back();
    (void)migrator.migrate(previous);
    require(database.run([&](ruvia::db_client& client) -> ruvia::task<void> {
        co_await client.connect();
        auto db = client.handle();
        auto transaction = co_await db.begin_transaction();
        (void)co_await transaction.execute(migration.sql());
        co_await transaction.rollback();
        const auto columns = co_await db.query("SELECT count(*) FROM information_schema.columns WHERE table_schema='public' AND table_name='sys_website_access_log' AND column_name='cache_reason'");
        require(columns.front()[0].as<std::int64_t>().value() == 0, "cache-reason DDL rolls back atomically");
        const auto constraint = co_await db.query("SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid='public.sys_website_access_log'::regclass AND conname='sys_website_access_log_cache_status_check'");
        require(constraint.size() == 1 && constraint.front()[0].value().value().find("revalidated") == std::string_view::npos, "cache-status constraint rolls back with its column");
    }) == 0,
        "cache-reason migration rollback");
    const auto applied = migrator.migrate(service::config::schema_migrations);
    require(applied.applied().size() == service::config::schema_migrations.size() - previous.size(),
        "remaining migrations apply after rollback");
    const auto repeated = migrator.migrate(service::config::schema_migrations);
    require(repeated.applied().empty(), "cache-reason migration repeats without changes");
}
}  // namespace

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
        const auto result = database.run([](ruvia::db_client& client) -> ruvia::task<void> {
            co_await client.connect();
            auto db = client.handle();
            auto transaction = co_await db.begin_transaction();
            const auto columns = co_await transaction.query("SELECT cache_policy_id FROM sys_website LIMIT 1");
            (void)columns;
            (void)co_await transaction.query("SELECT cache_status,cache_reason FROM sys_website_access_log LIMIT 1");
            co_await transaction.execute("INSERT INTO sys_tenant(name,slug) VALUES('Cache QA','cache-qa')");
            co_await transaction.execute("INSERT INTO sys_cache_policy(tenant_id,name,config) SELECT id,'Policy','{}' FROM sys_tenant");
            co_await transaction.rollback();
            const auto rows = co_await db.query("SELECT COUNT(*) FROM sys_cache_policy");
            if (rows.front()[0].as<std::int64_t>().value() != 0) {
                throw std::runtime_error("rollback failed");
            }
        });
        if (result != 0) {
            return result;
        }
        verify_existing_tenants(*base);
        verify_fresh_install(*base);
        verify_cache_reason_rollback(*base);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
