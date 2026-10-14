#include "service/domains/agent/heartbeat_store.h"

#include <stdexcept>

#include "service/domains/agent/apply_result_store.h"
#include "service/domains/agent/desired_state_store.h"
#include "service/domains/agent/desired_summary_store.h"
#include "service/domains/agent/release_objects_store.h"
#include "support/postgres_test.h"
namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("heartbeat storage invariant failed");
    }
}
ruvia::task<void> verify(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    auto tx = co_await db.begin_transaction();
    (void)co_await tx.execute("CREATE TEMP TABLE sys_node (id text,tenant_id text,cluster_id text,agent_id text,node_spec_revision bigint,applied_node_spec_revision bigint,registration_status text,active_release_id text,active_manifest_digest text,runtime jsonb,last_heartbeat_at timestamptz,updated_at timestamptz,deleted_at timestamptz,status text,name text,revision bigint) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster_release (id text,tenant_id text,cluster_id text,manifest_digest text) ON COMMIT DROP");
    (void)co_await tx.execute("INSERT INTO sys_node VALUES ('n','t','c','agent',3,2,'registered','r','digest','{}',NULL,NULL,NULL,'enabled','QA',1)");
    (void)co_await tx.execute("INSERT INTO sys_cluster_release VALUES ('r','t','c','digest'),('foreign','t','other','digest')");
    const service::agent::agent_principal principal{.node_id_ = "n", .cluster_id_ = "c", .tenant_id_ = "t", .agent_id_ = "agent"};
    const service::agent::heartbeat_report report{
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
        require(!(co_await service::agent::update_heartbeat(tx, identity, input, "{\"health\":\"ok\"}")));
        const auto after = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
        require(before[0][0].value() == after[0][0].value());
        (void)co_await tx.execute("ROLLBACK TO SAVEPOINT scenario");
        (void)co_await tx.execute("RELEASE SAVEPOINT scenario");
    }
    for (const auto revision : {2, 3}) {
        auto input = report;
        input.applied_node_spec_revision_ = revision;
        const auto update = co_await service::agent::update_heartbeat(tx, principal, input, "{\"health\":\"ok\"}");
        require(update.has_value());
        require(update->status_ == "enabled");
        require(update->became_online_ == (revision == 2));
        const auto rows = co_await tx.query("SELECT applied_node_spec_revision,runtime->>'health',last_heartbeat_at IS NOT NULL FROM sys_node");
        require(rows[0][0].as<int>() == revision && rows[0][1].value() == "ok" && rows[0][2].as<bool>().value());
    }
    auto repeated_report = report;
    repeated_report.applied_node_spec_revision_ = 3;
    auto repeated = co_await service::agent::update_heartbeat(tx, principal, repeated_report,
        "{\"health\":\"ok\"}");
    require(repeated.has_value() && !repeated->became_online_);
    auto stale_report = report;
    stale_report.applied_node_spec_revision_ = 1;
    require(!(co_await service::agent::update_heartbeat(tx, principal, stale_report, "{}")));
    (void)co_await tx.execute("ALTER TABLE sys_node ADD COLUMN desired_release_id text DEFAULT 'r', ADD COLUMN last_apply_phase text DEFAULT 'qa', ADD COLUMN last_apply_error_code text DEFAULT 'qa', ADD COLUMN last_apply_error text DEFAULT 'qa', ADD COLUMN last_apply_retryable boolean DEFAULT true");
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW()");
    const auto deleted_before = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(!(co_await service::agent::record_applied_deployment(tx, principal, 3, "r", "digest")));
    const auto deleted_after = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(deleted_before[0][0].value() == deleted_after[0][0].value());
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NULL");
    require(co_await service::agent::record_applied_deployment(tx, principal, 3, "r", "digest"));
    const auto applied = co_await tx.query("SELECT last_apply_phase IS NULL AND last_apply_error_code IS NULL AND last_apply_error IS NULL AND last_apply_retryable IS NULL FROM sys_node");
    require(applied[0][0].as<bool>().value());
    const service::agent::apply_failure_report failure{.revision_ = 3, .release_id_ = "r", .manifest_digest_ = "digest", .phase_ = "qa-phase", .error_code_ = "qa-code", .error_ = "qa-error", .retryable_ = true};
    const auto success_before = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require((co_await service::agent::record_failed_deployment(tx, principal, failure)) == service::agent::apply_failure_outcome::already_applied);
    const auto success_after = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(success_before[0][0].value() == success_after[0][0].value());
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW(),applied_node_spec_revision=2");
    require((co_await service::agent::record_failed_deployment(tx, principal, failure)) == service::agent::apply_failure_outcome::rejected);
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NULL");
    require((co_await service::agent::record_failed_deployment(tx, principal, failure)) == service::agent::apply_failure_outcome::recorded);
    const auto failed = co_await tx.query("SELECT last_apply_phase,last_apply_error_code,last_apply_error,last_apply_retryable FROM sys_node");
    require(failed[0][0].value() == "qa-phase" && failed[0][1].value() == "qa-code" && failed[0][2].value() == "qa-error" && failed[0][3].as<bool>().value());
    (void)co_await tx.execute("ALTER TABLE sys_node ADD COLUMN config jsonb DEFAULT '{}', ADD COLUMN node_spec_digest text");
    (void)co_await tx.execute("ALTER TABLE sys_cluster_release ADD COLUMN manifest_envelope text DEFAULT 'envelope'");
    const auto state = co_await service::agent::lock_desired_state(tx, principal);
    require(state && state->revision_ == 3 && state->name_ == "QA" && state->enabled_ && state->config_json_ == "{}" && state->release_id_ == "r" && state->manifest_digest_ == "digest" && state->manifest_envelope_ == "envelope");
    require(co_await service::agent::record_node_spec_digest(tx, "t", "n", 3, "spec-digest"));
    const auto digest_before = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(!(co_await service::agent::record_node_spec_digest(tx, "t", "n", 2, "bad")));
    require(!(co_await service::agent::record_node_spec_digest(tx, "other", "n", 3, "bad")));
    require(!(co_await service::agent::record_node_spec_digest(tx, "t", "other", 3, "bad")));
    const auto digest_after = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(digest_before[0][0].value() == digest_after[0][0].value());
    const auto stored_digest = co_await tx.query("SELECT node_spec_digest FROM sys_node");
    require(stored_digest[0][0].value() == "spec-digest");
    const auto summary = co_await service::agent::find_desired_summary(tx, principal);
    require(summary && summary->node_spec_revision_ == 3 && summary->release_id_ == "r" && summary->manifest_digest_ == "digest");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster_release_object (tenant_id text,release_id text,object_digest text,position int) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_delivery_object (tenant_id text,digest_sha256 text,kind text,payload_envelope text) ON COMMIT DROP");
    (void)co_await tx.execute("INSERT INTO sys_cluster_release_object VALUES ('t','r','second',1),('t','r','first',0),('other','r','foreign',0)");
    (void)co_await tx.execute("INSERT INTO sys_delivery_object VALUES ('t','first','website','website-envelope'),('t','second','certificate','certificate-envelope'),('other','foreign','website','foreign-envelope')");
    const auto objects = co_await service::agent::load_release_objects(tx, principal, "r");
    require(objects.size() == 2 && objects[0].digest_ == "first" && objects[0].kind_ == "website" && objects[0].payload_envelope_ == "website-envelope");
    require(objects[1].digest_ == "second" && objects[1].kind_ == "certificate" && objects[1].payload_envelope_ == "certificate-envelope");
    require((co_await service::agent::load_release_objects(tx, principal, "foreign")).empty());
    for (int scenario = 0; scenario < 8; ++scenario) {
        (void)co_await tx.execute("SAVEPOINT summary_scope");
        auto identity = principal;
        if (scenario == 0) {
            identity.tenant_id_ = "other";
        }
        if (scenario == 1) {
            identity.node_id_ = "other";
        }
        if (scenario == 2) {
            identity.agent_id_ = "other";
        }
        if (scenario == 3) {
            identity.cluster_id_ = "other";
        }
        if (scenario == 4) {
            (void)co_await tx.execute("UPDATE sys_node SET desired_release_id='foreign'");
        }
        if (scenario == 5) {
            (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW()");
        }
        if (scenario == 6) {
            (void)co_await tx.execute("UPDATE sys_node SET registration_status='pending'");
        }
        if (scenario == 7) {
            (void)co_await tx.execute("UPDATE sys_node SET cluster_id='other'");
        }
        require((co_await service::agent::load_release_objects(tx, identity, "r")).empty());
        require(!(co_await service::agent::find_desired_summary(tx, identity)));
        require(!(co_await service::agent::lock_desired_state(tx, identity)));
        (void)co_await tx.execute("ROLLBACK TO SAVEPOINT summary_scope");
        (void)co_await tx.execute("RELEASE SAVEPOINT summary_scope");
    }
    co_await tx.rollback();
}
}  // namespace
int main() {
    return test_support::run_postgres_test(verify);
}
