"""Exercise the actual task SQL against a migrated, isolated PostgreSQL database.

Set PGHOST, PGPORT, PGUSER, PGPASSWORD and PGDATABASE, then run this file.
PSQL may point to psql.exe. By default all fixture writes roll back. --keep-fixture
is for browser QA against the same local database. Never use a production DB.
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

root = Path(__file__).resolve().parent.parent
source = (root / "service/domains/task/task.service.h").read_text(encoding="utf-8")
cte = re.search(r'kTaskRecordsSql = R"sql\((.*?)\)sql";', source, re.S).group(1)
cte = cte.replace("$1", "(SELECT id FROM sys_tenant ORDER BY sort LIMIT 1)")
for param in ("$2", "$3", "$4"):
    cte = cte.replace(param, "''")
cte = cte.replace("$5", "0")

fixture = r"""
BEGIN;
SELECT id AS qa_tenant FROM sys_tenant ORDER BY sort LIMIT 1 \gset
INSERT INTO sys_provider(id,tenant_id,kind,provider,name,account_id,deleted_at)
VALUES ('00000000-0000-4000-8000-000000000101', :'qa_tenant', 'dns', 'aliyun', 'QA DNS', 'qa', NOW());
INSERT INTO sys_dns_zone(id,tenant_id,provider_id,domain,deleted_at)
SELECT ('00000000-0000-4000-8000-00000000020'||i)::uuid, :'qa_tenant',
    '00000000-0000-4000-8000-000000000101', 'qa-'||i||'.example.com', NOW()
FROM generate_series(1,4) i;
INSERT INTO sys_sync_task(id,tenant_id,resource_type,dns_zone_id,operation,version,is_done,is_ok,
    count_fails,error,next_attempt_at,lease_owner,lease_until)
SELECT ('00000000-0000-4000-8000-00000000030'||i)::uuid, :'qa_tenant','dns_zone',
    ('00000000-0000-4000-8000-00000000020'||i)::uuid, 'sync', 2, i=4, i=4,
    CASE WHEN i=3 THEN 1 ELSE 0 END, CASE WHEN i=3 THEN 'QA: API read failed' ELSE '' END,
    NOW()+INTERVAL '1 day', CASE WHEN i=2 THEN 'qa-lease' END,
    CASE WHEN i=2 THEN NOW()+INTERVAL '1 day' END
FROM generate_series(1,4) i;
INSERT INTO sys_sync_event(tenant_id,task_id,resource_type,resource_id,operation,version,outcome,error,emitted_at)
VALUES
(:'qa_tenant','00000000-0000-4000-8000-000000000303','dns_zone','00000000-0000-4000-8000-000000000203','sync',1,'failed','old version failure',NOW()-INTERVAL '2 days'),
(:'qa_tenant','00000000-0000-4000-8000-000000000303','dns_zone','00000000-0000-4000-8000-000000000203','sync',2,'failed','QA: API read failed',NOW()),
(:'qa_tenant','00000000-0000-4000-8000-000000000304','dns_zone','00000000-0000-4000-8000-000000000204','sync',2,'failed','retained error after recovery',NOW()-INTERVAL '16 seconds'),
(:'qa_tenant','00000000-0000-4000-8000-000000000304','dns_zone','00000000-0000-4000-8000-000000000204','sync',2,'completed','',NOW());
INSERT INTO sys_sync_event(tenant_id,task_id,resource_type,resource_id,operation,version,outcome,emitted_at)
SELECT :'qa_tenant', gen_random_uuid(),'dns_zone','00000000-0000-4000-8000-000000000204','sync',i,'completed',NOW()-INTERVAL '5 days'
FROM generate_series(10,21) i;
INSERT INTO sys_cluster(id,tenant_id,dns_zone_id,name,hostname_prefix,deleted_at)
VALUES ('00000000-0000-4000-8000-000000000401', :'qa_tenant','00000000-0000-4000-8000-000000000201','QA cluster','qa',NOW());
INSERT INTO sys_node(id,tenant_id,cluster_id,name,deleted_at)
SELECT ('00000000-0000-4000-8000-00000000050'||i)::uuid, :'qa_tenant','00000000-0000-4000-8000-000000000401','QA node '||i,NOW()
FROM generate_series(1,3) i;
INSERT INTO sys_cluster_release(id,tenant_id,cluster_id,generation,status,manifest_digest,manifest_envelope,schema_version)
SELECT ('00000000-0000-4000-8000-00000000060'||i)::uuid, :'qa_tenant', '00000000-0000-4000-8000-000000000401',i,
    CASE WHEN i=1 THEN 'active' ELSE 'superseded' END, repeat('a',64), 'qa-only', 6
FROM generate_series(1,2) i;
INSERT INTO sys_node_release_target(tenant_id,release_id,node_id,status,retryable,last_error)
SELECT :'qa_tenant','00000000-0000-4000-8000-000000000601',('00000000-0000-4000-8000-00000000050'||i)::uuid,
    CASE WHEN i=3 THEN 'applied' ELSE 'failed' END, i=2, CASE WHEN i<3 THEN 'QA deployment error' END
FROM generate_series(1,3) i;
INSERT INTO sys_node_release_target(tenant_id,release_id,node_id,status,retryable,last_error)
VALUES (:'qa_tenant','00000000-0000-4000-8000-000000000602','00000000-0000-4000-8000-000000000501','failed',false,'old release');
INSERT INTO sys_tenant(id,name,slug) VALUES ('00000000-0000-4000-8000-000000000701','Other tenant','qa-other');
INSERT INTO sys_sync_event(tenant_id,task_id,resource_type,resource_id,operation,version,outcome,error)
VALUES ('00000000-0000-4000-8000-000000000701','00000000-0000-4000-8000-000000000303','dns_zone','00000000-0000-4000-8000-000000000203','sync',2,'completed','must not leak');
"""
assertions = r"""
DO $check$
BEGIN
 IF (SELECT count(*) FROM qa_records) <> 21 THEN RAISE EXCEPTION 'task count or tenant isolation'; END IF;
 IF (SELECT count(*) FROM qa_records WHERE status IN ('queued','running','retrying')) <> 4 THEN RAISE EXCEPTION 'active count'; END IF;
 IF (SELECT count(*) FROM qa_records WHERE status='failed') <> 1 THEN RAISE EXCEPTION 'final failures include retries or old releases'; END IF;
 IF (SELECT status FROM qa_records WHERE id='00000000-0000-4000-8000-000000000303' AND version=1) <> 'superseded' THEN RAISE EXCEPTION 'old version status'; END IF;
 IF (SELECT status FROM qa_records WHERE id='00000000-0000-4000-8000-000000000303' AND version=2) <> 'retrying' THEN RAISE EXCEPTION 'cross-tenant completion leaked'; END IF;
 IF (SELECT status FROM qa_records WHERE id='00000000-0000-4000-8000-000000000304') <> 'recovered' THEN RAISE EXCEPTION 'recovery status'; END IF;
 IF (SELECT error FROM sys_sync_event WHERE task_id='00000000-0000-4000-8000-000000000304' AND outcome='failed') <> 'retained error after recovery' THEN RAISE EXCEPTION 'failure snapshot lost'; END IF;
 IF (SELECT count(*) FROM qa_records WHERE resource_type='node') <> 4 THEN RAISE EXCEPTION 'node target identity'; END IF;
 IF (SELECT count(*) FROM qa_records WHERE updated_at >= NOW()-INTERVAL '1 day') <> 8 THEN RAISE EXCEPTION 'date filter'; END IF;
END;
$check$;
"""
sql = fixture + "\nCREATE TEMP VIEW qa_records AS " + cte + "SELECT * FROM filtered;\n" + assertions
sql += "COMMIT;" if "--keep-fixture" in sys.argv else "ROLLBACK;"
result = subprocess.run([os.environ.get("PSQL", "psql"), "-X", "-q", "-v", "ON_ERROR_STOP=1"],
                        input=sql, encoding="utf-8", capture_output=True)
if result.returncode:
    raise SystemExit(result.stderr)
print("Task read-model integration passed: version isolation, tenant isolation, recovery, node targets, counts, time filtering.")
