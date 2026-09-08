"""Run against a local server with task_center_read_model_test.py --keep-fixture.

TASK_QA_PASSWORD is the isolated server's admin password. This test is read-only.
"""
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


request("/api/tasks", 401, authenticated=False)
request("/api/auth/login", body={"username": "admin", "password": os.environ["TASK_QA_PASSWORD"]})
page = request("/api/tasks?page=1&page_size=6")["data"]
assert (page["total"], page["active"], page["failed"], len(page["list"])) == (21, 4, 1, 6)
assert request("/api/tasks?page=99&page_size=6")["data"]["total"] == 21
assert request("/api/tasks?page=99&page_size=6")["data"]["list"] == []
assert request("/api/tasks?type=node")["data"]["total"] == 4
assert request("/api/tasks?status=failed")["data"]["total"] == 1
assert request("/api/tasks?days=1")["data"]["total"] == 8
assert request("/api/tasks?keyword=%27%25_")["data"]["total"] == 0
assert request("/api/tasks?keyword=qa-3")["data"]["total"] == 2
for query in ("type=unknown", "status=unknown", "days=2", "page=-1"):
    request("/api/tasks?" + query, 400)
task_id = "00000000-0000-4000-8000-000000000304"
resource_id = "00000000-0000-4000-8000-000000000204"
detail = request(f"/api/tasks/{task_id}?resource_id={resource_id}&version=2")["data"]
assert detail["status"] == "recovered"
assert detail["error"] == ""
history = request(f"/api/tasks/{task_id}/history?version=2")["data"]
assert [event["outcome"] for event in history["list"]] == ["completed", "failed"]
assert history["list"][1]["error"] == "retained error after recovery"
other = request("/api/tasks/00000000-0000-4000-8000-000000000303/history?version=2")["data"]
assert len(other["list"]) == 1 and other["list"][0]["outcome"] == "failed"
request(f"/api/tasks/{task_id}/history?version=0", 400)
request(f"/api/tasks/{task_id}?resource_id={resource_id}&version=999", 404)
request(f"/api/tasks/{task_id}?resource_id=invalid&version=2", 400)
print("Task HTTP integration passed: authentication, pagination, filters, validation, detail, retained errors, tenant isolation.")
