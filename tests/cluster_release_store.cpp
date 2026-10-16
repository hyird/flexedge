#include <stdexcept>
#include <string>
#include <vector>

#include "service/features/background/queued_work.h"
#include "service/features/node_dispatch/release_store.h"
#include "support/postgres_test.h"
namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("cluster release storage invariant failed");
    }
}
ruvia::task<void> verify(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    auto tx = co_await db.begin_transaction();
    (void)co_await tx.execute("CREATE TEMP TABLE sys_dns_zone (id text,tenant_id text,domain text,deleted_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster (id text,tenant_id text,dns_zone_id text,hostname_prefix text,status text,release_generation bigint,current_release_id uuid,updated_at timestamptz,deleted_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster_release (id uuid DEFAULT gen_random_uuid(),tenant_id text,cluster_id text,generation bigint,schema_version bigint,status text,manifest_digest text,manifest_envelope text,target_node_count bigint,created_at timestamptz,activated_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_node (id text,tenant_id text,cluster_id text,registration_status text,status text,agent_id text,node_secret_hash text,node_secret_envelope text,desired_release_id uuid,updated_at timestamptz,deleted_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_node_release_target (tenant_id text,release_id uuid,node_id text,status text,updated_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster_release_object (tenant_id text,release_id uuid,position bigint,object_digest text) ON COMMIT DROP");
    (void)co_await tx.execute("INSERT INTO sys_dns_zone VALUES ('z','t','example.com',NULL)");
    (void)co_await tx.execute("INSERT INTO sys_cluster VALUES ('c','t','z','edge','enabled',0,NULL,NOW(),NULL)");
    (void)co_await tx.execute("INSERT INTO sys_node SELECT id,'t','c','registered','enabled','agent','hash','envelope',NULL,NOW(),NULL FROM (VALUES ('active'),('disabled'),('pending'),('no-secret'),('other-cluster'),('other-tenant'),('deleted')) fixture(id)");
    (void)co_await tx.execute("UPDATE sys_node SET status='disabled' WHERE id='disabled'");
    (void)co_await tx.execute("UPDATE sys_node SET registration_status='pending' WHERE id='pending'");
    (void)co_await tx.execute("UPDATE sys_node SET node_secret_hash=NULL WHERE id='no-secret'");
    (void)co_await tx.execute("UPDATE sys_node SET cluster_id='other' WHERE id='other-cluster'");
    (void)co_await tx.execute("UPDATE sys_node SET tenant_id='other' WHERE id='other-tenant'");
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW() WHERE id='deleted'");
    require(!(co_await service::node_dispatch::lock_current_release(tx, "t", "c")));
    for (const auto* tenant : {"other", "t"}) {
        if (std::string_view(tenant) == "t") {
            (void)co_await tx.execute("UPDATE sys_dns_zone SET deleted_at=NOW()");
        }
        bool rejected = false;
        try {
            (void)co_await service::node_dispatch::allocate_cluster_release(tx, tenant, "c", 5);
        } catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()) == "cluster release target does not exist";
        }
        require(rejected);
        const auto unchanged = co_await tx.query("SELECT release_generation FROM sys_cluster WHERE id='c'");
        require(unchanged[0][0].as<int>() == 0);
        require((co_await tx.query("SELECT id FROM sys_cluster_release")).empty());
    }
    (void)co_await tx.execute("UPDATE sys_dns_zone SET deleted_at=NULL");
    for (std::int64_t generation = 1; generation <= 2; ++generation) {
        const auto allocated = co_await service::node_dispatch::allocate_cluster_release(tx, "t", "c", 5);
        require(allocated.generation_ == generation && allocated.access_domain_ == "edge.example.com" && allocated.enabled_);
        co_await service::node_dispatch::link_release_object(tx, "t", allocated.id_, 0, "digest");
        // Activation changes the task of every new target node (the node task
        // id is the node id); the pending target of the superseded release
        // belongs to the same node.
        service::background::queued_work work;
        co_await service::node_dispatch::activate_cluster_release(tx, "t", "c", allocated.id_, "digest", "envelope",
            work);
        require(work.changed_tasks() == std::vector<std::string>{"active"});
        const auto current = co_await service::node_dispatch::lock_current_release(tx, "t", "c");
        require(current && current->id_ == allocated.id_ && current->schema_version_ == 5);
        const auto targets = co_await tx.query("SELECT node_id,status FROM sys_node_release_target WHERE release_id=$1", allocated.id_);
        require(targets.size() == 1 && targets[0][0].value() == "active" && targets[0][1].value() == "pending");
        const auto desired = co_await tx.query("SELECT id FROM sys_node WHERE desired_release_id=$1 ORDER BY id", allocated.id_);
        // Disabled registered nodes retain the desired release for later re-enabling.
        require(desired.size() == 2 && desired[0][0].value() == "active" && desired[1][0].value() == "disabled");
        const auto cluster = co_await tx.query("SELECT current_release_id::text FROM sys_cluster WHERE id='c'");
        require(cluster[0][0].value() == allocated.id_);
    }
    const auto releases = co_await tx.query("SELECT status,target_node_count FROM sys_cluster_release ORDER BY generation");
    require(releases.size() == 2 && releases[0][0].value() == "superseded" && releases[1][0].value() == "active");
    require(releases[0][1].as<int>() == 1 && releases[1][1].as<int>() == 1);
    (void)co_await tx.execute("CREATE TEMP TABLE sys_certificate (id text,tenant_id text,expires_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_website (id text,tenant_id text,cluster_id text,deleted_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_website_certificate_binding (website_id text,certificate_id text,tenant_id text) ON COMMIT DROP");
    // Expired certificates still need their consumers republished to remove stale material.
    (void)co_await tx.execute("INSERT INTO sys_certificate VALUES ('cert','t',NOW()-INTERVAL '1 day'),('cert','other',NOW())");
    (void)co_await tx.execute("INSERT INTO sys_website VALUES ('w1','t','c',NULL),('w2','t','c',NULL),('w3','t','d',NULL),('gone','t','deleted',NOW()),('foreign','other','foreign',NULL)");
    (void)co_await tx.execute("INSERT INTO sys_website_certificate_binding VALUES ('w1','cert','t'),('w2','cert','t'),('w3','cert','t'),('gone','cert','t'),('foreign','cert','other')");
    const auto tenant_clusters = co_await service::node_dispatch::find_certificate_consumer_clusters(tx, "t", "cert");
    const std::vector<std::string> expected_tenant_clusters{"c", "d"};
    require(tenant_clusters == expected_tenant_clusters);
    const auto other_tenant_clusters = co_await service::node_dispatch::find_certificate_consumer_clusters(tx, "other", "cert");
    const std::vector<std::string> expected_other_tenant_clusters{"foreign"};
    require(other_tenant_clusters == expected_other_tenant_clusters);
    require((co_await service::node_dispatch::find_certificate_consumer_clusters(tx, "t", "missing")).empty());
    require((co_await service::node_dispatch::find_certificate_consumer_clusters(tx, "missing", "cert")).empty());
    co_await tx.rollback();
}
}  // namespace
int main() {
    return test_support::run_postgres_test(verify);
}