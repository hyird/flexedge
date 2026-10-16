"""Prepare deterministic data for task_center_http_test.py and local browser QA.

The QA server must first install the current baseline and create its admin/tenant.
Point PG* at its dedicated loopback database ending in _task_center_qa. A dry run
rolls back by default; --keep-fixture commits these rows for the running QA server.
This script only creates fixtures. Production Query Object behavior is exercised
through the C++ task_center/task_history/attempt_trace_store CTests and HTTP API.
"""
import argparse
import os
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--keep-fixture", action="store_true", help="commit the isolated HTTP/browser QA fixture")
arguments = parser.parse_args()
if os.environ.get("PGHOST") != "127.0.0.1":
    raise SystemExit("PGHOST must be 127.0.0.1")
if not os.environ.get("PGDATABASE", "").endswith("_task_center_qa"):
    raise SystemExit("PGDATABASE must end in _task_center_qa")

def uid(n):
    return f"00000000-0000-4000-8000-{n:012d}"


Z1, Z2, Z3, Z4 = (uid(n) for n in (201, 202, 203, 204))
P1, P2 = uid(101), uid(102)
C1 = uid(401)
N1, N2, N3, N4, N5, N6 = (uid(n) for n in range(501, 507))
R1, R2 = uid(601), uid(602)
OTHER = uid(701)

fixture = r"""
BEGIN;
DO $guard$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = 'sys_sync_task'::regclass
        AND attname = 'resource_id' AND attgenerated = 's')
        OR NOT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = 'sys_sync_task'::regclass
        AND attname = 'wait_task_id') THEN
        RAISE EXCEPTION 'task fixture requires the current fresh-install baseline';
    END IF;
    IF (SELECT count(*) FROM sys_tenant WHERE status = 'enabled' AND deleted_at IS NULL) <> 1 THEN
        RAISE EXCEPTION 'task fixture requires exactly one enabled QA tenant';
    END IF;
    IF EXISTS (SELECT 1 FROM sys_sync_task) OR EXISTS (SELECT 1 FROM sys_sync_event)
        OR EXISTS (SELECT 1 FROM sys_node) THEN
        RAISE EXCEPTION 'task fixture requires an empty QA task and node database';
    END IF;
END;
$guard$;
SELECT id AS qa_tenant FROM sys_tenant WHERE status = 'enabled' AND deleted_at IS NULL ORDER BY sort LIMIT 1 \gset
CREATE TEMP TABLE qa_context ON COMMIT DROP AS SELECT :'qa_tenant'::uuid AS tenant;
INSERT INTO sys_tenant(id,name,slug) VALUES ('{OTHER}','Other tenant','qa-other');
INSERT INTO sys_provider(id,tenant_id,kind,provider,name,account_id,deleted_at) VALUES
('{P1}', :'qa_tenant', 'dns', 'aliyun', 'QA 100%_dns', 'qa', NOW()),
('{P2}', :'qa_tenant', 'certificate', 'letsencrypt', NULL, NULL, NOW());
INSERT INTO sys_dns_zone(id,tenant_id,provider_id,domain,sync_status,deleted_at)
SELECT ('00000000-0000-4000-8000-00000000020'||i)::uuid, :'qa_tenant', '{P1}', 'qa-'||i||'.example.com',
    CASE i WHEN 3 THEN 'failed' WHEN 4 THEN 'synced' ELSE 'pending' END, NOW()
FROM generate_series(1,4) i;
-- One live zone allows the final node-edit/SSE regression. Its provider
-- remains deleted, so no fixture can reach an external DNS API.
UPDATE sys_dns_zone SET deleted_at=NULL,
    runtime='{"lines":[{"code":"default","name":"Default","display_name":"Default","status":"enabled"}],"record_states":[],"challenge_records":[]}',
    next_check_at=NOW()+INTERVAL '1 day' WHERE id='{Z1}';
INSERT INTO sys_cluster(id,tenant_id,dns_zone_id,name,hostname_prefix)
VALUES ('{C1}', :'qa_tenant', '{Z1}', 'QA cluster', 'qa');
-- N1 healthy, N2 failed publish (offline), N3 retrying configure (never seen),
-- N4 failed configure (online); N5 pending registration and N6 deleted are no tasks.
INSERT INTO sys_node(id,tenant_id,cluster_id,name,registration_status,agent_id,node_secret_hash,node_secret_envelope,
    registered_at,last_heartbeat_at,node_spec_revision,applied_node_spec_revision,updated_at,deleted_at)
SELECT n.id::uuid, :'qa_tenant', '{C1}', n.name, n.registration, repeat('a',31)||n.i, repeat('c',64), 'qa-only',
    CASE WHEN n.registration = 'registered' THEN NOW() - n.registered * INTERVAL '1 day' END, n.heartbeat,
    n.spec, n.applied, NOW(), n.deleted
FROM (VALUES
(1, '{N1}', 'QA node 1', 'registered', 3, NOW(), 1, 1, NULL::timestamptz),
(2, '{N2}', 'QA node 2', 'registered', 3, NOW() - INTERVAL '1 hour', 1, 1, NULL),
(3, '{N3}', 'QA node 3', 'registered', 5, NULL, 3, 1, NULL),
(4, '{N4}', 'QA node 4', 'registered', 2, NOW(), 2, 1, NULL),
(5, '{N5}', 'QA node 5', 'pending', 0, NULL, 1, 0, NULL),
(6, '{N6}', 'QA node 6', 'registered', 1, NULL, 1, 0, NOW())
) AS n(i, id, name, registration, registered, heartbeat, spec, applied, deleted);
UPDATE sys_node SET config_task_revision=3, config_task_error='QA: node rejected config',
    config_task_retryable=true, config_task_failures=1, config_task_updated_at=NOW()-INTERVAL '15 minutes'
    WHERE id='{N3}';
UPDATE sys_node SET config='{"endpoints":[{"id":"00000000-0000-4000-8000-000000000801","ip_address":"192.0.2.3","line_code":"default"}]}'
    WHERE id='{N3}';
UPDATE sys_node SET config_task_revision=2, config_task_error='QA: node apply failed',
    config_task_retryable=false, config_task_failures=1, config_task_updated_at=NOW()-INTERVAL '100 minutes'
    WHERE id='{N4}';
INSERT INTO sys_cluster_release(id,tenant_id,cluster_id,generation,status,manifest_digest,manifest_envelope,schema_version)
VALUES ('{R1}', :'qa_tenant', '{C1}', 1, 'superseded', repeat('a',64), 'qa-only', 6),
       ('{R2}', :'qa_tenant', '{C1}', 2, 'active', repeat('a',64), 'qa-only', 6);
UPDATE sys_cluster SET release_generation=2 WHERE id='{C1}';
INSERT INTO sys_node_release_target(tenant_id,release_id,node_id,status,retryable,last_error,applied_at,updated_at) VALUES
(:'qa_tenant', '{R1}', '{N1}', 'failed', true, 'stale failure', NULL, NOW()-INTERVAL '6 hours'),
(:'qa_tenant', '{R2}', '{N1}', 'applied', true, NULL, NOW()-INTERVAL '70 minutes', NOW()-INTERVAL '70 minutes'),
(:'qa_tenant', '{R2}', '{N2}', 'failed', false, 'QA deployment error', NULL, NOW()-INTERVAL '80 minutes'),
(:'qa_tenant', '{R1}', '{N3}', 'failed', false, 'superseded failure', NULL, NOW()-INTERVAL '95 minutes'),
(:'qa_tenant', '{R2}', '{N3}', 'pending', NULL, NULL, NULL, NOW()-INTERVAL '90 minutes');
UPDATE sys_node_release_target SET count_fails=1 WHERE status='failed';

-- Retries and leases lie a day ahead so the running server never claims them.
INSERT INTO sys_sync_task(tenant_id,resource_type,dns_zone_id,provider_id,operation,version,processed_version,is_done,
    is_ok,count_fails,error,next_attempt_at,lease_owner,lease_until,updated_at) VALUES
(:'qa_tenant', 'dns_zone', '{Z1}', NULL, 'sync', 1, 0, false, false, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, NOW()-INTERVAL '10 minutes'),
(:'qa_tenant', 'dns_zone', '{Z2}', NULL, 'sync', 1, 0, false, false, 0, '', NOW()+INTERVAL '1 day', 'qa-lease', NOW()+INTERVAL '1 day', NOW()-INTERVAL '20 minutes'),
(:'qa_tenant', 'dns_zone', '{Z3}', NULL, 'sync', 2, 1, false, false, 2, 'QA: API read failed', NOW()+INTERVAL '1 day', NULL, NULL, NOW()-INTERVAL '30 minutes'),
(:'qa_tenant', 'dns_zone', '{Z4}', NULL, 'sync', 1, 1, true, true, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, NOW()-INTERVAL '40 minutes'),
(:'qa_tenant', 'provider', NULL, '{P1}', 'verify', 1, 1, true, true, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, NOW()-INTERVAL '50 minutes'),
(:'qa_tenant', 'provider', NULL, '{P2}', 'verify', 2, 2, true, true, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, NOW()-INTERVAL '60 minutes');

INSERT INTO sys_sync_event(id,tenant_id,task_id,resource_type,resource_id,operation,version,outcome,error,started_at,emitted_at,
    duration_ms,attempt,detail) OVERRIDING SYSTEM VALUE
SELECT e.id, e.tenant::uuid, COALESCE(e.task::uuid, (SELECT marker.id FROM sys_sync_task marker WHERE
        marker.tenant_id = e.tenant::uuid AND marker.resource_id = e.resource::uuid), e.resource::uuid),
    e.type, e.resource::uuid, e.operation, e.version, e.outcome, e.error,
    CASE WHEN e.duration > 0 THEN NOW() - e.minutes * INTERVAL '1 minute' - e.duration * INTERVAL '1 millisecond' END,
    NOW() - e.minutes * INTERVAL '1 minute', e.duration, e.attempt, e.detail::jsonb
FROM (VALUES
(990001, :'qa_tenant', NULL, 'dns_zone', '{Z3}', 'sync', 1, 'failed', 'QA: old version', 300, 0, 1, '{"kind":"none","truncated":false}'),
(990002, :'qa_tenant', NULL, 'dns_zone', '{Z3}', 'sync', 2, 'failed', 'QA: API timeout', 29, 2000, 1, '{"kind":"none","truncated":false}'),
(990003, :'qa_tenant', NULL, 'dns_zone', '{Z3}', 'sync', 2, 'failed', 'QA: API read failed', 25, 1000, 2, '{"kind":"none","truncated":false}'),
(990004, :'qa_tenant', NULL, 'dns_zone', '{Z4}', 'sync', 1, 'completed', '', 88, 0, 1, '{"kind":"dns_sync","truncated":false}'),
(990005, :'qa_tenant', NULL, 'provider', '{P1}', 'verify', 1, 'failed', 'QA: credentials rejected', 55, 0, 1, '{"kind":"none","truncated":false}'),
(990006, :'qa_tenant', NULL, 'provider', '{P1}', 'verify', 1, 'completed', '', 52, 0, 2, '{"kind":"none","truncated":false}'),
(990007, :'qa_tenant', NULL, 'provider', '{P2}', 'verify', 1, 'failed', 'QA: before re-initiation', 65, 0, 1, '{"kind":"none","truncated":false}'),
(990008, :'qa_tenant', NULL, 'provider', '{P2}', 'verify', 2, 'completed', '', 62, 0, 1, '{"kind":"none","truncated":false}'),
(990009, :'qa_tenant', '{R1}', 'node', '{N1}', 'publish', 1, 'failed', 'QA: stale publish', 360, 0, 1, '{"kind":"none","truncated":false}'),
(990010, :'qa_tenant', '{R2}', 'node', '{N1}', 'publish', 2, 'failed', 'QA: publish failed', 75, 0, 1, '{"kind":"none","truncated":false}'),
(990011, :'qa_tenant', '{R2}', 'node', '{N1}', 'publish', 2, 'completed', '', 72, 0, 2, '{"kind":"none","truncated":false}'),
(990012, :'qa_tenant', '{N3}', 'node', '{N3}', 'configure', 2, 'failed', 'QA: old revision', 200, 0, 1, '{"kind":"node_apply","truncated":false,"retryable":false}'),
(990013, :'qa_tenant', '{N3}', 'node', '{N3}', 'configure', 3, 'failed', 'QA: node rejected config', 15, 0, 1, '{"kind":"node_apply","truncated":false,"retryable":true}'),
(990014, :'qa_tenant', '{N4}', 'node', '{N4}', 'configure', 2, 'failed', 'QA: node apply failed', 100, 0, 1, '{"kind":"node_apply","truncated":false,"retryable":false}'),
(990901, '{OTHER}', '{Z1}', 'dns_zone', '{Z1}', 'sync', 1, 'failed', 'must not leak', 4, 0, 1, '{"kind":"none","truncated":false}')
) AS e(id, tenant, task, type, resource, operation, version, outcome, error, minutes, duration, attempt, detail);
INSERT INTO sys_sync_event_trace(event_id,tenant_id,summary,trace,trace_bytes) VALUES
(990003, :'qa_tenant', '{"state":"available","step_count":0,"call_count":0,"log_count":0,"truncated":false,"failed_step":null,"steps":[]}',
 '{"worker_type":"dns-sync","capture":{"truncated":false,"degraded":false,"bytes":64,"omitted_steps":0,"omitted_calls":0,"omitted_logs":0,"omitted_body_bytes":0,"redactions":0},"steps":[]}', 64);
"""
ids = {"Z1": Z1, "Z2": Z2, "Z3": Z3, "Z4": Z4, "P1": P1, "P2": P2, "C1": C1, "N1": N1, "N2": N2, "N3": N3,
       "N4": N4, "N5": N5, "N6": N6, "R1": R1, "R2": R2, "OTHER": OTHER}
for key, value in ids.items():
    fixture = fixture.replace("{" + key + "}", value)

script = fixture + ("COMMIT;" if arguments.keep_fixture else "ROLLBACK;")
result = subprocess.run([os.environ.get("PSQL", "psql"), "-X", "-q", "-v", "ON_ERROR_STOP=1"],
                        input=script, encoding="utf-8", capture_output=True)
if result.returncode:
    raise SystemExit(result.stderr)
print("Task-center HTTP/browser QA fixture " + ("committed." if arguments.keep_fixture else "validated and rolled back."))
