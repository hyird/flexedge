#include <stdexcept>
#include "service/features/node_dispatch/object.store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("delivery object invariant failed");
}
ruvia::Task<void> verify(ruvia::DbClient& client) {
    service::utils::configureSecretKey(std::string(64, 'a'));
    co_await client.connect();
    auto tx = co_await client.beginTransaction();
    (void)co_await tx.execute("CREATE TEMP TABLE sys_delivery_object (tenant_id text, digest_sha256 text, kind text, payload_envelope text, created_at timestamptz, PRIMARY KEY(tenant_id,digest_sha256)) ON COMMIT DROP");
    flexedge::node::v2::DeliveryObject object;
    object.mutable_content()->mutable_website()->set_id("qa-website");
    object.set_digest_sha256(flexedge::node::artifactDigest(object.content()));
    co_await service::node_dispatch::persistObject(tx, "t1", object);
    const auto initial = co_await tx.query("SELECT payload_envelope FROM sys_delivery_object WHERE tenant_id='t1'");
    const std::string envelope(initial.front()[0].value().value());
    const auto serialized = flexedge::node::serializeArtifact(object.content());
    require(envelope != serialized && service::utils::openSecret(envelope) == serialized);
    co_await service::node_dispatch::persistObject(tx, "t1", object);
    co_await service::node_dispatch::persistObject(tx, "t2", object);
    const auto rows = co_await tx.query("SELECT tenant_id,payload_envelope FROM sys_delivery_object ORDER BY tenant_id");
    require(rows.size() == 2 && rows[0][1].value() == envelope);
    auto invalid = object;
    invalid.set_digest_sha256(std::string(64,'0'));
    bool invalidRejected = false;
    try { co_await service::node_dispatch::persistObject(tx, "t1", invalid); }
    catch (const std::runtime_error& error) {
        invalidRejected = std::string_view(error.what()) == "delivery object digest does not match payload";
    }
    require(invalidRejected);
    for (const auto& corruption : {std::string("malformed"), service::utils::sealSecret("different payload")}) {
        (void)co_await tx.execute("UPDATE sys_delivery_object SET payload_envelope=$1 WHERE tenant_id='t1'",corruption);
        bool rejected = false;
        try { co_await service::node_dispatch::persistObject(tx, "t1", object); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected);
    }
    (void)co_await tx.execute("UPDATE sys_delivery_object SET kind='certificate',payload_envelope=$1 WHERE tenant_id='t1'",envelope);
    bool kindRejected = false;
    try { co_await service::node_dispatch::persistObject(tx, "t1", object); }
    catch (const std::runtime_error& error) {
        kindRejected = std::string_view(error.what()) == "content-addressed delivery object conflicts with storage";
    }
    require(kindRejected);
    co_await tx.rollback();
}
}
int main() { return test_support::runPostgresTest(verify); }