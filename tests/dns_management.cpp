#include <ruvia/web/app.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/db/db_migration.h>

#include <cstdlib>
#include <iostream>

#include "service/config/rate_limit.h"
#include "service/config/schema.h"
#include "service/domains/dns_zone/dns_zone_command_service.h"
#include "service/domains/dns_zone/dns_zone_read_service.h"
#include "service/domains/dns_zone/dns_zone_schema.h"
#include "service/domains/task/task_service.h"
#include "support/postgres_test.h"
#include "support/redis_test.h"

namespace {
constexpr std::string_view tenant{"01000000-0000-0000-0000-000000000001"};
constexpr std::string_view zone{"01000000-0000-0000-0000-000000000002"};
constexpr std::string_view provider{"01000000-0000-0000-0000-000000000003"};

// A loopback fixture uses the production validation and command/read services.
// Authentication has its own HTTP suite; no test route ships in the service.
class dns_commands final {
public:
    RUVIA_ROUTES_BEGIN(dns_commands)
    RUVIA_GET("/records", read);
    RUVIA_GET("/task", task);
    RUVIA_PUT("/records", write, service::dns_zone::dns_zone_config_body);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> task(ruvia::context& c) {
        co_return c.json(co_await service::task::get_task_service().detail(
            c, std::string(tenant), "dns_zone", std::string(zone)));
    }
    ruvia::task<ruvia::http_response> read(ruvia::context& c) {
        const auto value = co_await service::dns_zone::get_dns_zone_read_service().get(c, std::string(tenant), std::string(zone));
        co_return c.json(value);
    }
    ruvia::task<ruvia::http_response> write(ruvia::context& c) {
        const auto revision = co_await service::dns_zone::get_dns_zone_command_service().update_config(c,
            std::string(tenant), std::string(zone), service::common::require_expected_revision(c),
            c.request_state<service::dns_sync::zone_config_input>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "DNS 记录已更新"));
    }
};

ruvia::task<void> seed(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto credential = service::dns::serialize_dns_provider_config(service::utils::seal_secret("qa-token"), "qa");
    (void)co_await db.execute("INSERT INTO sys_tenant(id,name,slug) VALUES ($1,'DNS QA','dns-qa')", tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id,tenant_id,kind,provider,name,account_id,status,config) "
        "VALUES ($1,$2,'dns','cloudflare','DNS','qa','verified',$3::jsonb)",
        provider, tenant, std::string_view(credential));
    service::dns_sync::zone_runtime_data runtime;
    runtime.lines_.push_back({.code_ = "default", .name_ = "Default", .display_name_ = "Default", .status_ = "enabled"});
    runtime.record_states_.push_back({.id_ = "01000000-0000-0000-0000-000000000008", .remote_record_id_ = "retired-system", .sync_status_ = "synced", .synced_revision_ = 1, .last_error_ = {}, .managed_ = true});
    service::dns_sync::zone_config_data records;
    for (int index = 1; index <= 2; ++index) {
        const auto id = "01000000-0000-0000-0000-00000000000" + std::to_string(index + 3);
        records.records_.push_back({.id_ = id, .type_ = "A", .name_ = index == 1 ? "www" : "other", .content_ = index == 1 ? "192.0.2.1" : "192.0.2.2", .ttl_ = 600, .priority_ = {}, .proxied_ = false, .line_code_ = "default"});
        runtime.record_states_.push_back({.id_ = id, .remote_record_id_ = "remote-" + std::to_string(index), .sync_status_ = "synced", .synced_revision_ = 1, .last_error_ = {}, .managed_ = false});
    }
    const auto config_json = ruvia::to_json(service::dns_sync::to_output(records));
    const auto runtime_json = ruvia::to_json(service::dns_sync::to_output(runtime));
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(id,tenant_id,provider_id,domain,config,runtime,sync_status,synced_revision) "
        "VALUES ($1,$2,$3,'example.com',$4::jsonb,$5::jsonb,'synced',1)",
        zone, tenant, provider,
        std::string_view(config_json), std::string_view(runtime_json));
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(tenant_id,resource_type,dns_zone_id,operation,version,processed_version,is_done,is_ok) "
        "VALUES ($1,'dns_zone',$2,'sync',1,1,true,true)",
        tenant, zone);
    (void)co_await db.execute(
        "INSERT INTO sys_cluster(tenant_id,dns_zone_id,name,hostname_prefix) VALUES ($1,$2,'Empty cluster','edge')",
        tenant, zone);
}
}  // namespace

int main(int argc, char** argv) try {
    const auto database = test_support::isolated_postgres_config();
    const auto redis = test_support::isolated_redis_config();
    const auto* origin = std::getenv("DNS_TEST_ORIGIN_PORT");
    if (!database || !redis || !origin) {
        return 77;
    }
    if (argc != 2) {
        return 1;
    }
    service::utils::configure_secret_key(std::string(64, 'a'));
    service::config::configure_outbound_origins(service::config::make_outbound_origins(""));
    service::config::configure_rate_limits({.outbound_global_ = {10000, std::chrono::seconds(1)},
        .outbound_account_ = {10000, std::chrono::seconds(1)}});
    const test_support::postgres_test_database temporary(*database);
    const ruvia::db_migrator migrator(temporary.config(), {.table_ = "schema_migrations"});
    (void)migrator.migrate(service::config::schema_migrations);
    if (temporary.run(seed) != 0) {
        return 1;
    }
    auto& app = ruvia::app();
    app.database({.config_ = temporary.config()});
    app.redis({.config_ = *redis});
    app.use_worker_state<service::background::work_available::claim_signals>();
    app.get_http_client({.alias_ = "cloudflare", .config_ = {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = static_cast<std::uint16_t>(std::stoi(origin)), .connection_count_ = 2}});
    app.server({.worker_count_ = 2, .process_signal_handlers_ = ruvia::process_signal_handler_policy::install});
    app.listen({.address_ = "127.0.0.1", .http_ = static_cast<std::uint16_t>(std::stoi(argv[1]))});
    app.run();
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
