"""Local task-center reads and resource writes; run after the isolated --keep-fixture setup
of task_center_fixture.py against the same database."""
import http.cookies
import json
import os
import urllib.error
import urllib.parse
import urllib.request
import uuid

base = os.environ.get("TASK_QA_URL", "http://127.0.0.1:1102")
if urllib.parse.urlparse(base).hostname not in {"127.0.0.1", "localhost", "::1"}:
    raise SystemExit("Only a local QA server is supported")
cookie = ""


def uid(n):
    return f"00000000-0000-4000-8000-{n:012d}"


Z1, Z2, Z3, Z4 = (uid(n) for n in (201, 202, 203, 204))
P1, P2 = uid(101), uid(102)
N1, N2, N3, N4, N5, N6 = (uid(n) for n in range(501, 507))


def request(path, expected=200, body=None, authenticated=True, method=None, revision=None):
    global cookie
    headers = {"Content-Type": "application/json"}
    if authenticated and cookie:
        headers["Cookie"] = cookie
    if revision is not None:
        headers["If-Match"] = f'"{revision}"'
    req = urllib.request.Request(base + path, headers=headers,
        data=json.dumps(body).encode() if body is not None else None, method=method)
    try:
        response = urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    assert response.status == expected, (path, response.status, expected)
    for value in response.headers.get_all("Set-Cookie", []):
        parsed = http.cookies.SimpleCookie(value)
        cookie = "; ".join(f"{key}={item.value}" for key, item in parsed.items())
    with response:
        return json.load(response)


def frames(path, count=1):
    """The first `count` frames after `ready` as (event, data); a rejected
    request (HTTP 400 parameter error) reads as ("http-<status>", body)."""
    req = urllib.request.Request(base + path, headers={"Accept": "text/event-stream", "Cookie": cookie})
    try:
        response = urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as error:
        return [(f"http-{error.code}", json.load(error))]
    assert response.status == 200, (path, response.status)
    result, event, data = [], None, None
    with response:
        for raw in response:
            line = raw.decode().rstrip("\r\n")
            if line.startswith("event: "): event = line[7:]
            elif line.startswith("data: "): data = line[6:]
            elif not line and event:
                if event != "ready":
                    result.append((event, json.loads(data)))
                    if len(result) == count:
                        return result
                event, data = None, None
    raise AssertionError(f"SSE closed after {result}: {path}")


def snapshot(path, expected_code=0):
    """`snapshot` data, or asserts the error: 10001 is rejected with HTTP 400,
    business errors arrive as a `resource-error` frame on an open stream."""
    [(event, value)] = frames(path)
    if expected_code == 10001:
        assert (event, value["code"]) == ("http-400", 10001), (path, event, value)
    elif expected_code:
        assert (event, value["code"]) == ("resource-error", expected_code), (path, event, value)
    else:
        assert event == "snapshot" and value["code"] == 0, (path, event, value)
    return value.get("data")


def read(path, expected_code=0, status=200):
    """GET `data`, or asserts the error envelope and its HTTP status."""
    value = request(path, status)
    assert value["code"] == expected_code, (path, value)
    return value.get("data")


def total(query):
    return snapshot("/api/tasks/stream?" + query)["total"]


request("/api/tasks/stream", 401, authenticated=False)
request("/api/tasks", 401, authenticated=False)
request(f"/api/tasks/dns_zone/{Z1}/stream", 401, authenticated=False)
request(f"/api/tasks/dns_zone/{Z1}", 401, authenticated=False)
request("/api/auth/login", body={"username": "admin", "password": os.environ["TASK_QA_PASSWORD"]})

# List: pagination, tenant-wide counters, order updated_at DESC.
order = [Z1, N3, Z2, Z3, Z4, P1, P2, N1, N2, N4]
page = snapshot("/api/tasks/stream?page=1&page_size=4")
assert (page["total"], page["page"], page["page_size"], page["total_pages"]) == (10, 1, 4, 3), page
assert (page["active"], page["failed"]) == (4, 2), page
assert [item["resource_id"] for item in page["list"]] == order[:4], page["list"]
assert [item["status"] for item in page["list"]] == ["queued", "retrying", "running", "retrying"]
last = snapshot("/api/tasks/stream?page=3&page_size=4")
assert [item["resource_id"] for item in last["list"]] == order[8:] and last["total"] == 10, last
beyond = snapshot("/api/tasks/stream?page=99&page_size=4")
assert beyond["list"] == [] and beyond["total"] == 10 and beyond["active"] == 4, beyond
assert snapshot("/api/tasks/stream")["page_size"] == 20
# GET reads answer the stream's first snapshot.
assert read("/api/tasks?page=1&page_size=4") == page
assert read("/api/tasks?status=failed&page_size=100") == snapshot("/api/tasks/stream?status=failed&page_size=100")
read("/api/tasks?status=unknown", 10001, 400)
for item in snapshot("/api/tasks/stream?page_size=100")["list"]:
    assert "version" not in item and "id" not in item and item["resource_id"] not in (N5, N6), item

# Filters; counters ignore them.
statuses = {"queued": 1, "running": 1, "retrying": 2, "failed": 2, "completed": 4,
            "active": 4, "attention": 4}
for status, count in statuses.items():
    assert total("status=" + status) == count, status
filtered = snapshot("/api/tasks/stream?status=failed")
assert (filtered["active"], filtered["failed"]) == (4, 2), filtered
assert [item["resource_id"] for item in filtered["list"]] == [N2, N4], filtered
for task_type, count in {"dns_zone": 4, "provider": 2, "node": 4, "website": 0, "certificate": 0}.items():
    assert total("type=" + task_type) == count, task_type
assert total("status=active&type=node") == 1
for keyword, count in {"%25": 1, "_": 1, "%27": 0, "%5C": 0, "QA-1.EXAMPLE": 1, "+qa-1.example.com+": 1,
                       "000000000201": 1, "qa+node": 4, "letsencrypt": 1}.items():
    assert total("keyword=" + keyword) == count, keyword
for query in ("status=unknown", "status=ACTIVE", "status=merged", "type=unknown", "type=cluster", "page=0",
              "page=-1", "page_size=0", "page_size=101", "keyword=" + "a" * 201):
    snapshot("/api/tasks/stream?" + query, expected_code=10001)

# Task snapshots.
fields = {"resource_type", "resource_id", "resource_kind", "name", "operation", "status", "error",
          "next_attempt_at", "updated_at", "last_success_at", "attempt_count", "failure_count", "node_offline"}
retrying = snapshot(f"/api/tasks/dns_zone/{Z3}/stream")
assert set(retrying) == fields, retrying
assert (retrying["status"], retrying["operation"], retrying["error"], retrying["attempt_count"],
        retrying["failure_count"]) == ("retrying", "sync", "QA: API read failed", 3, 2), retrying
assert retrying["next_attempt_at"] and retrying["last_success_at"] == "" and retrying["name"] == "qa-3.example.com"
running = snapshot(f"/api/tasks/dns_zone/{Z2}/stream")
assert (running["status"], running["operation"], running["next_attempt_at"]) == ("running", "sync", "")
attention = snapshot(f"/api/tasks/dns_zone/{Z4}/stream")
assert (attention["status"], attention["error"]) == ("completed", "")
recovered = snapshot(f"/api/tasks/provider/{P1}/stream")
assert (recovered["status"], recovered["resource_kind"], recovered["attempt_count"], recovered["failure_count"],
        recovered["name"]) == ("completed", "dns", 2, 0, "QA 100%_dns"), recovered
reinitiated = snapshot(f"/api/tasks/provider/{P2}/stream")
assert (reinitiated["resource_kind"], reinitiated["name"], reinitiated["attempt_count"],
        reinitiated["failure_count"]) == ("certificate", "letsencrypt", 2, 0), reinitiated
healthy = snapshot(f"/api/tasks/node/{N1}/stream")
assert (healthy["status"], healthy["operation"], healthy["attempt_count"], healthy["failure_count"],
        healthy["node_offline"]) == ("completed", "publish", 3, 0, False), healthy
publish_failed = snapshot(f"/api/tasks/node/{N2}/stream")
assert (publish_failed["status"], publish_failed["error"], publish_failed["node_offline"]) == \
    ("failed", "QA deployment error", True), publish_failed
configure = snapshot(f"/api/tasks/node/{N3}/stream")
assert (configure["status"], configure["operation"], configure["error"], configure["node_offline"],
        configure["failure_count"]) == ("retrying", "configure", "QA: node rejected config", True, 1), configure
configure_failed = snapshot(f"/api/tasks/node/{N4}/stream")
assert (configure_failed["status"], configure_failed["error"], configure_failed["node_offline"]) == \
    ("failed", "QA: node apply failed", False), configure_failed
assert read(f"/api/tasks/dns_zone/{Z3}") == retrying
assert read(f"/api/tasks/node/{N4}") == configure_failed

# The detail stream: snapshot first, then the history (newest first).
[(first, head), (second, body)] = frames(f"/api/tasks/dns_zone/{Z3}/stream", 2)
assert (first, head["code"], second, body["code"]) == ("snapshot", 0, "history", 0), (first, second)
assert head["data"] == retrying, head
history = body["data"]
assert set(history) == {"summary", "list"}, history
assert [(entry["id"], entry["attempt"], entry["operation"], entry["outcome"], entry["error"])
        for entry in history["list"]] == [("990003", 2, "sync", "failed", "QA: API read failed"),
                                          ("990002", 1, "sync", "failed", "QA: API timeout"),
                                          ("990001", 1, "sync", "failed", "QA: old version")], history["list"]
assert [entry["trace"]["attempt_id"] for entry in history["list"]] == ["990003", "990002", "990001"]
assert [entry["trace"]["state"] for entry in history["list"]] == ["available", "none", "none"]
assert history["list"][1]["duration_ms"] == 2000 and history["list"][1]["started_at"]
assert history["list"][1]["detail"] == {"kind": "none", "truncated": False}
summary = history["summary"]
assert (summary["attempt_count"], summary["failure_count"], summary["last_success_at"]) == (3, 3, ""), summary
assert summary["first_failure_at"] == history["list"][2]["emitted_at"], summary
node_history = frames(f"/api/tasks/node/{N3}/stream", 2)[1][1]["data"]
assert [(entry["operation"], entry["outcome"]) for entry in node_history["list"]] == \
    [("configure", "failed"), ("configure", "failed")], node_history
recovery = frames(f"/api/tasks/provider/{P1}/stream", 2)[1][1]["data"]
assert [entry["outcome"] for entry in recovery["list"]] == ["completed", "failed"]
assert recovery["summary"]["last_success_at"] == recovery["list"][0]["emitted_at"]
isolated = frames(f"/api/tasks/dns_zone/{Z1}/stream", 2)[1][1]["data"]
assert isolated["list"] == [] and isolated["summary"]["attempt_count"] == 0, isolated

# Attempt traces: GET only.
trace = read(f"/api/tasks/dns_zone/{Z3}/attempts/990003/trace")
assert (trace["attempt_id"], trace["resource_type"], trace["resource_id"]) == ("990003", "dns_zone", Z3), trace
assert (trace["state"], trace["kind"], trace["attempt"], trace["outcome"], trace["error"]) == \
    ("available", "none", 2, "failed", "QA: API read failed"), trace
assert trace["worker_type"] == "dns-sync" and trace["capture"]["bytes"] == 64 and trace["steps"] == [], trace
assert "task_id" not in trace
untraced = read(f"/api/tasks/dns_zone/{Z3}/attempts/990002/trace")
assert (untraced["state"], untraced["attempt"], untraced["capture"]["bytes"]) == ("none", 1, 0), untraced
assert untraced["worker_type"] == "dns-sync" and untraced["duration_ms"] == 2000, untraced
# Another task's attempt, the same task under another type, another tenant's
# attempt and an unknown one are all "not found".
for path in (f"dns_zone/{Z1}/attempts/990003", f"node/{Z3}/attempts/990003", f"dns_zone/{Z1}/attempts/990901",
             f"dns_zone/{Z3}/attempts/999999999"):
    read(f"/api/tasks/{path}/trace", 16706, 404)
for attempt_id in ("0", "-1", "not-a-number"):
    read(f"/api/tasks/dns_zone/{Z3}/attempts/{attempt_id}/trace", 10001, 400)
read("/api/tasks/dns_zone/invalid/attempts/990003/trace", 10001, 400)
read(f"/api/tasks/cluster/{Z3}/attempts/990003/trace", 10001, 400)
request(f"/api/tasks/dns_zone/{Z3}/attempts/990003/trace/stream", 404)

# Unknown tasks, tasks under the wrong type, removed nodes; invalid identities; the removed API.
for path in (f"dns_zone/{uid(999)}", f"node/{Z1}", f"dns_zone/{N1}", f"node/{N5}", f"node/{N6}"):
    snapshot(f"/api/tasks/{path}/stream", expected_code=16701)
    read(f"/api/tasks/{path}", 16701, 404)
snapshot("/api/tasks/dns_zone/invalid/stream", expected_code=10001)
snapshot(f"/api/tasks/cluster/{Z1}/stream", expected_code=10001)
read(f"/api/tasks/cluster/{Z1}", 10001, 400)
request("/api/operations/stream", 404)


# Actual authenticated writes, after all fixed-fixture count assertions.
# Creating a provider and changing only its display name schedules no
# background work: no task appears and responses carry no task reference.
def task_ids():
    page = snapshot("/api/tasks/stream?page_size=100")
    assert page["total"] == len(page["list"]), page
    return {(item["resource_type"], item["resource_id"]) for item in page["list"]}


def write(path, body=None, method="POST", revision=None):
    before = task_ids()
    response = request(path, body=body, method=method, revision=revision)
    assert response["code"] == 0 and "data" not in response, response
    assert task_ids() == before, (path, "a write without background work created a task")
    return response


def failed_write(path, status, code, body=None, method="POST", revision=None):
    before = task_ids()
    response = request(path, expected=status, body=body, method=method, revision=revision)
    assert response["code"] == code, response
    assert task_ids() == before, (path, "failed write created a task")


account_id = uuid.uuid4().hex
provider_name = "Task center QA " + account_id[:8]
provider_body = {"name": provider_name, "provider": "cloudflare", "account_id": account_id,
                 "api_token": "qa-only-token-never-used-for-network"}
write("/api/providers/dns", provider_body)
providers = snapshot("/api/providers/dns/stream?page_size=100&keyword=" + urllib.parse.quote(provider_name))
[provider_id] = [item["id"] for item in providers["list"] if item["name"] == provider_name]
failed_write("/api/providers/dns", 409, 16103, provider_body)
write(f"/api/providers/dns/{provider_id}", {"name": provider_name + " renamed"}, method="PUT", revision=1)
failed_write(f"/api/providers/dns/{provider_id}", 412, 16107, {"name": "Must not be saved"}, method="PUT", revision=1)
provider = snapshot(f"/api/providers/dns/{provider_id}/stream")
assert provider["name"] == provider_name + " renamed" and provider["revision"] == 2, provider
write(f"/api/providers/dns/{provider_id}", method="DELETE", revision=2)
snapshot(f"/api/providers/dns/{provider_id}/stream", expected_code=16101)

print("Task HTTP/SSE integration passed: authentication, pagination, filters, validation, GET/stream parity, "
      "marker and node statuses, current-work failure counts, history, GET-only attempt traces, tenant isolation, "
      "removed /api/operations, writes without background work create no task.")
