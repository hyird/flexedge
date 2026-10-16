#include <stdexcept>

#include "service/domains/website/aggregate_store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("website aggregate invariant failed");
    }
}

ruvia::task<void> verify(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    auto transaction = co_await db.begin_transaction();
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_website (id uuid PRIMARY KEY DEFAULT gen_random_uuid(), "
        "tenant_id uuid, cluster_id uuid, status text, revision bigint, config jsonb, runtime jsonb, "
        "created_at timestamptz, updated_at timestamptz, deleted_at timestamptz) ON COMMIT DROP");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string other_tenant = "00000000-0000-0000-0000-000000000002";
    const std::string cluster = "00000000-0000-0000-0000-000000000010";
    const std::string next_cluster = "00000000-0000-0000-0000-000000000011";
    const auto id = co_await service::website::insert_website(transaction, tenant, cluster, "enabled", "{}");
    require(!id.empty());
    require((co_await service::website::lock_website_revision(transaction, tenant, id)) == 1);
    auto snapshot = co_await service::website::lock_website_snapshot(transaction, tenant, id);
    require(snapshot && snapshot->revision_ == 1);
    require(snapshot && snapshot->cluster_id_ == cluster && snapshot->config_json_ == "{}");
    require(!(co_await service::website::lock_website_revision(transaction, other_tenant, id)));
    require(!(co_await service::website::lock_website_snapshot(transaction, other_tenant, id)));
    require(!(co_await service::website::update_website(transaction, other_tenant, id, 1, next_cluster, "disabled", "{}")));
    require(!(co_await service::website::update_website(transaction, tenant, id, 9, next_cluster, "disabled", "{}")));
    require((co_await service::website::lock_website_revision(transaction, tenant, id)) == 1);
    require((co_await service::website::update_website(transaction, tenant, id, 1, next_cluster, "disabled", "{\"v\":2}")) == 2);
    snapshot = co_await service::website::lock_website_snapshot(transaction, tenant, id);
    require(snapshot && snapshot->revision_ == 2);
    require(snapshot && snapshot->cluster_id_ == next_cluster && snapshot->config_json_ == "{\"v\": 2}");
    const auto persisted = co_await transaction.query(
        "SELECT status, runtime::text, created_at IS NOT NULL, updated_at IS NOT NULL FROM sys_website WHERE id=$1", id);
    require(persisted.front()[0].as<std::string>() == "disabled");
    require(persisted.front()[1].as<std::string>() == "{}");
    require(persisted.front()[2].as<bool>().value_or(false));
    require(persisted.front()[3].as<bool>().value_or(false));
    require(!(co_await service::website::soft_delete_website(transaction, other_tenant, id, 2)));
    require(!(co_await service::website::soft_delete_website(transaction, tenant, id, 1)));
    const auto deleted = co_await service::website::soft_delete_website(transaction, tenant, id, 2);
    require(deleted && deleted->revision_ == 3);
    require(deleted && deleted->cluster_id_ == snapshot->cluster_id_ && deleted->config_json_ == snapshot->config_json_);
    require(!(co_await service::website::lock_website_snapshot(transaction, tenant, id)));
    require(!(co_await service::website::lock_website_revision(transaction, tenant, id)));
    require(!(co_await service::website::soft_delete_website(transaction, tenant, id, 3)));
    require(!(co_await service::website::update_website(transaction, tenant, id, 3, cluster, "enabled", "{}")));
    const auto tombstone = co_await transaction.query("SELECT revision, deleted_at IS NOT NULL FROM sys_website WHERE id=$1", id);
    require(tombstone.front()[0].as<int>() == 3);
    require(tombstone.front()[1].as<bool>().value_or(false));
    // Deliberately permissive temporary schema simulates broken storage invariants.
    const auto corrupt_id = co_await service::website::insert_website(
        transaction, tenant, cluster, "enabled", "{}");
    (void)co_await transaction.execute(
        "UPDATE sys_website SET revision = NULL, config = NULL WHERE id = $1", corrupt_id);
    bool revision_rejected = false;
    try {
        (void)co_await service::website::lock_website_revision(transaction, tenant, corrupt_id);
    } catch (const std::exception&) {
        revision_rejected = true;
    }
    require(revision_rejected);
    bool snapshot_rejected = false;
    try {
        (void)co_await service::website::lock_website_snapshot(transaction, tenant, corrupt_id);
    } catch (const std::exception&) {
        snapshot_rejected = true;
    }
    require(snapshot_rejected);
    co_await transaction.rollback();
}
}  // namespace

int main() {
    return test_support::run_postgres_test(verify);
}
