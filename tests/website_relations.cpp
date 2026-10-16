#include <ruvia/web/db/db_handle.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "service/domains/certificate/certificate_reference.h"
#include "service/domains/cluster/cluster_reference.h"
#include "service/domains/dns_zone/dns_zone_reference.h"
#include "service/domains/website/relation_store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("website relation invariant failed");
    }
}

ruvia::task<void> verify_references(ruvia::db_handle& db) {
    auto transaction = co_await db.begin_transaction();
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_cluster (id uuid, tenant_id uuid, status text, "
        "deleted_at timestamptz, dns_zone_id uuid) ON COMMIT DROP");
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_certificate (id uuid, tenant_id uuid, issued_revision bigint, "
        "expires_at timestamptz, deleted_at timestamptz, subject_alt_names varchar(253)[]) ON "
        "COMMIT DROP");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string other_tenant = "00000000-0000-0000-0000-000000000002";
    const std::string cluster = "00000000-0000-0000-0000-000000000010";
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_dns_zone (id uuid, tenant_id uuid, domain text, sort int, "
        "deleted_at timestamptz) ON COMMIT DROP");
    require((co_await service::dns_zone::lock_live_dns_zones(transaction, tenant)).empty());
    (void)co_await transaction.execute(
        "INSERT INTO sys_dns_zone VALUES "
        "($1, $2, 'example.com', 0, NULL), "
        "($1, $2, 'sub.example.com', 9, NULL), "
        "($1, $2, 'alt.example.com', 1, NULL), "
        "($1, $2, 'deleted.sub.example.com', 0, NOW()), "
        "($1, $3, 'foreign.sub.example.com', 0, NULL)",
        cluster, tenant, other_tenant);
    const auto zones = co_await service::dns_zone::lock_live_dns_zones(transaction, tenant);
    require(zones.size() == 3);
    require(zones[0].domain_ == "alt.example.com");
    require(zones[1].domain_ == "sub.example.com");
    require(zones[2].domain_ == "example.com");
    for (const auto& zone : zones) {
        require(zone.id_ == cluster);
    }
    for (const auto* field : {"id", "domain"}) {
        // The temporary schema intentionally permits violations of production NOT NULL.
        (void)co_await transaction.execute("SAVEPOINT invalid_zone");
        (void)co_await transaction.execute(
            std::string("UPDATE sys_dns_zone SET ") + field + " = NULL WHERE tenant_id=$1", tenant);
        bool rejected = false;
        try {
            (void)co_await service::dns_zone::lock_live_dns_zones(transaction, tenant);
        } catch (const std::bad_optional_access&) {
            rejected = true;
        }
        require(rejected);
        (void)co_await transaction.execute("ROLLBACK TO SAVEPOINT invalid_zone");
        (void)co_await transaction.execute("RELEASE SAVEPOINT invalid_zone");
    }
    std::array<std::string, 2> certificates{{
        "00000000-0000-0000-0000-000000000040",
        "00000000-0000-0000-0000-000000000041",
    }};
    require(!(co_await service::cluster::lock_enabled_cluster(transaction, tenant, cluster)));
    (void)co_await transaction.execute(
        "INSERT INTO sys_cluster VALUES ($1, $2, 'enabled', NULL)", cluster, tenant);
    require((co_await service::cluster::lock_enabled_cluster(transaction, tenant, cluster)).has_value());
    require(!(co_await service::cluster::lock_enabled_cluster(transaction, other_tenant, cluster)));
    (void)co_await transaction.execute("UPDATE sys_cluster SET status='disabled'");
    require(!(co_await service::cluster::lock_enabled_cluster(transaction, tenant, cluster)));
    (void)co_await transaction.execute("UPDATE sys_cluster SET status='enabled', deleted_at=NOW()");
    require(!(co_await service::cluster::lock_enabled_cluster(transaction, tenant, cluster)));
    require((co_await service::certificate::lock_usable_certificates(transaction, tenant, {})).has_value());
    require(!(co_await service::certificate::lock_usable_certificates(transaction, tenant, certificates)));
    (void)co_await transaction.execute(
        "INSERT INTO sys_certificate VALUES ($1, $2, 1, NOW() + INTERVAL '1 day', NULL, "
        "ARRAY['example.com', 'www.example.com']), ($3, $2, 1, NOW() + INTERVAL '1 day', NULL, "
        "ARRAY['*.example.com'])",
        certificates[0], tenant, certificates[1]);
    {
        // Configured names come back with each locked certificate for domain matching.
        const auto locked =
            co_await service::certificate::lock_usable_certificates(transaction, tenant, certificates);
        require(locked && locked->size() == 2);
        for (const auto& certificate : *locked) {
            require(certificate.id_ == certificates[0]
                        ? certificate.names_ == std::vector<std::string>{"example.com", "www.example.com"}
                        : certificate.id_ == certificates[1] &&
                              certificate.names_ == std::vector<std::string>{"*.example.com"});
        }
    }
    require(!(co_await service::certificate::lock_usable_certificates(transaction, other_tenant, certificates)));
    // One unavailable member must reject the entire selection.
    for (const auto* mutation : {
             "issued_revision=0", "expires_at=NOW()", "expires_at=NULL", "deleted_at=NOW()"}) {
        (void)co_await transaction.execute(
            std::string("UPDATE sys_certificate SET ") + mutation + " WHERE id=$1", certificates[1]);
        require(!(co_await service::certificate::lock_usable_certificates(transaction, tenant, certificates)));
        (void)co_await transaction.execute(
            "UPDATE sys_certificate SET issued_revision=1, expires_at=NOW()+INTERVAL '1 day', "
            "deleted_at=NULL WHERE id=$1",
            certificates[1]);
        require((co_await service::certificate::lock_usable_certificates(transaction, tenant, certificates))
                .has_value());
    }
    co_await transaction.rollback();
}

ruvia::task<void> verify(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    co_await verify_references(db);
    auto transaction = co_await db.begin_transaction();
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_website_domain_claim (tenant_id uuid, website_id uuid, "
        "domain_id uuid, domain_key text, dns_mode text, dns_zone_id uuid) ON COMMIT DROP");
    (void)co_await transaction.execute(
        "CREATE TEMP TABLE sys_website_certificate_binding (tenant_id uuid, website_id uuid, "
        "certificate_id uuid, position bigint) ON COMMIT DROP");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string other_tenant = "00000000-0000-0000-0000-000000000002";
    const std::string website = "00000000-0000-0000-0000-000000000010";
    const std::string other_website = "00000000-0000-0000-0000-000000000011";
    const std::string zone = "00000000-0000-0000-0000-000000000030";
    std::array<service::website::domain_claim, 2> claims{{
        {"00000000-0000-0000-0000-000000000020", "one.example", "managed", zone},
        {"00000000-0000-0000-0000-000000000021", "two.example", "external", {}},
    }};
    std::array<std::string, 2> certificates{{
        "00000000-0000-0000-0000-000000000040",
        "00000000-0000-0000-0000-000000000041",
    }};
    co_await service::website::replace_relation_projections(transaction, tenant, website, claims, certificates);
    require(co_await service::website::has_conflicting_domain_claims(transaction, tenant, claims, {}));
    require(!(co_await service::website::has_conflicting_domain_claims(transaction, tenant, claims, website)));
    require(!(co_await service::website::has_conflicting_domain_claims(transaction, other_tenant, claims, {})));
    require(!(co_await service::website::has_conflicting_domain_claims(transaction, tenant, {}, {})));
    co_await service::website::replace_relation_projections(transaction, other_tenant, website, claims, certificates);
    co_await service::website::replace_relation_projections(transaction, tenant, other_website, claims, certificates);
    require(co_await service::website::has_conflicting_domain_claims(transaction, tenant, claims, website));
    const auto domains = co_await transaction.query(
        "SELECT domain_id::text, domain_key, dns_mode, dns_zone_id::text FROM "
        "sys_website_domain_claim WHERE tenant_id=$1 AND website_id=$2 ORDER BY domain_key",
        tenant, website);
    require(domains.size() == 2);
    for (std::size_t i = 0; i < claims.size(); ++i) {
        require(domains[i][0].as<std::string>() == claims[i].id_);
        require(domains[i][1].as<std::string>() == claims[i].key_);
        require(domains[i][2].as<std::string>() == claims[i].dns_mode_);
        require(domains[i][3].as<std::string>() == claims[i].dns_zone_id_);
    }
    std::swap(certificates[0], certificates[1]);
    co_await service::website::replace_relation_projections(transaction, tenant, website, {}, certificates);
    const auto bindings = co_await transaction.query(
        "SELECT certificate_id::text, position FROM sys_website_certificate_binding "
        "WHERE tenant_id=$1 AND website_id=$2 ORDER BY position",
        tenant, website);
    require(bindings.size() == 2);
    for (std::size_t i = 0; i < certificates.size(); ++i) {
        require(bindings[i][0].as<std::string>() == certificates[i]);
        require(bindings[i][1].as<int>() == static_cast<int>(i));
    }
    co_await service::website::replace_relation_projections(transaction, tenant, website, {}, {});
    for (const auto* table : {"sys_website_domain_claim", "sys_website_certificate_binding"}) {
        const auto rows = co_await transaction.query(
            std::string("SELECT tenant_id::text, website_id::text, count(*) FROM ") + table +
            " GROUP BY tenant_id, website_id ORDER BY tenant_id, website_id");
        require(rows.size() == 2);
        require(rows[0][0].as<std::string>() == tenant);
        require(rows[0][1].as<std::string>() == other_website);
        require(rows[1][0].as<std::string>() == other_tenant);
        require(rows[1][1].as<std::string>() == website);
        require(rows[0][2].as<int>() == 2 && rows[1][2].as<int>() == 2);
    }
    co_await transaction.rollback();
    // A failed db_transaction is closed by the database layer; each probe owns its transaction.
    for (const bool domain_conflict : {true, false}) {
        auto probe = co_await db.begin_transaction();
        (void)co_await probe.execute(
            "CREATE TEMP TABLE conflict_probe (domain_key text CONSTRAINT uq_website_domain_claim UNIQUE, "
            "other_key text CONSTRAINT uq_other_key UNIQUE) ON COMMIT DROP");
        (void)co_await probe.execute("INSERT INTO conflict_probe VALUES ('a', 'b')");
        bool caught = false;
        try {
            (void)co_await probe.execute(domain_conflict
                                             ? "INSERT INTO conflict_probe VALUES ('a', 'c')"
                                             : "INSERT INTO conflict_probe VALUES ('c', 'b')");
        } catch (const ruvia::db_error& error) {
            caught = true;
            require(error.sql_state() == "23505");
            require(service::website::is_domain_claim_conflict(error) == domain_conflict);
        }
        require(caught);
    }
}
}  // namespace

int main() {
    return test_support::run_postgres_test(verify);
}