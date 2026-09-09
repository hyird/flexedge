#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <ruvia/core/AsioTask.h>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/db/DbClient.h>
#include "service/domains/node/endpoint_claim.store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("endpoint claim invariant failed");
}

ruvia::Task<void> verifyBody(ruvia::DbClient& client, bool& ownsTable) {
    co_await client.connect();
    auto setup = co_await client.beginTransaction();
    (void)co_await setup.execute(
        "CREATE TABLE sys_node_endpoint_claim (tenant_id uuid NOT NULL, "
        "node_id uuid NOT NULL, endpoint_id uuid NOT NULL, ip_address inet NOT NULL, "
        "PRIMARY KEY (tenant_id,node_id,endpoint_id), "
        "CONSTRAINT uq_node_endpoint_claim UNIQUE (tenant_id,ip_address))");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string otherTenant = "00000000-0000-0000-0000-000000000002";
    const std::string node = "00000000-0000-0000-0000-000000000010";
    const std::string otherNode = "00000000-0000-0000-0000-000000000011";
    std::array<service::node_config::NodeEndpointData, 2> endpoints{{
        {"00000000-0000-0000-0000-000000000020", "192.0.2.1", ""},
        {"00000000-0000-0000-0000-000000000021", "2001:db8::1", ""},
    }};
    co_await service::node::replaceEndpointClaims(setup, tenant, node, endpoints);
    co_await service::node::replaceEndpointClaims(setup, otherTenant, node, endpoints);
    auto rows1 = co_await setup.query("SELECT count(*) FROM sys_node_endpoint_claim");
    require(rows1.front()[0].as<int>().value_or(0) == 4);
    std::array<service::node_config::NodeEndpointData, 1> occupied{{
        {"00000000-0000-0000-0000-000000000022", "2001:db8::2", ""},
    }};
    co_await service::node::replaceEndpointClaims(setup, tenant, otherNode, occupied);
    co_await setup.commit();
    ownsTable = true;
    auto failedTransaction = co_await client.beginTransaction();
    endpoints[1].ipAddress = "2001:0db8:0:0:0:0:0:2";
    bool conflict = false;
    try {
        co_await service::node::replaceEndpointClaims(failedTransaction, tenant, node, endpoints);
    } catch (const ruvia::DbError& error) {
        if (error.sqlState() != "23505" ||
            error.constraintName() != "uq_node_endpoint_claim") throw;
        conflict = true;
    }
    require(conflict);
    auto transaction = co_await client.beginTransaction();
    const auto rows2 = co_await transaction.query(
        "SELECT count(*) FROM sys_node_endpoint_claim WHERE tenant_id=$1 AND node_id=$2 "
        "AND ip_address='2001:db8::1'::inet", tenant, node);
    require(rows2.front()[0].as<int>().value_or(0) == 1);
    const auto rows3 = co_await transaction.query("SELECT count(*) FROM sys_node_endpoint_claim");
    require(rows3.front()[0].as<int>().value_or(0) == 5);
    endpoints[1].ipAddress = "2001:db8::3";
    co_await service::node::replaceEndpointClaims(transaction, tenant, node, endpoints);
    const auto rows4 = co_await transaction.query(
        "SELECT count(*) FROM sys_node_endpoint_claim WHERE tenant_id=$1 AND node_id=$2 "
        "AND ip_address='2001:db8::3'::inet", tenant, node);
    require(rows4.front()[0].as<int>().value_or(0) == 1);
    co_await service::node::replaceEndpointClaims(transaction, tenant, node, {});
    const auto rows5 = co_await transaction.query("SELECT count(*) FROM sys_node_endpoint_claim");
    require(rows5.front()[0].as<int>().value_or(0) == 3);
    co_await service::node::clearEndpointClaims(transaction, tenant, otherNode);
    const auto rows6 = co_await transaction.query(
        "SELECT count(*) FROM sys_node_endpoint_claim WHERE tenant_id=$1", otherTenant);
    require(rows6.front()[0].as<int>().value_or(0) == 2);
    co_await transaction.rollback();
}
ruvia::Task<void> verify(ruvia::DbClient& client) {
    bool ownsTable = false;
    std::exception_ptr failure;
    try { co_await verifyBody(client, ownsTable); }
    catch (...) { failure = std::current_exception(); }
    if (ownsTable) {
        (void)co_await client.execute("DROP TABLE sys_node_endpoint_claim");
    }
    if (failure) std::rethrow_exception(failure);
}
} // namespace

int main() { return test_support::runPostgresTest(verify); }
