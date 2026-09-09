#include <stdexcept>
#include "service/domains/agent/release_objects.store.h"
#include "service/domains/agent/desired_state.store.h"
#include "service/domains/agent/desired_summary.store.h"
#include "service/domains/agent/heartbeat.store.h"
#include "service/domains/agent/apply_result.store.h"
#include "support/postgres_test.h"
namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("heartbeat storage invariant failed");
}
ruvia::Task<void> verify(ruvia::DbClient& client) {
    co_await client.connect();
    auto tx = co_await client.beginTransaction();
    (void)co_await tx.execute("CREATE TEMP TABLE sys_node (id text,tenant_id text,cluster_id text,agent_id text,node_spec_revision bigint,applied_node_spec_revision bigint,registration_status text,active_release_id text,active_manifest_digest text,runtime jsonb,last_heartbeat_at timestamptz,updated_at timestamptz,deleted_at timestamptz,status text,name text,revision bigint) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster_release (id text,tenant_id text,cluster_id text,manifest_digest text) ON COMMIT DROP");
    (void)co_await tx.execute("INSERT INTO sys_node VALUES ('n','t','c','agent',3,2,'registered','r','digest','{}',NULL,NULL,NULL,'enabled','QA',1)");
    (void)co_await tx.execute("INSERT INTO sys_cluster_release VALUES ('r','t','c','digest'),('foreign','t','other','digest')");
    const service::agent::AgentPrincipal principal{.nodeId="n",.clusterId="c",.tenantId="t",.agentId="agent"};
    const service::agent::HeartbeatReport report{
        .nodeId = "n",
        .appliedNodeSpecRevision = 2,
        .activeReleaseId = "r",
        .activeManifestDigest = "digest",
        .agentVersion = {},
        .health = {},
        .lastError = {},
        .originHealth = {},
    };
    for (int scenario=0; scenario<8; ++scenario) {
        auto identity = principal;
        auto input = report;
        (void)co_await tx.execute("SAVEPOINT scenario");
        if (scenario==0) input.appliedNodeSpecRevision=1;
        if (scenario==1) input.appliedNodeSpecRevision=4;
        if (scenario==2) identity.tenantId="other";
        if (scenario==3) identity.agentId="other";
        if (scenario==4) input.activeManifestDigest="wrong";
        if (scenario==5) input.activeReleaseId="foreign";
        if (scenario==6) (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW()");
        if (scenario==7) (void)co_await tx.execute("UPDATE sys_node SET registration_status='pending'");
        const auto before = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
        require(!(co_await service::agent::updateHeartbeat(tx,identity,input,"{\"health\":\"ok\"}")));
        const auto after = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
        require(before[0][0].value() == after[0][0].value());
        (void)co_await tx.execute("ROLLBACK TO SAVEPOINT scenario");
        (void)co_await tx.execute("RELEASE SAVEPOINT scenario");
    }
    for (const auto revision : {2,3}) {
        auto input = report;
        input.appliedNodeSpecRevision=revision;
        const auto update = co_await service::agent::updateHeartbeat(tx,principal,input,"{\"health\":\"ok\"}");
        require(update.has_value());
        require(update->status == "enabled");
        require(update->becameOnline == (revision == 2));
        const auto rows = co_await tx.query("SELECT applied_node_spec_revision,runtime->>'health',last_heartbeat_at IS NOT NULL FROM sys_node");
        require(rows[0][0].as<int>() == revision && rows[0][1].value() == "ok" && rows[0][2].as<bool>().value());
    }
    auto repeatedReport = report;
    repeatedReport.appliedNodeSpecRevision = 3;
    auto repeated = co_await service::agent::updateHeartbeat(tx, principal, repeatedReport,
                                                              "{\"health\":\"ok\"}");
    require(repeated.has_value() && !repeated->becameOnline);
    auto staleReport = report;
    staleReport.appliedNodeSpecRevision = 1;
    require(!(co_await service::agent::updateHeartbeat(tx, principal, staleReport, "{}")));
    (void)co_await tx.execute("ALTER TABLE sys_node ADD COLUMN desired_release_id text DEFAULT 'r', ADD COLUMN last_apply_phase text DEFAULT 'qa', ADD COLUMN last_apply_error_code text DEFAULT 'qa', ADD COLUMN last_apply_error text DEFAULT 'qa', ADD COLUMN last_apply_retryable boolean DEFAULT true");
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW()");
    const auto deletedBefore = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(!(co_await service::agent::recordAppliedDeployment(tx,principal,3,"r","digest")));
    const auto deletedAfter = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(deletedBefore[0][0].value() == deletedAfter[0][0].value());
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NULL");
    require(co_await service::agent::recordAppliedDeployment(tx,principal,3,"r","digest"));
    const auto applied = co_await tx.query("SELECT last_apply_phase IS NULL AND last_apply_error_code IS NULL AND last_apply_error IS NULL AND last_apply_retryable IS NULL FROM sys_node");
    require(applied[0][0].as<bool>().value());
    const service::agent::ApplyFailureReport failure{.revision=3,.releaseId="r",.manifestDigest="digest",
        .phase="qa-phase",.errorCode="qa-code",.error="qa-error",.retryable=true};
    const auto successBefore = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require((co_await service::agent::recordFailedDeployment(tx,principal,failure)) == service::agent::ApplyFailureOutcome::alreadyApplied);
    const auto successAfter = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(successBefore[0][0].value() == successAfter[0][0].value());
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW(),applied_node_spec_revision=2");
    require((co_await service::agent::recordFailedDeployment(tx,principal,failure)) == service::agent::ApplyFailureOutcome::rejected);
    (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NULL");
    require((co_await service::agent::recordFailedDeployment(tx,principal,failure)) == service::agent::ApplyFailureOutcome::recorded);
    const auto failed = co_await tx.query("SELECT last_apply_phase,last_apply_error_code,last_apply_error,last_apply_retryable FROM sys_node");
    require(failed[0][0].value() == "qa-phase" && failed[0][1].value() == "qa-code" && failed[0][2].value() == "qa-error" && failed[0][3].as<bool>().value());
    (void)co_await tx.execute("ALTER TABLE sys_node ADD COLUMN config jsonb DEFAULT '{}', ADD COLUMN node_spec_digest text");
    (void)co_await tx.execute("ALTER TABLE sys_cluster_release ADD COLUMN manifest_envelope text DEFAULT 'envelope'");
    const auto state = co_await service::agent::lockDesiredState(tx,principal);
    require(state && state->revision == 3 && state->name == "QA" && state->enabled && state->configJson == "{}" && state->releaseId == "r" && state->manifestDigest == "digest" && state->manifestEnvelope == "envelope");
    require(co_await service::agent::recordNodeSpecDigest(tx,"t","n",3,"spec-digest"));
    const auto digestBefore = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(!(co_await service::agent::recordNodeSpecDigest(tx,"t","n",2,"bad")));
    require(!(co_await service::agent::recordNodeSpecDigest(tx,"other","n",3,"bad")));
    require(!(co_await service::agent::recordNodeSpecDigest(tx,"t","other",3,"bad")));
    const auto digestAfter = co_await tx.query("SELECT row_to_json(n)::text FROM sys_node n");
    require(digestBefore[0][0].value() == digestAfter[0][0].value());
    const auto storedDigest = co_await tx.query("SELECT node_spec_digest FROM sys_node");
    require(storedDigest[0][0].value() == "spec-digest");
    const auto summary = co_await service::agent::findDesiredSummary(tx,principal);
    require(summary && summary->nodeSpecRevision == 3 && summary->releaseId == "r" && summary->manifestDigest == "digest");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_cluster_release_object (tenant_id text,release_id text,object_digest text,position int) ON COMMIT DROP");
    (void)co_await tx.execute("CREATE TEMP TABLE sys_delivery_object (tenant_id text,digest_sha256 text,kind text,payload_envelope text) ON COMMIT DROP");
    (void)co_await tx.execute("INSERT INTO sys_cluster_release_object VALUES ('t','r','second',1),('t','r','first',0),('other','r','foreign',0)");
    (void)co_await tx.execute("INSERT INTO sys_delivery_object VALUES ('t','first','website','website-envelope'),('t','second','certificate','certificate-envelope'),('other','foreign','website','foreign-envelope')");
    const auto objects = co_await service::agent::loadReleaseObjects(tx,principal,"r");
    require(objects.size() == 2 && objects[0].digest == "first" && objects[0].kind == "website" && objects[0].payloadEnvelope == "website-envelope");
    require(objects[1].digest == "second" && objects[1].kind == "certificate" && objects[1].payloadEnvelope == "certificate-envelope");
    require((co_await service::agent::loadReleaseObjects(tx,principal,"foreign")).empty());
    for (int scenario=0; scenario<8; ++scenario) {
        (void)co_await tx.execute("SAVEPOINT summary_scope");
        auto identity = principal;
        if (scenario==0) identity.tenantId="other";
        if (scenario==1) identity.nodeId="other";
        if (scenario==2) identity.agentId="other";
        if (scenario==3) identity.clusterId="other";
        if (scenario==4) (void)co_await tx.execute("UPDATE sys_node SET desired_release_id='foreign'");
        if (scenario==5) (void)co_await tx.execute("UPDATE sys_node SET deleted_at=NOW()");
        if (scenario==6) (void)co_await tx.execute("UPDATE sys_node SET registration_status='pending'");
        if (scenario==7) (void)co_await tx.execute("UPDATE sys_node SET cluster_id='other'");
        require((co_await service::agent::loadReleaseObjects(tx,identity,"r")).empty());
        require(!(co_await service::agent::findDesiredSummary(tx,identity)));
        require(!(co_await service::agent::lockDesiredState(tx,identity)));
        (void)co_await tx.execute("ROLLBACK TO SAVEPOINT summary_scope");
        (void)co_await tx.execute("RELEASE SAVEPOINT summary_scope");
    }
    co_await tx.rollback();
}
}
int main() { return test_support::runPostgresTest(verify); }
