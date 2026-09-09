#include <stdexcept>
#include "service/domains/website/aggregate.store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("website aggregate invariant failed");
}

ruvia::Task<void> verify(ruvia::DbClient& client) {
    co_await client.connect();
    auto transaction = co_await client.beginTransaction();
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_website (id uuid PRIMARY KEY DEFAULT gen_random_uuid(), "
        "tenant_id uuid, cluster_id uuid, status text, revision bigint, config jsonb, runtime jsonb, "
        "created_at timestamptz, updated_at timestamptz, deleted_at timestamptz) ON COMMIT DROP");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string otherTenant = "00000000-0000-0000-0000-000000000002";
    const std::string cluster = "00000000-0000-0000-0000-000000000010";
    const std::string nextCluster = "00000000-0000-0000-0000-000000000011";
    const auto id = co_await service::website::insertWebsite(transaction, tenant, cluster, "enabled", "{}");
    require(!id.empty());
    require((co_await service::website::findWebsiteRevision(transaction, tenant, id)) == 1);
    require((co_await service::website::lockWebsiteRevision(transaction, tenant, id)) == 1);
    auto snapshot = co_await service::website::lockWebsiteSnapshot(transaction, tenant, id);
    require(snapshot && snapshot->revision == 1);
    require(snapshot && snapshot->clusterId == cluster && snapshot->configJson == "{}");
    require(!(co_await service::website::findWebsiteRevision(transaction, otherTenant, id)));
    require(!(co_await service::website::lockWebsiteRevision(transaction, otherTenant, id)));
    require(!(co_await service::website::lockWebsiteSnapshot(transaction, otherTenant, id)));
    require(!(co_await service::website::updateWebsite(transaction, otherTenant, id, 1, nextCluster, "disabled", "{}")));
    require(!(co_await service::website::updateWebsite(transaction, tenant, id, 9, nextCluster, "disabled", "{}")));
    require((co_await service::website::findWebsiteRevision(transaction, tenant, id)) == 1);
    require((co_await service::website::updateWebsite(transaction, tenant, id, 1, nextCluster, "disabled", "{\"v\":2}")) == 2);
    snapshot = co_await service::website::lockWebsiteSnapshot(transaction, tenant, id);
    require(snapshot && snapshot->revision == 2);
    require(snapshot && snapshot->clusterId == nextCluster && snapshot->configJson == "{\"v\": 2}");
    const auto persisted = co_await transaction.query(
        "SELECT status, runtime::text, created_at IS NOT NULL, updated_at IS NOT NULL FROM sys_website WHERE id=$1", id);
    require(persisted.front()[0].as<std::string>() == "disabled");
    require(persisted.front()[1].as<std::string>() == "{}");
    require(persisted.front()[2].as<bool>().value_or(false));
    require(persisted.front()[3].as<bool>().value_or(false));
    require(!(co_await service::website::softDeleteWebsite(transaction, otherTenant, id, 2)));
    require(!(co_await service::website::softDeleteWebsite(transaction, tenant, id, 1)));
    const auto deleted = co_await service::website::softDeleteWebsite(transaction, tenant, id, 2);
    require(deleted && deleted->revision == 3);
    require(deleted && deleted->clusterId == snapshot->clusterId && deleted->configJson == snapshot->configJson);
    require(!(co_await service::website::findWebsiteRevision(transaction, tenant, id)));
    require(!(co_await service::website::lockWebsiteSnapshot(transaction, tenant, id)));
    require(!(co_await service::website::lockWebsiteRevision(transaction, tenant, id)));
    require(!(co_await service::website::softDeleteWebsite(transaction, tenant, id, 3)));
    require(!(co_await service::website::updateWebsite(transaction, tenant, id, 3, cluster, "enabled", "{}")));
    const auto tombstone = co_await transaction.query("SELECT revision, deleted_at IS NOT NULL FROM sys_website WHERE id=$1", id);
    require(tombstone.front()[0].as<int>() == 3);
    require(tombstone.front()[1].as<bool>().value_or(false));
    // Deliberately permissive temporary schema simulates broken storage invariants.
    const auto corruptId = co_await service::website::insertWebsite(
        transaction, tenant, cluster, "enabled", "{}");
    (void)co_await transaction.execute(
        "UPDATE sys_website SET revision = NULL, config = NULL WHERE id = $1", corruptId);
    bool revisionRejected = false;
    try {
        (void)co_await service::website::findWebsiteRevision(transaction, tenant, corruptId);
    } catch (const std::exception&) { revisionRejected = true; }
    require(revisionRejected);
    bool snapshotRejected = false;
    try {
        (void)co_await service::website::lockWebsiteSnapshot(transaction, tenant, corruptId);
    } catch (const std::exception&) { snapshotRejected = true; }
    require(snapshotRejected);
    co_await transaction.rollback();
}
}

int main() { return test_support::runPostgresTest(verify); }
