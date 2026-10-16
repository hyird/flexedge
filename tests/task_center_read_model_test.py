"""Exercise the actual task-center SQL against a migrated, isolated PostgreSQL database.

Set PGHOST, PGPORT, PGUSER, PGPASSWORD and PGDATABASE, then run this file.
PSQL may point to psql.exe or a wrapper. By default all fixture writes roll back.
--keep-fixture commits the fixture for task_center_http_test.py and browser QA
against the same local database. Never use a production DB.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

if os.environ.get("PGHOST") not in {"127.0.0.1", "localhost", "::1"}:
    raise SystemExit("PGHOST must be loopback")
if not os.environ.get("PGDATABASE", "").endswith("_task_center_qa"):
    raise SystemExit("PGDATABASE must end in _task_center_qa")

# The read model is written in C++ (service/domains/task/task_service.h) as
# raw SQL constants; compose the same statements from their exact text.
root = Path(__file__).resolve().parent.parent
source = (root / "service/domains/task/task_service.h").read_text(encoding="utf-8")
sql = dict(re.findall(r'inline constexpr std::string_view (\w+)\{R"sql\((.*?)\)sql"\};', source, re.S))
compositions = {
    "list": ("tasks_sql", "task_columns_sql", "list_filter_sql", "list_order_sql"),
    "detail": ("tasks_sql", "task_columns_sql", "detail_filter_sql"),
    "history": ("attempts_sql", "history_order_sql"),
    "recorded": ("attempts_sql", "recorded_filter_sql"),
}
statements = {name: "".join(sql[part] for part in parts) for name, parts in compositions.items()}
statements["counts"] = sql["tasks_sql"] + sql["list_counts_sql"].replace("LIST_FILTER", sql["list_filter_sql"])
statements["summary"] = sql["summary_sql"]
statements["trace"] = sql["trace_sql"]

# service::node::online_window_seconds, bound as $2 of every task statement.
online_window_seconds = 15
summary_none = ('{"state":"none","step_count":0,"call_count":0,"log_count":0,"truncated":false,'
                '"failed_step":null,"steps":[]}')


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
SELECT id AS qa_tenant FROM sys_tenant ORDER BY sort LIMIT 1 \gset
CREATE TEMP TABLE qa_context ON COMMIT DROP AS SELECT :'qa_tenant'::uuid AS tenant;
INSERT INTO sys_tenant(id,name,slug) VALUES ('{OTHER}','Other tenant','qa-other');
INSERT INTO sys_provider(id,tenant_id,kind,provider,name,account_id,deleted_at) VALUES
('{P1}', :'qa_tenant', 'dns', 'aliyun', 'QA 100%_dns', 'qa', NOW()),
('{P2}', :'qa_tenant', 'certificate', 'letsencrypt', NULL, NULL, NOW());
INSERT INTO sys_dns_zone(id,tenant_id,provider_id,domain,sync_status,deleted_at)
SELECT ('00000000-0000-4000-8000-00000000020'||i)::uuid, :'qa_tenant', '{P1}', 'qa-'||i||'.example.com',
    CASE i WHEN 3 THEN 'failed' WHEN 4 THEN 'conflict' ELSE 'pending' END, NOW()
FROM generate_series(1,4) i;
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
INSERT INTO sys_cluster_release(id,tenant_id,cluster_id,generation,status,manifest_digest,manifest_envelope,schema_version)
VALUES ('{R1}', :'qa_tenant', '{C1}', 1, 'superseded', repeat('a',64), 'qa-only', 6),
       ('{R2}', :'qa_tenant', '{C1}', 2, 'active', repeat('a',64), 'qa-only', 6);
INSERT INTO sys_node_release_target(tenant_id,release_id,node_id,status,retryable,last_error,applied_at,updated_at) VALUES
(:'qa_tenant', '{R1}', '{N1}', 'failed', true, 'stale failure', NULL, NOW()-INTERVAL '6 hours'),
(:'qa_tenant', '{R2}', '{N1}', 'applied', true, NULL, NOW()-INTERVAL '70 minutes', NOW()-INTERVAL '70 minutes'),
(:'qa_tenant', '{R2}', '{N2}', 'failed', false, 'QA deployment error', NULL, NOW()-INTERVAL '80 minutes'),
(:'qa_tenant', '{R1}', '{N3}', 'failed', false, 'superseded failure', NULL, NOW()-INTERVAL '95 minutes'),
(:'qa_tenant', '{R2}', '{N3}', 'pending', NULL, NULL, NULL, NOW()-INTERVAL '90 minutes');

-- Retries and leases lie a day ahead so the running server never claims them.
INSERT INTO sys_sync_task(tenant_id,resource_type,dns_zone_id,provider_id,operation,version,processed_version,is_done,
    is_ok,count_fails,error,next_attempt_at,lease_owner,lease_until,updated_at) VALUES
(:'qa_tenant', 'dns_zone', '{Z1}', NULL, 'sync', 1, 0, false, false, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, NOW()-INTERVAL '10 minutes'),
(:'qa_tenant', 'dns_zone', '{Z2}', NULL, 'sync_local', 1, 0, false, false, 0, '', NOW()+INTERVAL '1 day', 'qa-lease', NOW()+INTERVAL '1 day', NOW()-INTERVAL '20 minutes'),
(:'qa_tenant', 'dns_zone', '{Z3}', NULL, 'sync', 2, 1, false, false, 2, 'QA: API read failed', NOW()+INTERVAL '1 day', NULL, NULL, NOW()-INTERVAL '30 minutes'),
(:'qa_tenant', 'dns_zone', '{Z4}', NULL, 'sync_remote', 1, 1, true, true, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, NOW()-INTERVAL '40 minutes'),
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
(990004, :'qa_tenant', NULL, 'dns_zone', '{Z4}', 'sync_remote', 1, 'completed', '', 88, 0, 1, '{"kind":"dns_sync","truncated":false,"conflict_count":1}'),
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

task_columns = ("resource_type, resource_id, resource_kind, name, operation, status, warning, error, next_attempt_at, "
                "updated_at, last_success_at, attempt_count, failure_count, node_offline")
attempt_columns = ("id, attempt, operation, outcome, error, started_at, emitted_at, duration_ms, next_attempt_at, "
                   "detail, trace")
summary_columns = "attempt_count, failure_count, first_failure_at, last_success_at"


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


read_model = f"""
PREPARE qa_list(uuid, bigint, text, text, text, bigint, bigint) AS {statements['list']};
PREPARE qa_counts(uuid, bigint, text, text, text) AS {statements['counts']};
PREPARE qa_detail(uuid, bigint, text, text) AS {statements['detail']};
PREPARE qa_history(uuid, text, text, text, bigint) AS {statements['history']};
PREPARE qa_recorded(uuid, text, text, text, text) AS {statements['recorded']};
PREPARE qa_summary(uuid, text, text) AS {statements['summary']};
PREPARE qa_trace(uuid, text, text, bigint) AS {statements['trace']};
CREATE TEMP TABLE qa_tasks ({task_columns}) ON COMMIT DROP AS
    EXECUTE qa_list(:'qa_tenant', {online_window_seconds}, '', '', '', 100, 0);
CREATE TEMP TABLE qa_page ({task_columns}) ON COMMIT DROP AS
    EXECUTE qa_list(:'qa_tenant', {online_window_seconds}, '', '', '', 3, 3);
CREATE TEMP TABLE qa_other ({task_columns}) ON COMMIT DROP AS
    EXECUTE qa_list('{OTHER}', {online_window_seconds}, '', '', '', 100, 0);
CREATE TEMP TABLE qa_detail ({task_columns}) ON COMMIT DROP AS
    EXECUTE qa_detail(:'qa_tenant', {online_window_seconds}, 'node', '{N3}');
CREATE TEMP TABLE qa_missing ({task_columns}) ON COMMIT DROP AS
    EXECUTE qa_detail(:'qa_tenant', {online_window_seconds}, 'node', '{Z1}');
CREATE TEMP TABLE qa_history ({attempt_columns}) ON COMMIT DROP AS
    EXECUTE qa_history(:'qa_tenant', 'dns_zone', '{Z3}', {literal(summary_none)}, 50);
CREATE TEMP TABLE qa_history_limited ({attempt_columns}) ON COMMIT DROP AS
    EXECUTE qa_history(:'qa_tenant', 'dns_zone', '{Z3}', {literal(summary_none)}, 2);
CREATE TEMP TABLE qa_node_history ({attempt_columns}) ON COMMIT DROP AS
    EXECUTE qa_history(:'qa_tenant', 'node', '{N1}', {literal(summary_none)}, 50);
CREATE TEMP TABLE qa_isolated_history ({attempt_columns}) ON COMMIT DROP AS
    EXECUTE qa_history(:'qa_tenant', 'dns_zone', '{Z1}', {literal(summary_none)}, 50);
CREATE TEMP TABLE qa_recorded ({attempt_columns}) ON COMMIT DROP AS
    EXECUTE qa_recorded(:'qa_tenant', 'dns_zone', '{Z3}', {literal(summary_none)}, '990003,990001,990005,990901');
CREATE TEMP TABLE qa_summary ({summary_columns}) ON COMMIT DROP AS EXECUTE qa_summary(:'qa_tenant', 'dns_zone', '{Z3}');
CREATE TEMP TABLE qa_done_summary ({summary_columns}) ON COMMIT DROP AS EXECUTE qa_summary(:'qa_tenant', 'provider', '{P1}');
CREATE TEMP TABLE qa_trace (attempt_id, resource_type, resource_id, kind, attempt, outcome, error, started_at, emitted_at,
    duration_ms, worker_type, capture, steps) ON COMMIT DROP AS EXECUTE qa_trace(:'qa_tenant', 'dns_zone', '{Z3}', 990003);
CREATE TEMP TABLE qa_foreign_trace (attempt_id, resource_type, resource_id, kind, attempt, outcome, error, started_at,
    emitted_at, duration_ms, worker_type, capture, steps) ON COMMIT DROP AS
    EXECUTE qa_trace(:'qa_tenant', 'dns_zone', '{Z1}', 990901);
"""

# (status, type, keyword) -> filtered total; active and failed always count every task.
counts = {
    ("", "", ""): 10, ("queued", "", ""): 1, ("running", "", ""): 1, ("retrying", "", ""): 2, ("failed", "", ""): 2,
    ("completed", "", ""): 3, ("attention_required", "", ""): 1, ("active", "", ""): 4, ("attention", "", ""): 5,
    ("", "dns_zone", ""): 4, ("", "provider", ""): 2, ("", "node", ""): 4, ("", "website", ""): 0,
    ("", "certificate", ""): 0, ("active", "node", ""): 1, ("", "", "%"): 1, ("", "", "_"): 1, ("", "", "'"): 0,
    ("", "", "QA-1.EXAMPLE"): 1, ("", "", "000000000201"): 1, ("", "", "qa node"): 4, ("", "", "LETSENCRYPT"): 1,
}
for index, (key, expected) in enumerate(counts.items()):
    status, type_, keyword = key
    read_model += (f"CREATE TEMP TABLE qa_count_{index} (total, active, failed) ON COMMIT DROP AS EXECUTE "
                   f"qa_counts(:'qa_tenant', {online_window_seconds}, {literal(status)}, {literal(type_)}, "
                   f"{literal(keyword)});\n")
count_checks = "\n".join(
    f" IF (SELECT (total, active, failed) FROM qa_count_{index}) IS DISTINCT FROM ({expected}::bigint, 4::bigint, "
    f"2::bigint) THEN RAISE EXCEPTION 'count case {index}: %', (SELECT (total, active, failed) FROM qa_count_{index}); "
    f"END IF;"
    for index, (key, expected) in enumerate(counts.items()))

assertions = r"""
DO $check$
DECLARE
 task record;
BEGIN
 IF (SELECT count(*) FROM qa_tasks) <> 10 OR EXISTS (SELECT 1 FROM qa_tasks WHERE resource_id IN ('{N5}', '{N6}'))
 THEN RAISE EXCEPTION 'tasks: one per marker and per registered, undeleted node'; END IF;
 IF EXISTS (SELECT 1 FROM qa_other) THEN RAISE EXCEPTION 'tenant isolation'; END IF;
 IF (SELECT string_agg(resource_id || '/' || status, ',') FROM qa_tasks) <>
    '{Z1}/queued,{N3}/retrying,{Z2}/running,{Z3}/retrying,{Z4}/attention_required,{P1}/completed,{P2}/completed,'
    '{N1}/completed,{N2}/failed,{N4}/failed'
 THEN RAISE EXCEPTION 'statuses in updated_at order: %', (SELECT string_agg(resource_id || '/' || status, ',') FROM qa_tasks); END IF;
 IF (SELECT string_agg(resource_id, ',') FROM qa_page) <> '{Z3},{Z4},{P1}' THEN RAISE EXCEPTION 'pagination'; END IF;

 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{Z3}';
 IF (task.operation, task.error, task.attempt_count, task.failure_count, task.warning, task.node_offline)
    IS DISTINCT FROM ('sync'::text, 'QA: API read failed'::text, 3::bigint, 2::bigint, ''::text, false)
    OR task.next_attempt_at = '' OR task.last_success_at <> '' THEN RAISE EXCEPTION 'retrying marker: %', task; END IF;
 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{Z1}';
 IF (task.name, task.attempt_count, task.next_attempt_at, task.error) IS DISTINCT FROM ('qa-1.example.com'::text, 0::bigint, ''::text, ''::text)
 THEN RAISE EXCEPTION 'queued marker: %', task; END IF;
 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{Z2}';
 IF (task.operation, task.next_attempt_at) IS DISTINCT FROM ('sync_local'::text, ''::text) THEN RAISE EXCEPTION 'running marker: %', task; END IF;
 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{Z4}';
 IF (task.warning, task.resource_kind, task.failure_count) IS DISTINCT FROM ('dns_conflict'::text, ''::text, 0::bigint)
    OR task.last_success_at = '' THEN RAISE EXCEPTION 'attention marker: %', task; END IF;
 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{P1}';
 IF (task.name, task.resource_kind, task.attempt_count, task.failure_count, task.error, task.warning)
    IS DISTINCT FROM ('QA 100%_dns'::text, 'dns'::text, 2::bigint, 1::bigint, ''::text, ''::text)
 THEN RAISE EXCEPTION 'recovered marker: %', task; END IF;
 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{P2}';
 IF (task.name, task.resource_kind, task.attempt_count, task.failure_count)
    IS DISTINCT FROM ('letsencrypt'::text, 'certificate'::text, 2::bigint, 0::bigint)
 THEN RAISE EXCEPTION 're-initiated marker counts only its current version: %', task; END IF;

 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{N1}';
 IF (task.name, task.operation, task.attempt_count, task.failure_count, task.error, task.node_offline)
    IS DISTINCT FROM ('QA node 1'::text, 'publish'::text, 3::bigint, 1::bigint, ''::text, false)
    OR task.last_success_at = '' THEN RAISE EXCEPTION 'healthy node: %', task; END IF;
 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{N2}';
 IF (task.operation, task.error, task.node_offline) IS DISTINCT FROM ('publish'::text, 'QA deployment error'::text, true)
 THEN RAISE EXCEPTION 'failed publish: %', task; END IF;
 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{N3}';
 IF (task.operation, task.error, task.failure_count, task.attempt_count, task.node_offline, task.next_attempt_at)
    IS DISTINCT FROM ('configure'::text, 'QA: node rejected config'::text, 1::bigint, 2::bigint, true, ''::text)
 THEN RAISE EXCEPTION 'retrying configure beats a queued publish: %', task; END IF;
 SELECT * INTO task FROM qa_tasks WHERE resource_id = '{N4}';
 IF (task.operation, task.error, task.node_offline) IS DISTINCT FROM ('configure'::text, 'QA: node apply failed'::text, false)
 THEN RAISE EXCEPTION 'failed configure: %', task; END IF;
 IF (SELECT (status::text, operation::text) FROM qa_detail) IS DISTINCT FROM ('retrying'::text, 'configure'::text)
    OR EXISTS (SELECT 1 FROM qa_missing) THEN RAISE EXCEPTION 'detail by type and id'; END IF;

 IF (SELECT string_agg(id || '/' || attempt || '/' || operation || '/' || outcome, ',') FROM qa_history)
    <> '990003/2/sync/failed,990002/1/sync/failed,990001/1/sync/failed' THEN RAISE EXCEPTION 'history newest first'; END IF;
 IF (SELECT count(*) FROM qa_history_limited) <> 2 THEN RAISE EXCEPTION 'history limit'; END IF;
 IF (SELECT string_agg((trace::jsonb->>'attempt_id') || '/' || (trace::jsonb->>'state'), ',') FROM qa_history)
    <> '990003/available,990002/none,990001/none' THEN RAISE EXCEPTION 'trace summaries'; END IF;
 IF (SELECT (duration_ms, started_at <> '', detail::jsonb) FROM qa_history WHERE id = '990002')
    IS DISTINCT FROM (2000::bigint, true, '{"kind":"none","truncated":false}'::jsonb) THEN RAISE EXCEPTION 'attempt fields'; END IF;
 IF (SELECT string_agg(operation, ',') FROM qa_node_history) <> 'publish,publish,publish' THEN RAISE EXCEPTION 'node history'; END IF;
 IF EXISTS (SELECT 1 FROM qa_isolated_history) THEN RAISE EXCEPTION 'history tenant isolation'; END IF;
 IF (SELECT string_agg(id, ',') FROM qa_recorded) <> '990001,990003' THEN RAISE EXCEPTION 'recorded attempts of this task only, oldest first'; END IF;
 IF (SELECT (attempt_count, failure_count, last_success_at) FROM qa_summary)
    IS DISTINCT FROM (3::bigint, 3::bigint, ''::text)
    OR (SELECT first_failure_at FROM qa_summary) <> (SELECT emitted_at FROM qa_history WHERE id = '990001')
 THEN RAISE EXCEPTION 'retrying summary'; END IF;
 IF (SELECT (attempt_count, failure_count, last_success_at <> '') FROM qa_done_summary)
    IS DISTINCT FROM (2::bigint, 1::bigint, true) THEN RAISE EXCEPTION 'completed summary'; END IF;
 IF (SELECT (attempt_id, resource_type, resource_id, kind, attempt, outcome, worker_type, (capture::jsonb->>'bytes')::int)
     FROM qa_trace) IS DISTINCT FROM ('990003'::text, 'dns_zone'::varchar, '{Z3}'::text, 'none'::text, 2::integer,
     'failed'::varchar, 'dns-sync'::text, 64) THEN RAISE EXCEPTION 'trace'; END IF;
 IF EXISTS (SELECT 1 FROM qa_foreign_trace) THEN RAISE EXCEPTION 'trace tenant isolation'; END IF;
COUNT_CHECKS
END;
$check$;
"""
for key, value in ids.items():
    assertions = assertions.replace("{" + key + "}", value)
assertions = assertions.replace("COUNT_CHECKS", count_checks)

script = fixture + read_model + assertions
script += "COMMIT;" if "--keep-fixture" in sys.argv else "ROLLBACK;"
result = subprocess.run([os.environ.get("PSQL", "psql"), "-X", "-q", "-v", "ON_ERROR_STOP=1"],
                        input=script, encoding="utf-8", capture_output=True)
if result.returncode:
    raise SystemExit(result.stderr)
print("Task read-model integration passed: marker and node statuses, current-work failure counts, warnings, "
      "updated_at order, pagination, filters, counts, history, recorded attempts, summaries, traces, tenant isolation.")
