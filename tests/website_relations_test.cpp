#include <array>
#include <optional>
#include <stdexcept>
#include "service/domains/website/relation.store.h"
#include "service/domains/website/reference.store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("website relation invariant failed");
}

ruvia::Task<void> verifyReferences(ruvia::DbClient& client) {
    auto transaction = co_await client.beginTransaction();
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_cluster (id uuid, tenant_id uuid, status text, "
        "deleted_at timestamptz) ON COMMIT DROP");
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_certificate (id uuid, tenant_id uuid, issued_revision bigint, "
        "expires_at timestamptz, deleted_at timestamptz) ON COMMIT DROP");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string otherTenant = "00000000-0000-0000-0000-000000000002";
    const std::string cluster = "00000000-0000-0000-0000-000000000010";
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_dns_zone (id uuid, tenant_id uuid, domain text, sort int, "
        "deleted_at timestamptz) ON COMMIT DROP");
    require((co_await service::website::lockDnsZoneReferences(transaction, tenant)).empty());
    (void)co_await transaction.execute(
        "INSERT INTO sys_dns_zone VALUES "
        "($1, $2, 'example.com', 0, NULL), "
        "($1, $2, 'sub.example.com', 9, NULL), "
        "($1, $2, 'alt.example.com', 1, NULL), "
        "($1, $2, 'deleted.sub.example.com', 0, NOW()), "
        "($1, $3, 'foreign.sub.example.com', 0, NULL)", cluster, tenant, otherTenant);
    const auto zones = co_await service::website::lockDnsZoneReferences(transaction, tenant);
    require(zones.size() == 3);
    require(zones[0].domain == "alt.example.com");
    require(zones[1].domain == "sub.example.com");
    require(zones[2].domain == "example.com");
    for (const auto& zone : zones) require(zone.id == cluster);
    for (const auto* field : {"id", "domain"}) {
        // The temporary schema intentionally permits violations of production NOT NULL.
        (void)co_await transaction.execute("SAVEPOINT invalid_zone");
        (void)co_await transaction.execute(
            std::string("UPDATE sys_dns_zone SET ") + field + " = NULL WHERE tenant_id=$1", tenant);
        bool rejected = false;
        try {
            (void)co_await service::website::lockDnsZoneReferences(transaction, tenant);
        } catch (const std::bad_optional_access&) { rejected = true; }
        require(rejected);
        (void)co_await transaction.execute("ROLLBACK TO SAVEPOINT invalid_zone");
        (void)co_await transaction.execute("RELEASE SAVEPOINT invalid_zone");
    }
    std::array<std::string, 2> certificates{{
        "00000000-0000-0000-0000-000000000040",
        "00000000-0000-0000-0000-000000000041",
    }};
    require(!(co_await service::website::lockAvailableCluster(transaction, tenant, cluster)));
    (void)co_await transaction.execute(
        "INSERT INTO sys_cluster VALUES ($1, $2, 'enabled', NULL)", cluster, tenant);
    require(co_await service::website::lockAvailableCluster(transaction, tenant, cluster));
    require(!(co_await service::website::lockAvailableCluster(transaction, otherTenant, cluster)));
    (void)co_await transaction.execute("UPDATE sys_cluster SET status='disabled'");
    require(!(co_await service::website::lockAvailableCluster(transaction, tenant, cluster)));
    (void)co_await transaction.execute("UPDATE sys_cluster SET status='enabled', deleted_at=NOW()");
    require(!(co_await service::website::lockAvailableCluster(transaction, tenant, cluster)));
    require(co_await service::website::lockAvailableCertificates(transaction, tenant, {}));
    require(!(co_await service::website::lockAvailableCertificates(transaction, tenant, certificates)));
    for (const auto& id : certificates) {
        (void)co_await transaction.execute(
            "INSERT INTO sys_certificate VALUES ($1, $2, 1, NOW() + INTERVAL '1 day', NULL)", id, tenant);
    }
    require(co_await service::website::lockAvailableCertificates(transaction, tenant, certificates));
    require(!(co_await service::website::lockAvailableCertificates(transaction, otherTenant, certificates)));
    // One unavailable member must reject the entire selection.
    for (const auto* mutation : {
             "issued_revision=0", "expires_at=NOW()", "expires_at=NULL", "deleted_at=NOW()"}) {
        (void)co_await transaction.execute(
            std::string("UPDATE sys_certificate SET ") + mutation + " WHERE id=$1", certificates[1]);
        require(!(co_await service::website::lockAvailableCertificates(transaction, tenant, certificates)));
        (void)co_await transaction.execute(
            "UPDATE sys_certificate SET issued_revision=1, expires_at=NOW()+INTERVAL '1 day', "
            "deleted_at=NULL WHERE id=$1", certificates[1]);
        require(co_await service::website::lockAvailableCertificates(transaction, tenant, certificates));
    }
    co_await transaction.rollback();
}

ruvia::Task<void> verify(ruvia::DbClient& client) {
    co_await client.connect();
    co_await verifyReferences(client);
    auto transaction = co_await client.beginTransaction();
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_website_domain_claim (tenant_id uuid, website_id uuid, "
        "domain_id uuid, domain_key text, dns_mode text, dns_zone_id uuid) ON COMMIT DROP");
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_website_certificate_binding (tenant_id uuid, website_id uuid, "
        "certificate_id uuid, position bigint) ON COMMIT DROP");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string otherTenant = "00000000-0000-0000-0000-000000000002";
    const std::string website = "00000000-0000-0000-0000-000000000010";
    const std::string otherWebsite = "00000000-0000-0000-0000-000000000011";
    const std::string zone = "00000000-0000-0000-0000-000000000030";
    std::array<service::website::DomainClaim, 2> claims{{
        {"00000000-0000-0000-0000-000000000020", "one.example", "managed", zone},
        {"00000000-0000-0000-0000-000000000021", "two.example", "external", {}},
    }};
    std::array<std::string, 2> certificates{{
        "00000000-0000-0000-0000-000000000040",
        "00000000-0000-0000-0000-000000000041",
    }};
    co_await service::website::replaceRelationProjections(transaction, tenant, website, claims, certificates);
    require(co_await service::website::hasConflictingDomainClaims(transaction, tenant, claims, {}));
    require(!(co_await service::website::hasConflictingDomainClaims(transaction, tenant, claims, website)));
    require(!(co_await service::website::hasConflictingDomainClaims(transaction, otherTenant, claims, {})));
    require(!(co_await service::website::hasConflictingDomainClaims(transaction, tenant, {}, {})));
    co_await service::website::replaceRelationProjections(transaction, otherTenant, website, claims, certificates);
    co_await service::website::replaceRelationProjections(transaction, tenant, otherWebsite, claims, certificates);
    require(co_await service::website::hasConflictingDomainClaims(transaction, tenant, claims, website));
    const auto domains = co_await transaction.query(
        "SELECT domain_id::text, domain_key, dns_mode, dns_zone_id::text FROM "
        "sys_website_domain_claim WHERE tenant_id=$1 AND website_id=$2 ORDER BY domain_key",
        tenant, website);
    require(domains.size() == 2);
    for (std::size_t i = 0; i < claims.size(); ++i) {
        require(domains[i][0].as<std::string>() == claims[i].id);
        require(domains[i][1].as<std::string>() == claims[i].key);
        require(domains[i][2].as<std::string>() == claims[i].dnsMode);
        require(domains[i][3].as<std::string>() == claims[i].dnsZoneId);
    }
    std::swap(certificates[0], certificates[1]);
    co_await service::website::replaceRelationProjections(transaction, tenant, website, {}, certificates);
    const auto bindings = co_await transaction.query(
        "SELECT certificate_id::text, position FROM sys_website_certificate_binding "
        "WHERE tenant_id=$1 AND website_id=$2 ORDER BY position", tenant, website);
    require(bindings.size() == 2);
    for (std::size_t i = 0; i < certificates.size(); ++i) {
        require(bindings[i][0].as<std::string>() == certificates[i]);
        require(bindings[i][1].as<int>() == static_cast<int>(i));
    }
    co_await service::website::replaceRelationProjections(transaction, tenant, website, {}, {});
    for (const auto* table : {"sys_website_domain_claim", "sys_website_certificate_binding"}) {
        const auto rows = co_await transaction.query(
            std::string("SELECT tenant_id::text, website_id::text, count(*) FROM ") + table +
            " GROUP BY tenant_id, website_id ORDER BY tenant_id, website_id");
        require(rows.size() == 2);
        require(rows[0][0].as<std::string>() == tenant);
        require(rows[0][1].as<std::string>() == otherWebsite);
        require(rows[1][0].as<std::string>() == otherTenant);
        require(rows[1][1].as<std::string>() == website);
        require(rows[0][2].as<int>() == 2 && rows[1][2].as<int>() == 2);
    }
    co_await transaction.rollback();
    // A failed DbTransaction is closed by the database layer; each probe owns its transaction.
    for (const bool domainConflict : {true, false}) {
        auto probe = co_await client.beginTransaction();
        (void)co_await probe.execute(
            "CREATE TEMP TABLE conflict_probe (domain_key text CONSTRAINT uq_website_domain_claim UNIQUE, "
            "other_key text CONSTRAINT uq_other_key UNIQUE) ON COMMIT DROP");
        (void)co_await probe.execute("INSERT INTO conflict_probe VALUES ('a', 'b')");
        bool caught = false;
        try {
            (void)co_await probe.execute(domainConflict
                ? "INSERT INTO conflict_probe VALUES ('a', 'c')"
                : "INSERT INTO conflict_probe VALUES ('c', 'b')");
        } catch (const ruvia::DbError& error) {
            caught = true;
            require(error.sqlState() == "23505");
            require(service::website::isDomainClaimConflict(error) == domainConflict);
        }
        require(caught);
    }
}
} // namespace

int main() { return test_support::runPostgresTest(verify); }