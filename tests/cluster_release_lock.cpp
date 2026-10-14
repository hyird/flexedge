#include <stdexcept>

#include "service/features/node_dispatch/release_store.h"
#include "support/postgres_test.h"
namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("cluster release lock invariant failed");
    }
}
ruvia::task<void> verify_body(ruvia::db_client& client, ruvia::db_client& peer, bool& owns_schema) {
    co_await client.connect();
    co_await peer.connect();
    auto db = client.handle();
    auto peer_db = peer.handle();
    auto setup = co_await db.begin_transaction();
    (void)co_await setup.execute("CREATE SCHEMA flexedge_release_lock_qa");
    (void)co_await setup.execute("SET LOCAL search_path=flexedge_release_lock_qa");
    (void)co_await setup.execute("CREATE TABLE sys_dns_zone (id text,tenant_id text,domain text,deleted_at timestamptz)");
    (void)co_await setup.execute("CREATE TABLE sys_cluster (id text PRIMARY KEY,tenant_id text,dns_zone_id text,hostname_prefix text,status text,release_generation bigint,updated_at timestamptz,deleted_at timestamptz)");
    (void)co_await setup.execute("CREATE TABLE sys_cluster_release (id uuid DEFAULT gen_random_uuid(),tenant_id text,cluster_id text,generation bigint,schema_version bigint,status text,created_at timestamptz,UNIQUE(tenant_id,cluster_id,generation))");
    (void)co_await setup.execute("INSERT INTO sys_dns_zone VALUES ('z','t','example.com',NULL)");
    (void)co_await setup.execute("INSERT INTO sys_cluster VALUES ('c','t','z','edge','enabled',0,NOW(),NULL)");
    co_await setup.commit();
    owns_schema = true;
    auto first = co_await db.begin_transaction();
    (void)co_await first.execute("SET LOCAL search_path=flexedge_release_lock_qa");
    require((co_await service::node_dispatch::allocate_cluster_release(first, "t", "c", 5)).generation_ == 1);
    {
        auto blocked = co_await peer_db.begin_transaction();
        (void)co_await blocked.execute("SET LOCAL search_path=flexedge_release_lock_qa");
        (void)co_await blocked.execute("SET LOCAL lock_timeout='100ms'");
        bool locked = false;
        try {
            (void)co_await service::node_dispatch::allocate_cluster_release(blocked, "t", "c", 5);
        } catch (const ruvia::db_error& error) {
            if (error.sql_state() != "55P03") {
                throw;
            }
            locked = true;
        }
        require(locked);
    }
    co_await first.commit();
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto next = co_await peer_db.begin_transaction();
        (void)co_await next.execute("SET LOCAL search_path=flexedge_release_lock_qa");
        (void)co_await next.execute("SET LOCAL lock_timeout='100ms'");
        require((co_await service::node_dispatch::allocate_cluster_release(next, "t", "c", 5)).generation_ == 2);
        // Both generation increment and building row must roll back together.
        co_await next.rollback();
    }
    const auto rows = co_await db.query("SELECT generation FROM flexedge_release_lock_qa.sys_cluster_release");
    require(rows.size() == 1 && rows[0][0].as<int>() == 1);
}
ruvia::task<void> verify(ruvia::db_client& client, ruvia::db_client& peer) {
    bool owns_schema = false;
    std::exception_ptr failure;
    try {
        co_await verify_body(client, peer, owns_schema);
    } catch (...) {
        failure = std::current_exception();
    }
    if (owns_schema) {
        (void)co_await client.handle().execute("DROP SCHEMA flexedge_release_lock_qa CASCADE");
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}
}  // namespace
int main() {
    return test_support::run_postgres_test(verify);
}