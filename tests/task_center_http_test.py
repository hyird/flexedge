"""Read-only task SSE integration test; run with task_center_read_model_test.py --keep-fixture."""
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

def snapshot(path, expected_code=0):
    req = urllib.request.Request(base + path, headers={"Accept": "text/event-stream", "Cookie": cookie})
    try:
        response = urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as error:
        if expected_code in (10001, 10003):
            assert error.code == (400 if expected_code == 10001 else 404), (path, error.code)
            return None
        raise
    assert response.status == 200
    event, data = None, None
    for raw in response:
        line = raw.decode().rstrip("\r\n")
        if line.startswith("event: "): event = line[7:]
        elif line.startswith("data: "): data = line[6:]
        elif not line and event == "snapshot":
            value = json.loads(data)
            if expected_code == 10003:
                assert value["code"] != 0, (path, value)
            else:
                assert value["code"] == expected_code, (path, value)
            response.close()
            return value.get("data")
        elif not line and event == "resource-error":
            value = json.loads(data)
            response.close()
            if expected_code == 10003:
                assert value["code"] != 0, (path, value)
            else:
                assert value["code"] == expected_code, (path, value)
            return value
    raise AssertionError(f"SSE closed without snapshot: {path}")

request("/api/tasks/stream", 401, authenticated=False)
request("/api/auth/login", body={"username": "admin", "password": os.environ["TASK_QA_PASSWORD"]})
page = snapshot("/api/tasks/stream?page=1&page_size=6")
assert (page["total"], page["active"], page["failed"], len(page["list"])) == (21, 4, 1, 6)
assert snapshot("/api/tasks/stream?page=99&page_size=6")["total"] == 21
assert snapshot("/api/tasks/stream?page=99&page_size=6")["list"] == []
assert snapshot("/api/tasks/stream?type=node")["total"] == 4
assert snapshot("/api/tasks/stream?status=failed")["total"] == 1
assert snapshot("/api/tasks/stream?days=1")["total"] == 8
assert snapshot("/api/tasks/stream?keyword=%27%25_")["total"] == 0
assert snapshot("/api/tasks/stream?keyword=qa-3")["total"] == 2
for query in ("type=unknown", "status=unknown", "days=2", "page=-1"):
    snapshot("/api/tasks/stream?" + query, expected_code=10001)
task_id = "00000000-0000-4000-8000-000000000304"
resource_id = "00000000-0000-4000-8000-000000000204"
detail = snapshot(f"/api/tasks/{task_id}/stream?resource_id={resource_id}&version=2")
assert detail["status"] == "recovered" and detail["error"] == ""
history = snapshot(f"/api/tasks/{task_id}/history/stream?version=2")
assert [event["outcome"] for event in history["list"]] == ["completed", "failed"]
assert history["list"][1]["error"] == "retained error after recovery"
other = snapshot("/api/tasks/00000000-0000-4000-8000-000000000303/history/stream?version=2")
assert len(other["list"]) == 1 and other["list"][0]["outcome"] == "failed"
snapshot(f"/api/tasks/{task_id}/history/stream?version=0", expected_code=10001)
snapshot(f"/api/tasks/{task_id}/stream?resource_id={resource_id}&version=999", expected_code=10003)
snapshot(f"/api/tasks/{task_id}/stream?resource_id=invalid&version=2", expected_code=10001)
print("Task SSE integration passed: authentication, pagination, filters, validation, detail, retained errors, tenant isolation.")
