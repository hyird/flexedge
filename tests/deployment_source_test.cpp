#include <optional>
#include <stdexcept>
#include "service/features/node_dispatch/deployment_source.store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("deployment source invariant failed");
}
ruvia::Task<void> verify(ruvia::DbClient& client) {
    co_await client.connect();
    auto tx = co_await client.beginTransaction();
    (void)co_await tx.execute("CREATE TEMP TABLE sys_website (id text, tenant_id text, cluster_id text, revision bigint, status text, config jsonb, sort int, deleted_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_certificate (id text, tenant_id text, subject_alt_names text[], material jsonb, issued_revision bigint, expires_at timestamptz, deleted_at timestamptz) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_website_certificate_binding (website_id text, certificate_id text, tenant_id text, position int) ON COMMIT DROP");
    (void)co_await tx.execute("INSERT INTO sys_website VALUES ('w1','t','c',7,'enabled','{}',2,NULL), ('w2','t','c',3,'disabled','{}',1,NULL), ('foreign','other','c',1,'enabled','{}',0,NULL), ('other-cluster','t','other',1,'enabled','{}',0,NULL), ('deleted','t','c',1,'enabled','{}',0,NOW())");
    (void)co_await tx.execute("INSERT INTO sys_certificate VALUES ('cert','t',ARRAY['*.example.com','example.com'],'{\"certificate_chain_pem\":\"qa-chain\",\"private_key_envelope\":\"qa-envelope\"}',1,NOW()+INTERVAL '1 day',NULL)");
    (void)co_await tx.execute("INSERT INTO sys_website_certificate_binding VALUES ('w1','cert','t',0)");
    const auto source = co_await service::node_dispatch::loadClusterDeploymentSource(tx,"t","c","edge.example.com",true);
    require(source.tenantId == "t" && source.clusterId == "c" && source.accessDomain == "edge.example.com" && source.enabled);
    require(source.websites.size() == 2);
    require(source.websites[0].id == "w2" && !source.websites[0].enabled && source.websites[0].revision == 3);
    require(source.websites[1].id == "w1" && source.websites[1].enabled && source.websites[1].revision == 7);
    const auto& cert = source.certificatesByWebsite.at("w1").at(0);
    require(cert.id == "cert" && cert.domains == std::vector<std::string>{"*.example.com","example.com"});
    require(cert.certificateChainPem == "qa-chain" && cert.privateKeyEnvelope == "qa-envelope");
    for (const auto* change : {"issued_revision=0", "expires_at=NOW()", "deleted_at=NOW()"}) {
        (void)co_await tx.execute(std::string("UPDATE sys_certificate SET ") + change);
        require((co_await service::node_dispatch::loadClusterDeploymentSource(tx,"t","c","edge.example.com",true)).certificatesByWebsite.empty());
        (void)co_await tx.execute("UPDATE sys_certificate SET issued_revision=1,expires_at=NOW()+INTERVAL '1 day',deleted_at=NULL");
    }
    (void)co_await tx.execute("UPDATE sys_website SET revision=NULL WHERE id='w1'");
    bool rejected = false;
    try {
        (void)co_await service::node_dispatch::loadClusterDeploymentSource(tx,"t","c","edge.example.com",true);
    } catch (const std::bad_optional_access&) { rejected = true; }
    require(rejected);
    co_await tx.rollback();
}
}
int main() { return test_support::runPostgresTest(verify); }