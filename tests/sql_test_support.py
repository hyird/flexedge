"""Shared extraction and execution support for isolated PostgreSQL SQL tests."""
import json
import os
import re
import subprocess


def store_statements(path):
    source = path.read_text(encoding="utf-8")
    return [
        "".join(json.loads(token) for token in re.findall(r'"(?:[^"\\]|\\.)*"', match[1]))
        for match in re.finditer(r'(?:query|execute)\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+),', source)
    ]



def run_sql(sql):
    if os.environ.get("PGHOST") not in {"127.0.0.1", "localhost", "::1"}:
        raise SystemExit("PGHOST must be loopback")
    if not os.environ.get("PGDATABASE", "").endswith("_auth_qa"):
        raise SystemExit("PGDATABASE must end in _auth_qa")
    result = subprocess.run(
        [os.environ.get("PSQL", "psql"), "-X", "-q", "-v", "ON_ERROR_STOP=1"],
        input=sql, encoding="utf-8", capture_output=True, timeout=30,
    )
    if result.returncode:
        raise SystemExit(result.stderr)
