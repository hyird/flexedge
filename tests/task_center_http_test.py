"""Read-only task-center SSE integration test; run with task_center_read_model_test.py --keep-fixture."""
import http.cookies
import json
import os
import urllib.error
import urllib.parse
import urllib.request

base = os.environ.get("TASK_QA_URL", "http://127.0.0.1:1102")
if urllib.parse.urlparse(base).hostname not in {"127.0.0.1", "localhost", "::1"}:
    raise SystemExit("Only a local QA server is supported")
cookie = ""


def op(n):
    return f"00000000-0000-4000-8000-0000000a{n:02d}00"


def sub(n, i=1):
    return f"00000000-0000-4000-8000-0000000b{n:02d}{i:02d}"


def request(path, expected=200, body=None, authenticated=True):
    global cookie
    headers = {"Content-Type": "application/json"}
    if authenticated and cookie:
        headers["Cookie"] = cookie
    req = urllib.request.Request(base + path, headers=headers,
        data=json.dumps(body).encode() if body is not None else None)
    try:
        response = urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    assert response.status == expected, (path, response.status, expected)
    for value in response.headers.get_all("Set-Cookie", []):
        parsed = http.cookies.SimpleCookie(value)
        cookie = "; ".join(f"{key}={item.value}" for key, item in parsed.items())
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
                if event not in ("ready", "heartbeat"):
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


def total(query):
    return snapshot("/api/operations/stream?" + query)["total"]


request("/api/operations/stream", 401, authenticated=False)
request(f"/api/operations/{op(1)}/stream", 401, authenticated=False)
request("/api/auth/login", body={"username": "admin", "password": os.environ["TASK_QA_PASSWORD"]})

# List: pagination, tenant-wide counters, order created_at DESC.
page = snapshot("/api/operations/stream?page=1&page_size=5")
assert (page["total"], page["page"], page["page_size"], page["total_pages"]) == (12, 1, 5, 3), page
assert (page["active"], page["attention"]) == (5, 5), page
assert [item["id"] for item in page["list"]] == [op(n) for n in (1, 2, 3, 4, 5)], page["list"]
assert [item["status"] for item in page["list"]] == ["queued", "running", "retrying", "partial_failed", "retrying"]
last = snapshot("/api/operations/stream?page=3&page_size=5")
assert [item["id"] for item in last["list"]] == [op(8), op(10)] and last["total"] == 12, last
beyond = snapshot("/api/operations/stream?page=99&page_size=5")
assert beyond["list"] == [] and beyond["total"] == 12 and beyond["active"] == 5, beyond
assert snapshot("/api/operations/stream")["page_size"] == 20

# Filters; counters ignore them.
statuses = {"queued": 2, "running": 1, "retrying": 2, "failed": 1, "partial_failed": 1, "attention_required": 1,
            "completed": 2, "merged": 1, "cancelled": 1, "active": 5, "attention": 5}
for status, count in statuses.items():
    assert total("status=" + status) == count, status
filtered = snapshot("/api/operations/stream?status=merged")
assert (filtered["active"], filtered["attention"]) == (5, 5) and filtered["list"][0]["id"] == op(6), filtered
assert total("action=dns_zone.sync") == 2
assert total("action=website.create") == 0 and total("action=no.such_action") == 0
for target_type, count in {"dns_zone": 7, "cluster": 1, "node": 2, "provider": 2, "website": 0, "certificate": 0}.items():
    assert total("target_type=" + target_type) == count, target_type
assert total("actor=me") == 10 and total("actor=system") == 1
assert total("days=0") == 12 and total("days=1") == 10 and total("days=7") == 11
assert total("status=active&target_type=dns_zone") == 3
for keyword, count in {"%25": 1, "_": 1, "100%25_": 1, "%27": 0, "%5C": 0, "QA-1.EXAMPLE": 2, "+qa-1.example.com+": 2,
                       "000000000401": 1, "letsencrypt": 1}.items():
    assert total("keyword=" + keyword) == count, keyword
for query in ("status=unknown", "status=ACTIVE", "target_type=unknown", "actor=other", "days=2", "days=x", "page=0",
              "page=-1", "page_size=0", "page_size=101", "keyword=" + "a" * 201, "action=" + "a" * 49):
    snapshot("/api/operations/stream?" + query, expected_code=10001)

# Detail snapshots (contract 3.3/3.4).
detail = snapshot(f"/api/operations/{op(4)}/stream")
assert (detail["status"], detail["subtask_count"], detail["settled_count"], detail["failed_count"]) == \
    ("partial_failed", 3, 3, 1), detail
assert (detail["target_type"], detail["target_name"], detail["target_deleted"]) == ("cluster", "QA cluster", False)
assert detail["actor_type"] == "user" and detail["actor_name"] == "Administrator" and "target_kind" not in detail
assert detail["error"] == "QA deployment error" and detail["finished_at"] and detail["next_attempt_at"] == ""
assert detail["summary"] == {"sections": ["dns"]}
subtasks = {item["id"]: item for item in detail["subtasks"]}
assert [item["id"] for item in detail["subtasks"]] == [sub(4, 1), sub(4, 2), sub(4, 3)]
assert subtasks[sub(4, 3)]["parent_id"] == sub(4, 1) and subtasks[sub(4, 1)]["parent_id"] == ""
assert (subtasks[sub(4, 1)]["status"], subtasks[sub(4, 2)]["status"]) == ("completed", "failed")
assert subtasks[sub(4, 2)]["error"] == "QA deployment error" and subtasks[sub(4, 2)]["finished_at"]
child = subtasks[sub(4, 3)]
assert (child["kind"], child["status"], child["warning"], child["resource_deleted"]) == \
    ("dns_sync", "attention_required", "dns_conflict", True), child
for item in detail["subtasks"]:
    assert item["duration_ms"] >= 0 and item["running_since"] == "" and "merged_into" not in item, item
    assert item["node_offline"] is False and "closed_by" not in item, item

retrying = snapshot(f"/api/operations/{op(5)}/stream")
assert (retrying["status"], retrying["settled_count"], retrying["failed_count"]) == ("retrying", 1, 1), retrying
assert retrying["finished_at"] == "" and retrying["next_attempt_at"] == "" and not retrying["target_deleted"]
publish, config = retrying["subtasks"]
assert publish["status"] == "completed" and publish["node_offline"] is False, publish
assert (config["parent_id"], config["kind"], config["status"], config["node_offline"]) == \
    (sub(5, 1), "node_config", "retrying", True), config
assert config["error"] == "QA: node rejected config" and config["duration_ms"] == 0 and config["finished_at"] == ""

running = snapshot(f"/api/operations/{op(2)}/stream")
assert running["status"] == "running" and running["subtasks"][0]["running_since"], running
assert running["summary"] == {"conflict_policy": "local"}
merged = snapshot(f"/api/operations/{op(6)}/stream")
assert merged["status"] == "merged" and merged["finished_at"], merged
assert merged["subtasks"][0]["merged_into"] == {"operation_id": op(1), "subtask_id": sub(1), "action": "dns_zone.sync",
                                                "target_name": "qa-1.example.com", "status": "queued"}, merged
assert "closed_by" not in merged["subtasks"][0]
cancelled = snapshot(f"/api/operations/{op(7)}/stream")
assert cancelled["status"] == "cancelled" and cancelled["settled_count"] == 3, cancelled
closers = [(item["status"], item.get("closed_by")) for item in cancelled["subtasks"]]
assert closers == [
    ("cancelled", {"operation_id": op(2), "action": "dns_zone.sync_local", "target_name": "qa-2.example.com"}),
    ("merged", {"operation_id": op(3), "action": "dns_zone.update_records", "target_name": "qa-3.example.com"}),
    ("cancelled", None)], closers
system = snapshot(f"/api/operations/{op(8)}/stream")
assert (system["actor_type"], system["actor_name"], system["status"], system["subtasks"]) == ("system", "", "completed", [])
assert system["finished_at"] == system["created_at"], system
recovered = snapshot(f"/api/operations/{op(9)}/stream")
assert recovered["target_kind"] == "dns" and recovered["status"] == "completed", recovered
item = recovered["subtasks"][0]
assert (item["recovered"], item["attempt_count"], item["failure_count"], item["error"]) == (True, 2, 1, "")
assert snapshot(f"/api/operations/{op(10)}/stream")["target_kind"] == "certificate"
attention = snapshot(f"/api/operations/{op(12)}/stream")
assert attention["status"] == "attention_required" and attention["subtasks"][0]["warning"] == "dns_conflict"
failed = snapshot(f"/api/operations/{op(13)}/stream")
assert failed["status"] == "failed" and failed["subtasks"][0]["node_offline"] is False, failed

# Expanded subtask: snapshot first, then its history (newest first).
retry_task = snapshot(f"/api/operations/{op(3)}/stream")
assert retry_task["next_attempt_at"] and retry_task["error"] == "QA: API read failed", retry_task
[(first, head), (second, body)] = frames(f"/api/operations/{op(3)}/stream?subtask_id={sub(3)}", 2)
assert (first, head["code"], second, body["code"]) == ("snapshot", 0, "history", 0), (first, second)
assert head["data"] == retry_task, head
history = body["data"]
assert history["subtask_id"] == sub(3) and history["truncated"] is False, history
assert [(entry["attempt"], entry["outcome"], entry["error"]) for entry in history["list"]] == \
    [(2, "failed", "QA: API read failed"), (1, "failed", "QA: API timeout")], history["list"]
assert [entry["trace"]["attempt_id"] for entry in history["list"]] == ["990002", "990001"]
assert history["list"][0]["trace"]["state"] == "available" and history["list"][1]["trace"]["state"] == "none"
assert history["list"][1]["duration_ms"] == 2000 and history["list"][1]["started_at"]
assert history["list"][1]["detail"] == {"kind": "none", "truncated": False}
summary = history["summary"]
assert (summary["attempt_count"], summary["failure_count"], summary["last_success_at"]) == (2, 2, ""), summary
assert summary["first_failure_at"] == history["list"][1]["emitted_at"] and summary["next_attempt_at"], summary
recovery = frames(f"/api/operations/{op(9)}/stream?subtask_id={sub(9)}", 2)[1][1]["data"]
assert [entry["outcome"] for entry in recovery["list"]] == ["completed", "failed"]
assert recovery["summary"]["last_success_at"] == recovery["list"][0]["emitted_at"]
for foreign in (sub(3), sub(11), "00000000-0000-4000-8000-00000000ffff"):
    errors = frames(f"/api/operations/{op(1)}/stream?subtask_id={foreign}", 2)
    assert [(event, value["code"]) for event, value in errors] == [("resource-error", 16705)] * 2, (foreign, errors)
snapshot(f"/api/operations/{op(1)}/stream?subtask_id=invalid", expected_code=10001)

# Attempt traces.
trace = snapshot(f"/api/operations/{op(3)}/attempts/990002/trace/stream")
assert (trace["attempt_id"], trace["subtask_id"], trace["resource_type"], trace["resource_id"]) == \
    ("990002", sub(3), "dns_zone", "00000000-0000-4000-8000-000000000203"), trace
assert (trace["state"], trace["kind"], trace["attempt"], trace["outcome"], trace["error"]) == \
    ("available", "none", 2, "failed", "QA: API read failed"), trace
assert trace["worker_type"] == "dns-sync" and trace["capture"]["bytes"] == 64 and trace["steps"] == [], trace
assert "task_id" not in trace
untraced = snapshot(f"/api/operations/{op(3)}/attempts/990001/trace/stream")
assert (untraced["state"], untraced["attempt"], untraced["capture"]["bytes"]) == ("none", 1, 0), untraced
assert untraced["worker_type"] == "dns-sync" and untraced["duration_ms"] == 2000, untraced
# Another task's attempt, another tenant's attempt, an untracked attempt and
# an unknown one are all "not found" for this task.
for attempt_id in (990002, 990901, 990008, 999999999):
    snapshot(f"/api/operations/{op(1)}/attempts/{attempt_id}/trace/stream", expected_code=16706)
snapshot(f"/api/operations/{op(99)}/attempts/990002/trace/stream", expected_code=16706)
for attempt_id in ("0", "-1", "not-a-number"):
    snapshot(f"/api/operations/{op(3)}/attempts/{attempt_id}/trace/stream", expected_code=10001)
snapshot("/api/operations/invalid/attempts/990002/trace/stream", expected_code=10001)

# Unknown and foreign-tenant tasks; the removed API.
snapshot(f"/api/operations/{op(99)}/stream", expected_code=16701)
snapshot(f"/api/operations/{op(11)}/stream", expected_code=16701)
snapshot("/api/operations/invalid/stream", expected_code=10001)
assert all(item["id"] != op(11) for item in snapshot("/api/operations/stream?page_size=100")["list"])
request("/api/tasks/stream", 404)
print("Task SSE integration passed: authentication, pagination, filters, validation, detail roll-up, merged/closed_by, "
      "target kinds, subtask history, attempt traces, tenant isolation, removed /api/tasks.")
