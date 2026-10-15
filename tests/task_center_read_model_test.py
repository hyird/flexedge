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


def op(n):
    return f"00000000-0000-4000-8000-0000000a{n:02d}00"


def sub(n, i):
    return f"00000000-0000-4000-8000-0000000b{n:02d}{i:02d}"


# The read model is composed in C++ (service/domains/task/task_service.h);
# rebuild the exact text of operation_sql()/subtask_sql()/list_filter_sql.
root = Path(__file__).resolve().parent.parent
source = (root / "service/domains/task/task_service.h").read_text(encoding="utf-8")


def resource_deleted_sql(kind, ident):
    live = lambda table: (f"NOT EXISTS (SELECT 1 FROM {table} live WHERE live.tenant_id = $1 AND "
                          f"live.id = {ident} AND live.deleted_at IS NULL)")
    tables = ("website", "cluster", "node", "dns_zone", "certificate", "provider")
    return ("CASE " + kind + "".join(f" WHEN '{t}' THEN {live('sys_' + t)}" for t in tables)
            + " ELSE FALSE END")


def static_sql(name):
    body = re.search(r"const std::string& " + name + r"\(\) \{\s*static const std::string sql =(.*?);\s*return sql;",
                     source, re.S)
    return evaluate(body.group(1))


calls = {
    "std::string": lambda value: value,
    "subtask_sql": lambda: static_sql("subtask_sql"),
    "resource_deleted_sql": resource_deleted_sql,
    # Rendered by ruvia from service::node::heartbeat_within_online_window.
    "service::node::node_heartbeat_recent_sql": lambda: "node.last_heartbeat_at >= (now() - CAST('15 seconds' AS interval))",
}


def evaluate(expression):
    """Concatenation of string literals and the known helper calls above."""
    tokens = re.findall(r'"(?:[^"\\]|\\.)*"|[A-Za-z_][\w:]*|[()+,]|\S', expression)
    position = 0

    def term():
        nonlocal position
        token = tokens[position]
        if token.startswith('"'):
            text = ""
            while position < len(tokens) and tokens[position].startswith('"'):
                text += tokens[position][1:-1].replace('\\"', '"').replace("\\\\", "\\")
                position += 1
            return text
        if token not in calls or tokens[position + 1] != "(":
            raise SystemExit(f"task_service.h composes SQL with unknown {token!r}; update this test")
        position += 2
        args = []
        while tokens[position] != ")":
            args.append(concatenation())
            if tokens[position] == ",":
                position += 1
        position += 1
        return calls[token](*args)

    def concatenation():
        nonlocal position
        text = term()
        while position < len(tokens) and tokens[position] == "+":
            position += 1
            text += term()
        return text

    text = concatenation()
    if position != len(tokens):
        raise SystemExit(f"Unparsed SQL expression tail: {tokens[position:]}")
    return text


operation_sql = static_sql("operation_sql")
subtask_sql = static_sql("subtask_sql")
list_filter_sql = evaluate(re.search(r"list_filter_sql\{(.*?)\};", source, re.S).group(1))

fixture = r"""
BEGIN;
SELECT id AS qa_tenant FROM sys_tenant ORDER BY sort LIMIT 1 \gset
SELECT id AS qa_admin FROM sys_admin WHERE username = 'admin' \gset
CREATE TEMP TABLE qa_context ON COMMIT DROP AS SELECT :'qa_tenant'::uuid AS tenant, :'qa_admin'::text AS admin;
INSERT INTO sys_tenant(id,name,slug) VALUES ('00000000-0000-4000-8000-000000000701','Other tenant','qa-other');
INSERT INTO sys_provider(id,tenant_id,kind,provider,name,account_id,deleted_at) VALUES
('00000000-0000-4000-8000-000000000101', :'qa_tenant', 'dns', 'aliyun', 'QA 100%_dns', 'qa', NOW()),
('00000000-0000-4000-8000-000000000102', :'qa_tenant', 'certificate', 'letsencrypt', NULL, NULL, NOW());
INSERT INTO sys_dns_zone(id,tenant_id,provider_id,domain,sync_status,deleted_at)
SELECT ('00000000-0000-4000-8000-00000000020'||i)::uuid, :'qa_tenant', '00000000-0000-4000-8000-000000000101',
    'qa-'||i||'.example.com', CASE WHEN i=4 THEN 'conflict' ELSE 'pending' END, NOW()
FROM generate_series(1,4) i;
INSERT INTO sys_cluster(id,tenant_id,dns_zone_id,name,hostname_prefix)
VALUES ('00000000-0000-4000-8000-000000000401', :'qa_tenant', '00000000-0000-4000-8000-000000000201', 'QA cluster', 'qa');
INSERT INTO sys_node(id,tenant_id,cluster_id,name,deleted_at)
SELECT ('00000000-0000-4000-8000-00000000050'||i)::uuid, :'qa_tenant', '00000000-0000-4000-8000-000000000401', 'QA node '||i, NOW()
FROM generate_series(1,2) i;
INSERT INTO sys_node(id,tenant_id,cluster_id,name,agent_id,node_secret_hash,node_secret_envelope)
VALUES ('00000000-0000-4000-8000-000000000503', :'qa_tenant', '00000000-0000-4000-8000-000000000401', 'QA node 3',
    repeat('b',32), repeat('c',64), 'qa-only');
INSERT INTO sys_cluster_release(id,tenant_id,cluster_id,generation,status,manifest_digest,manifest_envelope,schema_version)
SELECT ('00000000-0000-4000-8000-00000000060'||i)::uuid, :'qa_tenant', '00000000-0000-4000-8000-000000000401', i,
    CASE WHEN i=2 THEN 'active' ELSE 'superseded' END, repeat('a',64), 'qa-only', 6
FROM generate_series(1,2) i;

-- Tasks, newest first: queued, running, retrying, partial failure (children),
-- retrying through a child, merged, cancelled, recovered, attention, failed,
-- a system task without subtasks (30 h) and an unsettled 10-day-old task.
INSERT INTO sys_operation(id,tenant_id,action,actor_type,actor_id,actor_name,target_type,target_id,target_name,summary,created_at) VALUES
('{O1}', :'qa_tenant', 'dns_zone.sync', 'user', :'qa_admin', 'Administrator', 'dns_zone', '00000000-0000-4000-8000-000000000201', 'qa-1.example.com', '{}', NOW()-INTERVAL '10 minutes'),
('{O2}', :'qa_tenant', 'dns_zone.sync_local', 'user', :'qa_admin', 'Administrator', 'dns_zone', '00000000-0000-4000-8000-000000000202', 'qa-2.example.com', '{"conflict_policy":"local"}', NOW()-INTERVAL '20 minutes'),
('{O3}', :'qa_tenant', 'dns_zone.update_records', 'user', '00000000-0000-4000-8000-000000000d01', 'Other admin', 'dns_zone', '00000000-0000-4000-8000-000000000203', 'qa-3.example.com', '{"record_counts":{"created":2,"updated":1,"deleted":0}}', NOW()-INTERVAL '30 minutes'),
('{O4}', :'qa_tenant', 'cluster.update', 'user', :'qa_admin', 'Administrator', 'cluster', '00000000-0000-4000-8000-000000000401', 'QA cluster', '{"sections":["dns"]}', NOW()-INTERVAL '40 minutes'),
('{O5}', :'qa_tenant', 'node.update', 'user', :'qa_admin', 'Administrator', 'node', '00000000-0000-4000-8000-000000000503', 'QA node 3', '{"sections":["endpoints"]}', NOW()-INTERVAL '50 minutes'),
('{O6}', :'qa_tenant', 'dns_zone.sync', 'user', :'qa_admin', 'Administrator', 'dns_zone', '00000000-0000-4000-8000-000000000201', 'qa-1.example.com', '{}', NOW()-INTERVAL '60 minutes'),
('{O7}', :'qa_tenant', 'dns_zone.delete', 'user', :'qa_admin', 'Administrator', 'dns_zone', '00000000-0000-4000-8000-000000000202', 'qa-2.example.com', '{}', NOW()-INTERVAL '70 minutes'),
('{O9}', :'qa_tenant', 'provider.verify', 'user', :'qa_admin', 'Administrator', 'provider', '00000000-0000-4000-8000-000000000101', 'QA 100%_dns', '{}', NOW()-INTERVAL '80 minutes'),
('{O12}', :'qa_tenant', 'dns_zone.sync_remote', 'user', :'qa_admin', 'Administrator', 'dns_zone', '00000000-0000-4000-8000-000000000204', 'qa-4.example.com', '{"conflict_policy":"remote"}', NOW()-INTERVAL '90 minutes'),
('{O13}', :'qa_tenant', 'node.reset_credentials', 'user', :'qa_admin', 'Administrator', 'node', '00000000-0000-4000-8000-000000000503', 'QA node 3', '{}', NOW()-INTERVAL '100 minutes'),
('{O8}', :'qa_tenant', 'system.dns_cleanup', 'system', NULL, '', 'dns_zone', '00000000-0000-4000-8000-000000000204', 'qa-4.example.com', '{}', NOW()-INTERVAL '30 hours'),
('{O10}', :'qa_tenant', 'provider.update', 'user', :'qa_admin', 'Administrator', 'provider', '00000000-0000-4000-8000-000000000102', 'letsencrypt', '{"credentials_changed":true}', NOW()-INTERVAL '10 days'),
('{O11}', '00000000-0000-4000-8000-000000000701', 'dns_zone.sync', 'user', NULL, 'Other tenant', 'dns_zone', '00000000-0000-4000-8000-000000000201', 'qa-1.example.com', '{}', NOW()-INTERVAL '5 minutes');

INSERT INTO sys_operation_subtask(id,tenant_id,operation_id,parent_id,kind,resource_type,resource_id,resource_name,version,state,merged_into_id,closed_by_operation_id,created_at,closed_at)
SELECT s.id::uuid, operation.tenant_id, s.operation_id::uuid, s.parent_id::uuid, s.kind, s.resource_type, s.resource_id::uuid,
    s.resource_name, 1, s.state, s.merged_into::uuid, s.closed_by::uuid, operation.created_at + s.offset_seconds * INTERVAL '1 second',
    CASE WHEN s.state <> 'open' THEN operation.created_at + INTERVAL '5 minutes' END
FROM (VALUES
('{S1}', '{O1}', NULL, 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000201', 'qa-1.example.com', 'open', NULL, NULL, 1),
('{S2}', '{O2}', NULL, 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000202', 'qa-2.example.com', 'open', NULL, NULL, 1),
('{S3}', '{O3}', NULL, 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000203', 'qa-3.example.com', 'open', NULL, NULL, 1),
('{S4a}', '{O4}', NULL, 'node_publish', 'node', '00000000-0000-4000-8000-000000000501', 'QA node 1', 'open', NULL, NULL, 1),
('{S4b}', '{O4}', NULL, 'node_publish', 'node', '00000000-0000-4000-8000-000000000502', 'QA node 2', 'open', NULL, NULL, 2),
('{S4c}', '{O4}', '{S4a}', 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000204', 'qa-4.example.com', 'open', NULL, NULL, 3),
('{S5a}', '{O5}', NULL, 'node_publish', 'node', '00000000-0000-4000-8000-000000000503', 'QA node 3', 'open', NULL, NULL, 1),
('{S5b}', '{O5}', '{S5a}', 'node_config', 'node', '00000000-0000-4000-8000-000000000503', 'QA node 3', 'open', NULL, NULL, 2),
('{S6}', '{O6}', NULL, 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000201', 'qa-1.example.com', 'merged', '{S1}', NULL, 1),
('{S7a}', '{O7}', NULL, 'dns_delete', 'dns_zone', '00000000-0000-4000-8000-000000000202', 'qa-2.example.com', 'cancelled', NULL, '{O2}', 1),
('{S7b}', '{O7}', NULL, 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000202', 'qa-2.example.com', 'merged', NULL, '{O3}', 2),
('{S7c}', '{O7}', NULL, 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000202', 'qa-2.example.com', 'cancelled', NULL, NULL, 3),
('{S9}', '{O9}', NULL, 'provider_verify', 'provider', '00000000-0000-4000-8000-000000000101', 'QA 100%_dns', 'open', NULL, NULL, 1),
('{S10}', '{O10}', NULL, 'provider_verify', 'provider', '00000000-0000-4000-8000-000000000102', 'letsencrypt', 'open', NULL, NULL, 1),
('{S12}', '{O12}', NULL, 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000204', 'qa-4.example.com', 'completed', NULL, NULL, 1),
('{S13}', '{O13}', NULL, 'node_config', 'node', '00000000-0000-4000-8000-000000000503', 'QA node 3', 'open', NULL, NULL, 1),
('{S11}', '{O11}', NULL, 'dns_sync', 'dns_zone', '00000000-0000-4000-8000-000000000201', 'qa-1.example.com', 'open', NULL, NULL, 1)
) AS s(id, operation_id, parent_id, kind, resource_type, resource_id, resource_name, state, merged_into, closed_by, offset_seconds)
INNER JOIN sys_operation operation ON operation.id = s.operation_id::uuid
ORDER BY s.parent_id NULLS FIRST;

-- Markers own the sync subtasks; retries and leases lie a day ahead so the
-- running server never claims them.
INSERT INTO sys_sync_task(tenant_id,resource_type,dns_zone_id,provider_id,operation,version,is_done,is_ok,count_fails,error,
    next_attempt_at,lease_owner,lease_until,subtask_id) VALUES
(:'qa_tenant', 'dns_zone', '00000000-0000-4000-8000-000000000201', NULL, 'sync', 1, false, false, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, '{S1}'),
(:'qa_tenant', 'dns_zone', '00000000-0000-4000-8000-000000000202', NULL, 'sync_local', 1, false, false, 0, '', NOW()+INTERVAL '1 day', 'qa-lease', NOW()+INTERVAL '1 day', '{S2}'),
(:'qa_tenant', 'dns_zone', '00000000-0000-4000-8000-000000000203', NULL, 'sync', 1, false, false, 2, 'QA: API read failed', NOW()+INTERVAL '1 day', NULL, NULL, '{S3}'),
(:'qa_tenant', 'dns_zone', '00000000-0000-4000-8000-000000000204', NULL, 'sync', 1, true, true, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, '{S4c}'),
(:'qa_tenant', 'provider', NULL, '00000000-0000-4000-8000-000000000101', 'verify', 1, true, true, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, '{S9}'),
(:'qa_tenant', 'provider', NULL, '00000000-0000-4000-8000-000000000102', 'verify', 1, false, false, 0, '', NOW()+INTERVAL '1 day', NULL, NULL, '{S10}');

-- Node publish follows the subtask's target in the newest release: S5a's
-- failure in the superseded release no longer counts.
INSERT INTO sys_node_release_target(tenant_id,release_id,node_id,status,retryable,last_error,applied_at,subtask_id) VALUES
(:'qa_tenant', '00000000-0000-4000-8000-000000000602', '00000000-0000-4000-8000-000000000501', 'applied', true, NULL, NOW()-INTERVAL '35 minutes', '{S4a}'),
(:'qa_tenant', '00000000-0000-4000-8000-000000000602', '00000000-0000-4000-8000-000000000502', 'failed', false, 'QA deployment error', NULL, '{S4b}'),
(:'qa_tenant', '00000000-0000-4000-8000-000000000601', '00000000-0000-4000-8000-000000000503', 'failed', false, 'stale failure', NULL, '{S5a}'),
(:'qa_tenant', '00000000-0000-4000-8000-000000000602', '00000000-0000-4000-8000-000000000503', 'applied', true, NULL, NOW()-INTERVAL '45 minutes', '{S5a}');

INSERT INTO sys_sync_event(id,tenant_id,task_id,resource_type,resource_id,operation,version,outcome,error,started_at,emitted_at,
    duration_ms,attempt,detail,subtask_id) OVERRIDING SYSTEM VALUE VALUES
(990001, :'qa_tenant', gen_random_uuid(), 'dns_zone', '00000000-0000-4000-8000-000000000203', 'sync', 1, 'failed', 'QA: API timeout', NOW()-INTERVAL '29 minutes', NOW()-INTERVAL '29 minutes'+INTERVAL '2 seconds', 2000, 1, '{"kind":"none","truncated":false}', '{S3}'),
(990002, :'qa_tenant', gen_random_uuid(), 'dns_zone', '00000000-0000-4000-8000-000000000203', 'sync', 1, 'failed', 'QA: API read failed', NOW()-INTERVAL '28 minutes', NOW()-INTERVAL '28 minutes'+INTERVAL '1 second', 1000, 2, '{"kind":"none","truncated":false}', '{S3}'),
(990003, :'qa_tenant', gen_random_uuid(), 'provider', '00000000-0000-4000-8000-000000000101', 'verify', 1, 'failed', 'QA: credentials rejected', NULL, NOW()-INTERVAL '79 minutes', 0, 1, '{"kind":"none","truncated":false}', '{S9}'),
(990004, :'qa_tenant', gen_random_uuid(), 'provider', '00000000-0000-4000-8000-000000000101', 'verify', 1, 'completed', '', NULL, NOW()-INTERVAL '78 minutes', 0, 2, '{"kind":"none","truncated":false}', '{S9}'),
(990005, :'qa_tenant', gen_random_uuid(), 'node', '00000000-0000-4000-8000-000000000503', 'apply', 1, 'failed', 'QA: node rejected config', NULL, NOW()-INTERVAL '49 minutes', 0, 1, '{"kind":"none","truncated":false}', '{S5b}'),
(990006, :'qa_tenant', gen_random_uuid(), 'node', '00000000-0000-4000-8000-000000000503', 'apply', 1, 'failed', 'QA: node apply failed', NULL, NOW()-INTERVAL '99 minutes', 0, 1, '{"kind":"node_apply","truncated":false,"retryable":false}', '{S13}'),
(990007, :'qa_tenant', gen_random_uuid(), 'dns_zone', '00000000-0000-4000-8000-000000000204', 'sync_remote', 1, 'completed', '', NULL, NOW()-INTERVAL '88 minutes', 0, 1, '{"kind":"dns_sync","truncated":false,"conflict_count":1}', '{S12}'),
(990008, :'qa_tenant', gen_random_uuid(), 'dns_zone', '00000000-0000-4000-8000-000000000203', 'sync', 1, 'failed', 'untracked attempt', NULL, NOW()-INTERVAL '1 minute', 0, 1, '{"kind":"none","truncated":false}', NULL),
(990901, '00000000-0000-4000-8000-000000000701', gen_random_uuid(), 'dns_zone', '00000000-0000-4000-8000-000000000201', 'sync', 1, 'failed', 'must not leak', NULL, NOW()-INTERVAL '4 minutes', 0, 1, '{"kind":"none","truncated":false}', '{S11}');
INSERT INTO sys_sync_event_trace(event_id,tenant_id,summary,trace,trace_bytes) VALUES
(990002, :'qa_tenant', '{"state":"available","step_count":0,"call_count":0,"log_count":0,"truncated":false,"failed_step":null,"steps":[]}',
 '{"worker_type":"dns-sync","capture":{"truncated":false,"degraded":false,"bytes":64,"omitted_steps":0,"omitted_calls":0,"omitted_logs":0,"omitted_body_bytes":0,"redactions":0},"steps":[]}', 64);
"""
ids = {"O%d" % n: op(n) for n in range(1, 14)}
ids.update({"S%d" % n: sub(n, 1) for n in (1, 2, 3, 6, 9, 10, 11, 12, 13)})
ids.update({f"S{n}{letter}": sub(n, i) for n, count in ((4, 3), (5, 2), (7, 3))
            for i, letter in enumerate("abc"[:count], 1)})
for key, value in ids.items():
    fixture = fixture.replace("{" + key + "}", value)

read_model = f"""
CREATE FUNCTION pg_temp.qa_operations(uuid) RETURNS TABLE(o_id uuid, o_status text, o_subtasks bigint, o_settled bigint,
    o_failed bigint, o_error text, o_kind text, o_deleted boolean, o_created timestamptz, o_finished timestamptz,
    o_next timestamptz) LANGUAGE sql AS $qa$ {operation_sql} SELECT id, status::text, subtask_count, settled_count,
    failed_count, error_text::text, target_kind::text, target_deleted, created_at, finished, next_at FROM operations $qa$;
CREATE FUNCTION pg_temp.qa_subtasks(uuid) RETURNS TABLE(o_id uuid, o_status text, o_warning text, o_error text,
    o_next timestamptz, o_finished timestamptz, o_attempts bigint, o_failures bigint) LANGUAGE sql AS
    $qa$ WITH {subtask_sql} SELECT id, status::text, warning::text, error_text::text, next_at, finished, attempts,
    failures FROM shaped $qa$;
CREATE FUNCTION pg_temp.qa_count(uuid, text, text, text, text, text, integer, text) RETURNS bigint LANGUAGE sql AS
    $qa$ {operation_sql} SELECT count(*) FROM operations {list_filter_sql} $qa$;
"""

assertions = r"""
DO $check$
DECLARE
 qa_tenant uuid := (SELECT tenant FROM qa_context);
 qa_admin text := (SELECT admin FROM qa_context);
 op record;
 st record;
BEGIN
 IF (SELECT count(*) FROM pg_temp.qa_operations(qa_tenant)) <> 12 THEN RAISE EXCEPTION 'task count or tenant isolation'; END IF;
 IF (SELECT count(*) FROM pg_temp.qa_operations('00000000-0000-4000-8000-000000000701')) <> 1 THEN RAISE EXCEPTION 'other tenant'; END IF;
 IF (SELECT string_agg(o_status, ',' ORDER BY o_created DESC, o_id DESC) FROM pg_temp.qa_operations(qa_tenant)) <>
    'queued,running,retrying,partial_failed,retrying,merged,cancelled,completed,attention_required,failed,completed,queued'
 THEN RAISE EXCEPTION 'rolled-up statuses: %', (SELECT string_agg(o_status, ',' ORDER BY o_created DESC, o_id DESC) FROM pg_temp.qa_operations(qa_tenant)); END IF;
 SELECT * INTO op FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O4}';
 IF (op.o_subtasks, op.o_settled, op.o_failed, op.o_error, op.o_deleted) IS DISTINCT FROM (3::bigint, 3::bigint, 1::bigint, 'QA deployment error'::text, false)
    OR op.o_finished IS NULL THEN RAISE EXCEPTION 'partial failure with an attention child: %', op; END IF;
 SELECT * INTO op FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O5}';
 IF (op.o_subtasks, op.o_settled, op.o_failed, op.o_error) IS DISTINCT FROM (2::bigint, 1::bigint, 1::bigint, 'QA: node rejected config'::text)
    OR op.o_finished IS NOT NULL OR op.o_next IS NOT NULL THEN RAISE EXCEPTION 'retrying child rolls up: %', op; END IF;
 SELECT * INTO op FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O3}';
 IF op.o_next IS NULL OR op.o_error <> 'QA: API read failed' THEN RAISE EXCEPTION 'marker retry: %', op; END IF;
 SELECT * INTO op FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O7}';
 IF (op.o_subtasks, op.o_settled, op.o_failed) IS DISTINCT FROM (3::bigint, 3::bigint, 0::bigint) THEN RAISE EXCEPTION 'cancelled: %', op; END IF;
 SELECT * INTO op FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O8}';
 IF op.o_subtasks <> 0 OR op.o_finished IS DISTINCT FROM op.o_created THEN RAISE EXCEPTION 'task without subtasks: %', op; END IF;
 IF (SELECT o_kind FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O9}') IS DISTINCT FROM 'dns'
    OR (SELECT o_kind FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O10}') IS DISTINCT FROM 'certificate'
    OR (SELECT o_kind FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O1}') IS NOT NULL THEN RAISE EXCEPTION 'provider target kind'; END IF;
 IF NOT (SELECT o_deleted FROM pg_temp.qa_operations(qa_tenant) WHERE o_id = '{O1}') THEN RAISE EXCEPTION 'deleted target'; END IF;

 IF (SELECT string_agg(o_status || '/' || o_warning, ',' ORDER BY o_id) FROM pg_temp.qa_subtasks(qa_tenant) WHERE o_id IN ('{S4a}', '{S4b}', '{S4c}', '{S5a}', '{S5b}', '{S12}', '{S13}'))
    <> 'completed/,failed/,attention_required/dns_conflict,completed/,retrying/,attention_required/dns_conflict,failed/'
 THEN RAISE EXCEPTION 'subtask statuses'; END IF;
 SELECT * INTO st FROM pg_temp.qa_subtasks(qa_tenant) WHERE o_id = '{S9}';
 IF (st.o_status, st.o_attempts, st.o_failures, st.o_error) IS DISTINCT FROM ('completed'::text, 2::bigint, 1::bigint, ''::text) THEN RAISE EXCEPTION 'recovery: %', st; END IF;
 SELECT * INTO st FROM pg_temp.qa_subtasks(qa_tenant) WHERE o_id = '{S3}';
 IF (st.o_status, st.o_attempts, st.o_failures) IS DISTINCT FROM ('retrying'::text, 2::bigint, 2::bigint) OR st.o_next IS NULL THEN RAISE EXCEPTION 'retrying: %', st; END IF;
 IF (SELECT o_status FROM pg_temp.qa_subtasks(qa_tenant) WHERE o_id = '{S2}') <> 'running' THEN RAISE EXCEPTION 'leased marker'; END IF;
 IF EXISTS (SELECT 1 FROM pg_temp.qa_subtasks(qa_tenant) WHERE o_id = '{S11}') THEN RAISE EXCEPTION 'subtask tenant isolation'; END IF;

 IF (pg_temp.qa_count(qa_tenant, 'active', '', '', '', qa_admin, 0, ''), pg_temp.qa_count(qa_tenant, 'attention', '', '', '', qa_admin, 0, ''),
     pg_temp.qa_count(qa_tenant, 'queued', '', '', '', qa_admin, 0, ''), pg_temp.qa_count(qa_tenant, 'completed', '', '', '', qa_admin, 0, ''))
    IS DISTINCT FROM (5::bigint, 5::bigint, 2::bigint, 2::bigint) THEN RAISE EXCEPTION 'status filters'; END IF;
 IF (pg_temp.qa_count(qa_tenant, '', '', '', '', qa_admin, 1, ''), pg_temp.qa_count(qa_tenant, '', '', '', '', qa_admin, 7, ''))
    IS DISTINCT FROM (10::bigint, 11::bigint) THEN RAISE EXCEPTION 'date filter'; END IF;
 IF (pg_temp.qa_count(qa_tenant, '', 'dns_zone.sync', '', '', qa_admin, 0, ''), pg_temp.qa_count(qa_tenant, '', '', 'dns_zone', '', qa_admin, 0, ''),
     pg_temp.qa_count(qa_tenant, '', '', '', 'me', qa_admin, 0, ''), pg_temp.qa_count(qa_tenant, '', '', '', 'system', qa_admin, 0, ''))
    IS DISTINCT FROM (2::bigint, 7::bigint, 10::bigint, 1::bigint) THEN RAISE EXCEPTION 'action/target/actor filters'; END IF;
 IF (pg_temp.qa_count(qa_tenant, '', '', '', '', qa_admin, 0, '%'), pg_temp.qa_count(qa_tenant, '', '', '', '', qa_admin, 0, '_'),
     pg_temp.qa_count(qa_tenant, '', '', '', '', qa_admin, 0, ''''), pg_temp.qa_count(qa_tenant, '', '', '', '', qa_admin, 0, 'QA-1.EXAMPLE'),
     pg_temp.qa_count(qa_tenant, '', '', '', '', qa_admin, 0, '000000000401'))
    IS DISTINCT FROM (1::bigint, 1::bigint, 0::bigint, 2::bigint, 1::bigint) THEN RAISE EXCEPTION 'keyword filter'; END IF;
END;
$check$;
"""
for key, value in ids.items():
    assertions = assertions.replace("{" + key + "}", value)

sql = fixture + read_model + assertions
sql += "COMMIT;" if "--keep-fixture" in sys.argv else "ROLLBACK;"
result = subprocess.run([os.environ.get("PSQL", "psql"), "-X", "-q", "-v", "ON_ERROR_STOP=1"],
                        input=sql, encoding="utf-8", capture_output=True)
if result.returncode:
    raise SystemExit(result.stderr)
print("Task read-model integration passed: status roll-up incl. children, release generations, markers, attempts, "
      "merged/cancelled, target kinds, tenant isolation, filters, counts.")
