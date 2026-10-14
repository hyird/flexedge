#include <ruvia/core/asio_task.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/web/db/db_client.h>

#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include "service/domains/node/endpoint_claim_store.h"
#include "support/postgres_test.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("endpoint claim invariant failed");
    }
}

ruvia::task<void> verify_body(ruvia::db_client& client, bool& owns_table) {
    co_await client.connect();
    const auto db = client.handle();
    auto setup = co_await db.begin_transaction();
    (void)co_await setup.execute(
        "CREATE TABLE sys_node_endpoint_claim (tenant_id uuid NOT NULL, "
        "node_id uuid NOT NULL, endpoint_id uuid NOT NULL, ip_address inet NOT NULL, "
        "PRIMARY KEY (tenant_id,node_id,endpoint_id), "
        "CONSTRAINT uq_node_endpoint_claim UNIQUE (tenant_id,ip_address))");
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::string other_tenant = "00000000-0000-0000-0000-000000000002";
    const std::string node = "00000000-0000-0000-0000-000000000010";
    const std::string other_node = "00000000-0000-0000-0000-000000000011";
    std::array<service::node_config::node_endpoint_data, 2> endpoints{{
        {"00000000-0000-0000-0000-000000000020", "192.0.2.1", ""},
        {"00000000-0000-0000-0000-000000000021", "2001:db8::1", ""},
    }};
    co_await service::node::replace_endpoint_claims(setup, tenant, node, endpoints);
    co_await service::node::replace_endpoint_claims(setup, other_tenant, node, endpoints);
    auto rows1 = co_await setup.query("SELECT count(*) FROM sys_node_endpoint_claim");
    require(rows1.front()[0].as<int>().value_or(0) == 4);
    std::array<service::node_config::node_endpoint_data, 1> occupied{{
        {"00000000-0000-0000-0000-000000000022", "2001:db8::2", ""},
    }};
    co_await service::node::replace_endpoint_claims(setup, tenant, other_node, occupied);
    co_await setup.commit();
    owns_table = true;
    auto failed_transaction = co_await db.begin_transaction();
    endpoints[1].ip_address_ = "2001:0db8:0:0:0:0:0:2";
    bool conflict = false;
    try {
        co_await service::node::replace_endpoint_claims(failed_transaction, tenant, node, endpoints);
    } catch (const ruvia::db_error& error) {
        if (error.sql_state() != "23505" ||
            error.constraint_name() != "uq_node_endpoint_claim") {
            throw;
        }
        conflict = true;
    }
    require(conflict);
    auto transaction = co_await db.begin_transaction();
    const auto rows2 = co_await transaction.query(
        "SELECT count(*) FROM sys_node_endpoint_claim WHERE tenant_id=$1 AND node_id=$2 "
        "AND ip_address='2001:db8::1'::inet",
        tenant, node);
    require(rows2.front()[0].as<int>().value_or(0) == 1);
    const auto rows3 = co_await transaction.query("SELECT count(*) FROM sys_node_endpoint_claim");
    require(rows3.front()[0].as<int>().value_or(0) == 5);
    endpoints[1].ip_address_ = "2001:db8::3";
    co_await service::node::replace_endpoint_claims(transaction, tenant, node, endpoints);
    const auto rows4 = co_await transaction.query(
        "SELECT count(*) FROM sys_node_endpoint_claim WHERE tenant_id=$1 AND node_id=$2 "
        "AND ip_address='2001:db8::3'::inet",
        tenant, node);
    require(rows4.front()[0].as<int>().value_or(0) == 1);
    co_await service::node::replace_endpoint_claims(transaction, tenant, node, {});
    const auto rows5 = co_await transaction.query("SELECT count(*) FROM sys_node_endpoint_claim");
    require(rows5.front()[0].as<int>().value_or(0) == 3);
    co_await service::node::clear_endpoint_claims(transaction, tenant, other_node);
    const auto rows6 = co_await transaction.query(
        "SELECT count(*) FROM sys_node_endpoint_claim WHERE tenant_id=$1", other_tenant);
    require(rows6.front()[0].as<int>().value_or(0) == 2);
    co_await transaction.rollback();
}
ruvia::task<void> verify(ruvia::db_client& client) {
    bool owns_table = false;
    std::exception_ptr failure;
    try {
        co_await verify_body(client, owns_table);
    } catch (...) {
        failure = std::current_exception();
    }
    if (owns_table) {
        (void)co_await client.handle().execute("DROP TABLE sys_node_endpoint_claim");
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

int main() {
    return test_support::run_postgres_test(verify);
}
