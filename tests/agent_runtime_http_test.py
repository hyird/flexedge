"""Real node HTTP/WS integration on a disposable loopback server and QA cluster.

Requires NODE_QA_URL, NODE_QA_CLUSTER, NODE_QA_BINARY, NODE_QA_WEBSITE_CONFIG,
and AUTH_QA_PASSWORD. The JSON config should come from defaultWebsiteConfig().
"""
from contextlib import ExitStack
import http.cookiejar
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.request
import urllib.parse
import uuid
from support.tcp_fault_proxy import TcpFaultProxy


def stop_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)


def main():
    with ExitStack() as resources:
        run(resources)


def run(resources):
    base = os.environ["NODE_QA_URL"].rstrip("/")
    url = urllib.parse.urlsplit(base)
    if url.scheme != "http" or url.hostname != "127.0.0.1" or url.path:
        raise SystemExit("Requires isolated loopback HTTP server")
    cluster = str(uuid.UUID(os.environ["NODE_QA_CLUSTER"]))
    binary = Path(os.environ["NODE_QA_BINARY"]).resolve(strict=True)
    website_config = json.loads(Path(os.environ["NODE_QA_WEBSITE_CONFIG"]).read_text(encoding="utf-8-sig"))
    client = urllib.request.build_opener(urllib.request.ProxyHandler({}),
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def request(path, method="GET", data=None, revision=None):
        headers = {"Content-Type": "application/json"}
        if revision is not None:
            headers["If-Match"] = '"' + str(revision) + '"'
        req = urllib.request.Request(base + "/api/" + path, method=method, headers=headers,
            data=None if data is None else json.dumps(data).encode())
        with client.open(req, timeout=10) as response:
            result = json.load(response)
        if result["code"] != 0:
            raise AssertionError(f"{path}: business error {result['code']}")
        return result.get("data")

    name = "Agent-E2E-" + uuid.uuid4().hex[:10]
    hostname = name.lower() + ".agent-qa.invalid"
    marker = uuid.uuid4().hex
    class Origin(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = json.dumps({"marker":marker,"host":self.headers.get("Host"),"path":self.path}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        def log_message(self, *_):
            pass
    origin = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin)
    resources.callback(origin.server_close)
    origin_thread = threading.Thread(target=origin.serve_forever, daemon=True)
    origin_thread.start()
    resources.callback(origin_thread.join, timeout=5)
    resources.callback(origin.shutdown)
    website_config.update({"name":name,"domains":[{"id":str(uuid.uuid4()),"hostname":hostname,"dns_mode":"external"}],
        "origins":[{"id":str(uuid.uuid4()),"group":"default","protocol":"http","host":"127.0.0.1",
            "port":origin.server_port,"role":"primary","weight":100,"status":"enabled"}],
        "health_check_enabled":False})
    config = {"cluster_id":cluster,"name":name,"status":"enabled",
              "config":{"endpoints":[{"id":str(uuid.uuid4()),"ip_address":"127.0.0.42","line_code":"default"}]}}
    process = None
    node_id = website_id = None
    request("auth/login", "POST", {"username":"admin", "password":os.environ["AUTH_QA_PASSWORD"]})
    resources.callback(request, "auth/logout", "POST", {})
    directory = resources.enter_context(tempfile.TemporaryDirectory(prefix="flexedge-agent-e2e-"))
    work = Path(directory)
    shutil.copy2(binary, work / binary.name)
    log = work / "node.log"
    proxy = TcpFaultProxy(url.port or 80)
    resources.callback(proxy.close)
    website_path = "websites?cluster_id=" + cluster
    request(website_path, "POST", {"status":"enabled","config":website_config})
    website_id = next(w["id"] for w in request(website_path + "&page_size=100")["list"] if w["config"]["name"] == name)
    resources.callback(lambda: request("websites/" + website_id, "DELETE", revision=request("websites/" + website_id)["revision"]))
    credentials = request("nodes", "POST", config)
    (work / "credentials").write_text("node_id=" + credentials["node_id"] + "\nsecret=" + credentials["secret"] + "\n", encoding="utf-8", newline="\n")
    def current():
        return next(n for n in request("nodes?cluster_id=" + cluster + "&page_size=100")["list"] if n["name"] == name)
    node_id = current()["id"]
    resources.callback(lambda: request("nodes/" + node_id, "DELETE", revision=current()["revision"]))
    def launch():
        with log.open("ab") as output:
            child = subprocess.Popen([str(work / binary.name), "ws://127.0.0.1:" + str(proxy.port), str(work / "credentials")],
                cwd=work, stdout=output, stderr=output,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        resources.callback(stop_process, child)
        return child
    def applied(revision, old_heartbeat=None, previous_release=None):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError("node exited: " + log.read_text(errors="replace")[-2000:])
            node = current()
            runtime = node["runtime"]
            if (runtime["registration_status"] == "registered"
                and runtime["applied_node_spec_revision"] == revision
                and runtime.get("active_release_id") and runtime.get("active_manifest_digest")
                and (old_heartbeat is None or runtime.get("last_heartbeat_at") != old_heartbeat)
                and (previous_release is None or runtime["active_release_id"] != previous_release)):
                return node
            time.sleep(0.25)
        raise AssertionError("node apply timed out: " + log.read_text(errors="replace")[-2000:])
    request_number = 0
    seen_log_ids = set()
    def forwarded(expected_host, wait_for_log=True):
        nonlocal request_number
        request_number += 1
        target = f"/probe?qa={marker}&step={request_number}"
        endpoint = config["config"]["endpoints"][0]["ip_address"]
        req = urllib.request.Request("http://" + endpoint + target, headers={"Host":hostname})
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=10) as response:
            actual = json.load(response)
        if actual != {"marker":marker,"host":expected_host,"path":target}:
            raise AssertionError("origin response mismatch")
        if wait_for_log:
            delivered(target)
        return target
    def delivered(target):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            records = request("websites/" + website_id + "/access-logs?page_size=100")["list"]
            matches = [item for item in records if item["target"] == target]
            if matches:
                if len(matches) != 1:
                    raise AssertionError("duplicate access log for a single request")
                item = matches[0]
                if (item["node_id"] != node_id or item["host"] != hostname
                    or item["method"] != "GET" or item["protocol"] != "http"
                    or item["status_code"] != 200 or item["response_bytes"] <= 0
                    or item.get("query_string") != target.split("?", 1)[1]
                    or item["id"] in seen_log_ids):
                    raise AssertionError("access log correlation mismatch")
                seen_log_ids.add(item["id"])
                return
            time.sleep(0.25)
        raise AssertionError("access log delivery timed out")
    process = launch()
    first = applied(1)
    if not (work / "state/active/state.pb").is_file() or not any((work / "state/objects").iterdir()):
        raise AssertionError("active state or delivery objects were not persisted")
    forwarded(hostname)
    original_pid = process.pid
    if proxy.cut() < 1:
        raise AssertionError("no live connection was interrupted")
    connection_count = proxy.connections
    website_config["origin_host_header"] = "updated.agent-qa.invalid"
    website = request("websites/" + website_id)
    request("websites/" + website_id + "?cluster_id=" + cluster, "PUT",
        {"status":"enabled","config":website_config}, website["revision"])
    offline_target = forwarded(hostname, wait_for_log=False)
    time.sleep(1)
    if current()["runtime"]["active_release_id"] != first["runtime"]["active_release_id"]:
        raise AssertionError("offline node applied a new release")
    offline_logs = request("websites/" + website_id + "/access-logs?page_size=100")["list"]
    if any(item["target"] == offline_target for item in offline_logs):
        raise AssertionError("log unexpectedly crossed the disconnected proxy")
    proxy.restore()
    changed = applied(1, previous_release=first["runtime"]["active_release_id"])
    if process.pid != original_pid or proxy.connections <= connection_count:
        raise AssertionError("same-process reconnect was not observed")
    delivered(offline_target)
    forwarded("updated.agent-qa.invalid")
    config["config"]["endpoints"][0]["ip_address"] = "127.0.0.43"
    request("nodes/" + node_id, "PUT", config, changed["revision"])
    second = applied(2)
    forwarded("updated.agent-qa.invalid")
    stop_process(process)
    process = launch()
    restored = applied(2, old_heartbeat=second["runtime"].get("last_heartbeat_at"))
    if restored["runtime"]["active_release_id"] != second["runtime"]["active_release_id"]:
        raise AssertionError("restart changed active release unexpectedly")
    forwarded("updated.agent-qa.invalid")
    print("Real node passed: object delivery, HTTP forwarding, website publication, endpoint update, persisted restart, same-process reconnect, offline log recovery, five correlated access logs")


if __name__ == "__main__":
    main()