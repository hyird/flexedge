#include <stdexcept>
#include "service/features/node_dispatch/target.store.h"
#include "support/postgres_test.h"
namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("node target storage invariant failed");
}
ruvia::Task<void> verify(ruvia::DbClient& client) {
    co_await client.connect();
    auto tx = co_await client.beginTransaction();
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster_release (tenant_id text,id text,cluster_id text) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_node_release_target (fixture_id int GENERATED ALWAYS AS IDENTITY,tenant_id text,release_id text,node_id text,status text,updated_at timestamptz,failed_phase text DEFAULT 'qa',error_code text DEFAULT 'qa',last_error text DEFAULT 'qa',retryable boolean DEFAULT true,applied_at timestamptz) ON COMMIT DROP");
    for (const auto* tenant : {"t1","t2"}) {
        for (const auto* release : {"r1","r2"}) {
            (void)co_await tx.execute("INSERT INTO sys_cluster_release VALUES ($1,$2,$2)",tenant,release);
            for (const auto* node : {"n1","n2"}) {
                for (const auto* status : {"pending","failed","applied","excluded"}) {
                    (void)co_await tx.execute("INSERT INTO sys_node_release_target (tenant_id,release_id,node_id,status,updated_at) VALUES ($1,$2,$3,$4,'2000-01-01')",tenant,release,node,status);
                }
            }
        }
    }
    (void)co_await tx.execute("CREATE TEMP TABLE original ON COMMIT DROP AS TABLE sys_node_release_target");
    for (int scenario=0; scenario<5; ++scenario) {
        (void)co_await tx.execute("SAVEPOINT scenario");
        if (scenario == 0) co_await service::node_dispatch::excludePendingNodeTargets(tx,"t1","n1");
        if (scenario == 1) co_await service::node_dispatch::excludePendingNodeClusterTargets(tx,"t1","n1","r1");
        if (scenario == 2) co_await service::node_dispatch::markNodeTargetApplied(tx,"t1","r1","n1");
        if (scenario >= 3) co_await service::node_dispatch::markNodeTargetFailed(tx,"t1","r1","n1",
            scenario == 3 ? "r1" : "wrong-cluster",
            {.phase="activate",.errorCode="disk",.error="disk full",.retryable=false});
        const auto changed = co_await tx.query(
            "SELECT t.tenant_id,t.node_id,t.release_id,t.status,b.status, "
            "t.failed_phase IS NULL AND t.error_code IS NULL AND t.last_error IS NULL AND t.retryable IS NULL AND t.applied_at IS NOT NULL "
            "FROM sys_node_release_target t JOIN original b USING(fixture_id) WHERE t IS DISTINCT FROM b");
        require(changed.size() == (scenario == 0 ? 4 : scenario == 4 ? 0 : 2));
        for (const auto& row : changed) {
            require(row[0].value() == "t1" && row[1].value() == "n1");
            if (scenario != 0) require(row[2].value() == "r1");
            require(row[3].value() == (scenario == 2 ? "applied" : scenario == 3 ? "failed" : "excluded"));
            require(row[4].value() == "pending" || row[4].value() == "failed");
            if (scenario == 2) require(row[5].as<bool>().value());
        }
        if (scenario == 3) {
            const auto errors = co_await tx.query("SELECT count(*) FROM sys_node_release_target WHERE tenant_id='t1' AND release_id='r1' AND node_id='n1' AND status='failed' AND failed_phase='activate' AND error_code='disk' AND last_error='disk full' AND retryable=false");
            require(errors[0][0].as<int>().value() == 2);
        }
        (void)co_await tx.execute("ROLLBACK TO SAVEPOINT scenario");
        (void)co_await tx.execute("RELEASE SAVEPOINT scenario");
    }
    co_await tx.rollback();
}
}
int main() { return test_support::runPostgresTest(verify); }