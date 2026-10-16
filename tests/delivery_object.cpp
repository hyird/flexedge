#include <stdexcept>

#include "service/features/node_dispatch/object_store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("delivery object invariant failed");
    }
}
ruvia::task<void> verify(ruvia::db_client& client) {
    service::utils::configure_secret_key(std::string(64, 'a'));
    co_await client.connect();
    auto db = client.handle();
    auto tx = co_await db.begin_transaction();
    (void)co_await tx.execute("CREATE TEMP TABLE sys_delivery_object (tenant_id text, digest_sha256 text, kind text, payload_envelope text, created_at timestamptz, PRIMARY KEY(tenant_id,digest_sha256)) ON COMMIT DROP");
    flexedge::node::v2::DeliveryObjectContent content;
    content.mutable_website()->set_id("qa-website");
    const auto object = flexedge::node::pack_artifact<flexedge::node::v2::DeliveryObject>(content);
    co_await service::node_dispatch::persist_object(tx, "t1", object);
    const auto initial = co_await tx.query("SELECT payload_envelope FROM sys_delivery_object WHERE tenant_id='t1'");
    const std::string envelope(initial.front()[0].value().value());
    const auto serialized = flexedge::node::serialize_artifact(content);
    require(envelope != serialized && service::utils::open_secret(envelope) == serialized);
    co_await service::node_dispatch::persist_object(tx, "t1", object);
    co_await service::node_dispatch::persist_object(tx, "t2", object);
    const auto rows = co_await tx.query("SELECT tenant_id,payload_envelope FROM sys_delivery_object ORDER BY tenant_id");
    require(rows.size() == 2 && rows[0][1].value() == envelope);
    auto invalid = object;
    invalid.set_digest_sha256(std::string(64, '0'));
    bool invalid_rejected = false;
    try {
        co_await service::node_dispatch::persist_object(tx, "t1", invalid);
    } catch (const std::runtime_error& error) {
        invalid_rejected = std::string_view(error.what()) == "delivery object digest does not match payload";
    }
    require(invalid_rejected);
    for (const auto& corruption : {std::string("malformed"), service::utils::seal_secret("different payload")}) {
        (void)co_await tx.execute("UPDATE sys_delivery_object SET payload_envelope=$1 WHERE tenant_id='t1'", corruption);
        bool rejected = false;
        try {
            co_await service::node_dispatch::persist_object(tx, "t1", object);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected);
    }
    (void)co_await tx.execute("UPDATE sys_delivery_object SET kind='certificate',payload_envelope=$1 WHERE tenant_id='t1'", envelope);
    bool kind_rejected = false;
    try {
        co_await service::node_dispatch::persist_object(tx, "t1", object);
    } catch (const std::runtime_error& error) {
        kind_rejected = std::string_view(error.what()) == "content-addressed delivery object conflicts with storage";
    }
    require(kind_rejected);
    co_await tx.rollback();
}
}  // namespace
int main() {
    return test_support::run_postgres_test(verify);
}