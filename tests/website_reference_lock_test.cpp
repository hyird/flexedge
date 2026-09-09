#include <array>
#include <stdexcept>
#include "service/domains/website/reference.store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("website reference locking invariant failed");
}

ruvia::Task<void> verifyBody(ruvia::DbClient& client, ruvia::DbClient& peer, bool& ownsSchema) {
    co_await client.connect();
    co_await peer.connect();
    auto setup = co_await client.beginTransaction();
    (void)co_await setup.execute("CREATE SCHEMA flexedge_website_lock_qa");
    (void)co_await setup.execute("SET LOCAL search_path = flexedge_website_lock_qa");
    (void)co_await setup.execute(
        "CREATE TABLE sys_cluster (id uuid PRIMARY KEY, tenant_id uuid, status text, deleted_at timestamptz)");
    (void)co_await setup.execute(
        "CREATE TABLE sys_certificate (id uuid PRIMARY KEY, tenant_id uuid, issued_revision bigint, "
        "expires_at timestamptz, deleted_at timestamptz)");
    (void)co_await setup.execute(
        "CREATE TABLE sys_dns_zone (id uuid PRIMARY KEY, tenant_id uuid, domain text, sort int, deleted_at timestamptz)");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string id = "00000000-0000-0000-0000-000000000010";
    (void)co_await setup.execute("INSERT INTO sys_cluster VALUES ($1,$2,'enabled',NULL)", id, tenant);
    (void)co_await setup.execute(
        "INSERT INTO sys_certificate VALUES ($1,$2,1,NOW()+INTERVAL '1 day',NULL)", id, tenant);
    (void)co_await setup.execute("INSERT INTO sys_dns_zone VALUES ($1,$2,'example.com',0,NULL)", id, tenant);
    co_await setup.commit();
    ownsSchema = true;

    auto reader = co_await client.beginTransaction();
    (void)co_await reader.execute("SET LOCAL search_path = flexedge_website_lock_qa");
    require(co_await service::website::lockAvailableCluster(reader, tenant, id));
    const std::array<std::string, 1> certificates{id};
    require(co_await service::website::lockAvailableCertificates(reader, tenant, certificates));
    require((co_await service::website::lockDnsZoneReferences(reader, tenant)).size() == 1);

    const std::array<const char*, 3> mutations{
        "UPDATE sys_cluster SET status='disabled'",
        "UPDATE sys_certificate SET deleted_at=NOW()",
        "UPDATE sys_dns_zone SET domain='changed.example.com'",
    };
    for (const auto* mutation : mutations) {
        auto writer = co_await peer.beginTransaction();
        (void)co_await writer.execute("SET LOCAL search_path = flexedge_website_lock_qa");
        (void)co_await writer.execute("SET LOCAL lock_timeout = '100ms'");
        bool locked = false;
        try { (void)co_await writer.execute(mutation); }
        catch (const ruvia::DbError& error) {
            if (error.sqlState() != "55P03") throw;
            locked = true;
        }
        require(locked);
    }
    co_await reader.rollback();
    auto writer = co_await peer.beginTransaction();
    (void)co_await writer.execute("SET LOCAL search_path = flexedge_website_lock_qa");
    (void)co_await writer.execute("SET LOCAL lock_timeout = '100ms'");
    for (const auto* mutation : mutations) (void)co_await writer.execute(mutation);
    require(!(co_await service::website::lockAvailableCluster(writer, tenant, id)));
    require(!(co_await service::website::lockAvailableCertificates(writer, tenant, certificates)));
    const auto zones = co_await service::website::lockDnsZoneReferences(writer, tenant);
    require(zones.size() == 1 && zones.front().domain == "changed.example.com");
    co_await writer.rollback();
}

ruvia::Task<void> verify(ruvia::DbClient& client, ruvia::DbClient& peer) {
    bool ownsSchema = false;
    std::exception_ptr failure;
    try { co_await verifyBody(client, peer, ownsSchema); }
    catch (...) { failure = std::current_exception(); }
    if (ownsSchema) (void)co_await client.execute("DROP SCHEMA flexedge_website_lock_qa CASCADE");
    if (failure) std::rethrow_exception(failure);
}
}

int main() { return test_support::runPostgresTest(verify); }
