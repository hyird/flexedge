#include "service/features/node_channel/heartbeat_store.h"

#include <cstdint>
#include <source_location>
#include <stdexcept>

#include "service/features/node_channel/apply_result_store.h"
#include "support/postgres_test.h"
namespace {
void require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        throw std::runtime_error("heartbeat storage invariant failed at line " +
                                 std::to_string(location.line()));
    }
}
ruvia::task<void> verify(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    auto tx = co_await db.begin_transaction();
    const auto clock = co_await tx.query("SELECT (EXTRACT(EPOCH FROM NOW()) * 1000)::bigint");
    auto received_unix_ms = clock[0][0].as<std::int64_t>().value();
    (void)co_await tx.execute("CREATE TEMP TABLE sys_node (id text,tenant_id text,cluster_id text,agent_id text,node_spec_revision bigint,applied_node_spec_revision bigint,registration_status text,active_release_id text,active_manifest_digest text,runtime jsonb,last_heartbeat_at timestamptz,updated_at timestamptz,deleted_at timestamptz,status text,name text,revision bigint) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster_release (id text,tenant_id text,cluster_id text,manifest_digest text) ON COMMIT DROP");
    (void)co_await tx.execute("INSERT INTO sys_node VALUES ('n','t','c','agent',3,2,'registered','r','digest','{}',NULL,NULL,NULL,'enabled','QA',1)");
    (void)co_await tx.execute("INSERT INTO sys_cluster_release VALUES ('r','t','c','digest'),('foreign','t','other','digest')");
    const service::node_channel::agent_principal principal{.node_id_ = "n", .cluster_id_ = "c", .tenant_id_ = "t", .agent_id_ = "agent"};
    const service::node_channel::heartbeat_report report{
        .node_id_ = "n",
        .applied_node_spec_revision_ = 2,
        .active_release_id_ = "r",
        .active_manifest_digest_ = "digest",
        .agent_version_ = {},
        .health_ = {},
        .last_error_ = {},
        .origin_health_ = {},
    };
    for (int scenario = 0; scenario < 8; ++scenario) {
        auto identity = principal;
        auto input = report;
        (void)co_await tx.execute("SAVEPOINT scenario");
        if (scenario == 0) {
            input.applied_node_spec_revision_ = 1;
        }
        if (scenario == 1) {
            input.applied_node_spec_revision_ = 4;
        }
        if (scenario == 2) {
            identity.tenant_id_ = "other";
        }
        if (scenario == 3) {
            identity.agent_id_ = "other";
        }
        if (scenario == 4) {
            input.active_manifest_digest_ = "wrong";
        }
        if (scenario == 5) {
            input.active_release_id_ = "foreign";
        }
        if (scenario == 6) {
            (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW()");
        }
        if (scenario == 7) {
            (void)co_await tx.execute("UPDATE sys_node SET registration_status='pending'");
        }
        const auto before = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
        require(!(co_await service::node_channel::update_heartbeat(tx, identity, input,
            "{\"health\":\"ok\"}", ++received_unix_ms)));
        const auto after = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
        require(before[0][0].value() == after[0][0].value());
        (void)co_await tx.execute("ROLLBACK TO SAVEPOINT scenario");
        (void)co_await tx.execute("RELEASE SAVEPOINT scenario");
    }
    for (const auto revision : {2, 3}) {
        auto input = report;
        input.applied_node_spec_revision_ = revision;
        const auto update = co_await service::node_channel::update_heartbeat(tx, principal, input,
            "{\"health\":\"ok\"}", ++received_unix_ms);
        require(update.has_value());
        require(update->status_ == "enabled");
        require(update->became_online_ == (revision == 2));
        const auto rows = co_await tx.query("SELECT applied_node_spec_revision,runtime->>'health',(EXTRACT(EPOCH FROM last_heartbeat_at) * 1000)::bigint FROM sys_node");
        require(rows[0][0].as<int>() == revision && rows[0][1].value() == "ok" &&
                rows[0][2].as<std::int64_t>() == received_unix_ms);
    }
    auto repeated_report = report;
    repeated_report.applied_node_spec_revision_ = 3;
    const auto before_delayed = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    for (const auto delayed_at : {received_unix_ms, received_unix_ms - 1, received_unix_ms - 30'000}) {
        require(!(co_await service::node_channel::update_heartbeat(tx, principal, repeated_report,
            "{\"health\":\"stale\"}", delayed_at)));
        const auto after_delayed = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
        require(before_delayed[0][0].value() == after_delayed[0][0].value());
    }
    auto repeated = co_await service::node_channel::update_heartbeat(tx, principal, repeated_report,
        "{\"health\":\"ok\"}", ++received_unix_ms);
    require(repeated.has_value() && !repeated->became_online_);
    // Coming online follows the 15 second window of node_connection.h, inside one transaction
    // so both statements share NOW().
    (void)co_await tx.execute("UPDATE sys_node SET last_heartbeat_at = NOW() - INTERVAL '14 seconds'");
    const auto within_window = co_await service::node_channel::update_heartbeat(tx, principal, repeated_report,
        "{\"health\":\"ok\"}", ++received_unix_ms);
    require(within_window.has_value() && !within_window->became_online_);
    (void)co_await tx.execute("UPDATE sys_node SET last_heartbeat_at = NOW() - INTERVAL '16 seconds'");
    const auto after_window = co_await service::node_channel::update_heartbeat(tx, principal, repeated_report,
        "{\"health\":\"ok\"}", ++received_unix_ms);
    require(after_window.has_value() && after_window->became_online_);
    auto stale_report = report;
    stale_report.applied_node_spec_revision_ = 1;
    require(!(co_await service::node_channel::update_heartbeat(tx, principal, stale_report, "{}",
        ++received_unix_ms)));
    (void)co_await tx.execute("ALTER TABLE sys_node ADD COLUMN desired_release_id text DEFAULT 'r', ADD COLUMN last_apply_phase text DEFAULT 'qa', ADD COLUMN last_apply_error_code text DEFAULT 'qa', ADD COLUMN last_apply_error text DEFAULT 'qa', ADD COLUMN last_apply_retryable boolean DEFAULT true");
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW()");
    const auto deleted_before = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(!(co_await service::node_channel::record_applied_deployment(tx, principal, 3, "r", "digest")));
    const auto deleted_after = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(deleted_before[0][0].value() == deleted_after[0][0].value());
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NULL");
    require(co_await service::node_channel::record_applied_deployment(tx, principal, 3, "r", "digest"));
    const auto applied = co_await tx.query("SELECT last_apply_phase IS NULL AND last_apply_error_code IS NULL AND last_apply_error IS NULL AND last_apply_retryable IS NULL FROM sys_node");
    require(applied[0][0].as<bool>().value());
    const service::node_channel::apply_failure_report failure{.revision_ = 3, .release_id_ = "r", .manifest_digest_ = "digest", .phase_ = "qa-phase", .error_code_ = "qa-code", .error_ = "qa-error", .retryable_ = true};
    const auto success_before = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require((co_await service::node_channel::record_failed_deployment(tx, principal, failure)) == service::node_channel::apply_failure_outcome::already_applied);
    const auto success_after = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(success_before[0][0].value() == success_after[0][0].value());
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW(),applied_node_spec_revision=2");
    require((co_await service::node_channel::record_failed_deployment(tx, principal, failure)) == service::node_channel::apply_failure_outcome::rejected);
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NULL");
    require((co_await service::node_channel::record_failed_deployment(tx, principal, failure)) == service::node_channel::apply_failure_outcome::recorded);
    const auto failed = co_await tx.query("SELECT last_apply_phase,last_apply_error_code,last_apply_error,last_apply_retryable FROM sys_node");
    require(failed[0][0].value() == "qa-phase" && failed[0][1].value() == "qa-code" && failed[0][2].value() == "qa-error" && failed[0][3].as<bool>().value());
    co_await tx.rollback();
}
}  // namespace
int main() {
    return test_support::run_postgres_test(verify);
}
