"""Validate actual authentication-store SQL on an isolated PostgreSQL database.

Requires loopback PGHOST and a PGDATABASE ending in _auth_qa. Creates temporary
schema fixtures inside a rolled-back transaction; does not run C++ or HTTP code.
PSQL can point to psql.exe when run from WSL.
"""
from pathlib import Path
from sql_test_support import run_sql, store_statements
import re


def main():
    root = Path(__file__).resolve().parent.parent
    schema = (root / "service/config/schema.h").read_text(encoding="utf-8")
    tables = []
    for name in ("sys_tenant", "sys_admin", "sys_auth_session", "sys_auth_login_throttle"):
        tables.append(re.search(r"CREATE TABLE public\." + name + r" \(.*?\n\);", schema, re.S).group())
    statements = store_statements(root / "service/domains/auth/auth_session.store.h")
    if len(statements) != 7:
        raise AssertionError(f"expected seven store SQL statements, got {len(statements)}")
    select, revoke_expired, revoke_disabled, rotate, insert_next, create, logout = statements
    sql = "BEGIN;\n" + "\n".join(tables) + "\n"
    login_user, current_user = store_statements(root / "service/domains/auth/auth_user.store.h")
    sql += "CREATE TEMP VIEW qa_login AS " + login_user.replace("$1", "'qa'") + ";\n"
    sql += "CREATE TEMP VIEW qa_current AS " + current_user.replace(
        "$1", "(SELECT id FROM sys_admin WHERE username='qa')"
    ) + ";\n"
    remaining_lock, record_failure, clear_failures = store_statements(
        root / "service/domains/auth/login_throttle.store.h"
    )
    sql += "CREATE TEMP VIEW qa_lock(seconds) AS " + remaining_lock.replace("$1", "'qa'") + ";\n"
    sql += "PREPARE record_failure AS " + record_failure + ";\n"
    sql += "PREPARE clear_failures AS " + clear_failures + ";\n"
    for name, statement in (("create_session", create), ("rotate_session", rotate),
                            ("insert_next", insert_next), ("logout_session", logout),
                            ("read_session", select), ("revoke_expired", revoke_expired),
                            ("revoke_disabled", revoke_disabled)):
        sql += f"PREPARE {name} AS {statement};\n"
    sql += r"""
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM qa_login) OR EXISTS (SELECT 1 FROM qa_current) THEN
    RAISE EXCEPTION 'missing user returned'; END IF;
END $check$;
INSERT INTO sys_tenant(name,slug) VALUES ('QA','auth-qa');
INSERT INTO sys_admin(username,password_hash) VALUES ('qa','unused') RETURNING id AS admin_id \gset
DO $check$ BEGIN
 IF (SELECT count(*) FROM qa_login) <> 1 OR (SELECT count(*) FROM qa_current) <> 1 THEN
    RAISE EXCEPTION 'enabled user missing'; END IF;
 IF (SELECT password_hash FROM qa_login) <> 'unused' OR
    (SELECT username FROM qa_current) <> 'qa' THEN
    RAISE EXCEPTION 'user projection mismatch'; END IF;
END $check$;
UPDATE sys_admin SET deleted_at=NOW();
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM qa_login) OR EXISTS (SELECT 1 FROM qa_current) THEN
    RAISE EXCEPTION 'deleted user returned'; END IF;
END $check$;
UPDATE sys_admin SET deleted_at=NULL;
UPDATE sys_tenant SET status='disabled';
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM qa_login) OR EXISTS (SELECT 1 FROM qa_current) THEN
    RAISE EXCEPTION 'disabled tenant admitted user'; END IF;
END $check$;
UPDATE sys_tenant SET status='enabled', deleted_at=NOW();
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM qa_login) OR EXISTS (SELECT 1 FROM qa_current) THEN
    RAISE EXCEPTION 'deleted tenant admitted user'; END IF;
END $check$;
UPDATE sys_tenant SET deleted_at=NULL;
UPDATE sys_admin SET status='disabled';
DO $check$ BEGIN
 IF (SELECT status FROM qa_login) IS DISTINCT FROM 'disabled' OR
    (SELECT status FROM qa_current) IS DISTINCT FROM 'disabled' THEN
    RAISE EXCEPTION 'disabled status unavailable to authentication policy'; END IF;
END $check$;
UPDATE sys_admin SET status='enabled';
EXECUTE create_session(NULL, :'admin_id', repeat('a',64), 3600) \gset first_
SELECT family_id AS family FROM sys_auth_session WHERE id=:'first_id' \gset
EXECUTE read_session(:'first_id');
EXECUTE rotate_session(:'first_id');
EXECUTE insert_next(:'family', :'admin_id', repeat('b',64), 3600) \gset next_
DO $check$ BEGIN
 IF (SELECT count(*) FROM sys_auth_session) <> 2 OR
    (SELECT count(*) FROM sys_auth_session WHERE revoked_at IS NULL) <> 1 OR
    (SELECT count(*) FROM sys_auth_session WHERE rotated_at IS NOT NULL) <> 1 THEN
    RAISE EXCEPTION 'rotation state'; END IF;
 IF EXISTS (SELECT 1 FROM sys_auth_session WHERE expires_at <= NOW()) THEN
    RAISE EXCEPTION 'session lifetime'; END IF;
END $check$;
EXECUTE logout_session(:'next_id', repeat('c',64));
DO $check$ BEGIN
 IF (SELECT count(*) FROM sys_auth_session WHERE revoked_at IS NULL) <> 1 THEN
    RAISE EXCEPTION 'wrong secret revoked session'; END IF;
END $check$;
EXECUTE logout_session(:'next_id', repeat('b',64));
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM sys_auth_session WHERE revoked_at IS NULL) THEN
    RAISE EXCEPTION 'logout failed to revoke family'; END IF;
END $check$;
EXECUTE insert_next(:'family', :'admin_id', repeat('d',64), 3600);
EXECUTE revoke_expired(:'family');
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM sys_auth_session WHERE revoked_at IS NULL) THEN
    RAISE EXCEPTION 'expired family revocation'; END IF;
END $check$;
EXECUTE insert_next(:'family', :'admin_id', repeat('e',64), 3600);
EXECUTE revoke_disabled(:'family');
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM sys_auth_session WHERE revoked_at IS NULL) THEN
    RAISE EXCEPTION 'disabled user family revocation'; END IF;
END $check$;
EXECUTE record_failure('qa');
EXECUTE record_failure('qa');
EXECUTE record_failure('qa');
EXECUTE record_failure('qa');
DO $check$ BEGIN
 IF (SELECT failure_count FROM sys_auth_login_throttle WHERE username='qa') <> 4 OR
    EXISTS (SELECT 1 FROM qa_lock) THEN
    RAISE EXCEPTION 'premature login lock'; END IF;
END $check$;
EXECUTE record_failure('qa');
DO $check$ BEGIN
 IF (SELECT failure_count FROM sys_auth_login_throttle WHERE username='qa') <> 5 OR
    (SELECT seconds FROM qa_lock) IS DISTINCT FROM 900::bigint THEN
    RAISE EXCEPTION 'fifth failure must lock for fifteen minutes'; END IF;
END $check$;
UPDATE sys_auth_login_throttle SET window_started_at=NOW()-INTERVAL '16 minutes',
    locked_until=NOW()-INTERVAL '1 minute' WHERE username='qa';
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM qa_lock) THEN
    RAISE EXCEPTION 'expired lock still visible'; END IF;
END $check$;
EXECUTE record_failure('qa');
DO $check$ BEGIN
 IF (SELECT failure_count FROM sys_auth_login_throttle WHERE username='qa') <> 1 OR
    EXISTS (SELECT 1 FROM qa_lock) THEN
    RAISE EXCEPTION 'expired window failed to reset'; END IF;
END $check$;
EXECUTE record_failure('other');
EXECUTE clear_failures('qa');
DO $check$ BEGIN
 IF EXISTS (SELECT 1 FROM sys_auth_login_throttle WHERE username='qa') OR
    (SELECT count(*) FROM sys_auth_login_throttle WHERE username='other') <> 1 THEN
    RAISE EXCEPTION 'clearing failures must isolate username'; END IF;
END $check$;
ROLLBACK;
"""
    run_sql(sql)
    print("Authentication SQL passed: user visibility, sessions, login locking, expiry and reset.")


if __name__ == "__main__":
    main()
