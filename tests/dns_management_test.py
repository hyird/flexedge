"""Exercise production DNS writes against a controlled loopback provider."""
import http.server
import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

if not os.environ.get("PGHOST") or not os.environ.get("REDIS_TEST_PORT"):
    raise SystemExit(77)

records = {
    f"remote-{index}": {"id": f"remote-{index}", "type": "A", "name": f"{name}.example.com",
                       "content": f"192.0.2.{index}", "ttl": 600, "proxied": False}
    for index, name in [(1, "www"), (2, "other")]
}
writes = []


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, result):
        body = json.dumps({"success": True, "errors": [], "result": result,
                           "result_info": {"page": 1, "total_pages": 1, "total_count": len(result)}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if "dns_records" in self.path:
            self.reply(list(records.values()))
        else:
            self.reply([{"id": "zone", "name": "example.com", "status": "active"}])

    def do_POST(self):
        self.mutate()

    def do_PUT(self):
        self.mutate()

    def do_PATCH(self):
        self.mutate()

    def do_DELETE(self):
        key = self.path.rsplit("/", 1)[-1]
        writes.append(("DELETE", key))
        records.pop(key)
        self.reply({"id": key})

    def mutate(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        key = f"remote-{len(records) + 1}" if self.command == "POST" else self.path.rsplit("/", 1)[-1]
        writes.append((self.command, key))
        records[key] = {**data, "id": key}
        self.reply(records[key])


provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
thread = threading.Thread(target=provider.serve_forever, daemon=True)
thread.start()
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
env = {**os.environ, "DNS_TEST_ORIGIN_PORT": str(provider.server_port)}
process = subprocess.Popen([sys.argv[1], str(port)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
base = f"http://127.0.0.1:{port}/records"


def request(body=None, revision=None, expected=200, path="/records"):
    headers = {"Content-Type": "application/json"}
    if revision is not None:
        headers["If-Match"] = f'"{revision}"'
    req = urllib.request.Request(base.removesuffix("/records") + path, headers=headers, method="GET" if body is None else "PUT",
                                 data=None if body is None else json.dumps({"records": body}).encode())
    try:
        response = urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = json.load(response)
        assert response.status == expected, (response.status, data)
        return data


try:
    for _ in range(100):
        try:
            snapshot = request()
            break
        except (OSError, ValueError):
            if process.poll() is not None:
                raise AssertionError(process.communicate()[0])
            time.sleep(0.05)
    else:
        raise AssertionError("DNS fixture did not start")
    original = snapshot["config"]["records"]
    original_task = request(path="/task")
    for record_id, name in [("01000000-0000-0000-0000-000000000007", "edge"),
                            ("01000000-0000-0000-0000-000000000008", "retired")]:
        protected = {**original[0], "id": record_id, "name": name}
        response = request(original + [protected], snapshot["revision"], 422)
        assert str(response["code"]) == "16208", response
    assert not writes, "system ownership rejection reached the provider"
    records["remote-2"]["content"] = "198.51.100.2"  # Edited in the provider's console.
    edited = [{**record, "content": "192.0.2.9"} if record["name"] == "www" else record for record in original]
    request(edited, snapshot["revision"])
    assert writes == [("PUT", "remote-1")], writes
    assert records["remote-2"]["content"] == "198.51.100.2"
    snapshot = request()
    assert snapshot["desired_revision"] == 1 and snapshot["sync_status"] == "synced", snapshot
    assert request(path="/task") == original_task, "ordinary edits reset the system sync task"
    assert next(r for r in snapshot["config"]["records"] if r["name"] == "other")["content"] == "198.51.100.2"
    before = len(writes)
    request(original, 1, 412)
    assert len(writes) == before, "stale If-Match reached the provider"
    added = {"id": "01000000-0000-0000-0000-000000000006", "type": "TXT", "name": "verify",
             "content": "qa-token", "ttl": 600, "proxied": False, "line_code": "default"}
    request(snapshot["config"]["records"] + [added], snapshot["revision"])
    assert writes[-1] == ("POST", "remote-3"), writes
    snapshot = request()
    assert next(r for r in snapshot["config"]["records"] if r["name"] == "verify")["id"] == added["id"]
    request([r for r in snapshot["config"]["records"] if r["name"] != "www"], snapshot["revision"])
    assert writes[-1] == ("DELETE", "remote-1"), writes
    assert set(records) == {"remote-2", "remote-3"}
    assert request(path="/task") == original_task, "ordinary edits changed task state or history"
    print("DNS management: direct provider writes, external edits preserved, stable ids, If-Match, system ownership and no redundant sync task passed (loopback provider).")
finally:
    process.terminate()
    try:
        output = process.communicate(timeout=15)[0]
    except subprocess.TimeoutExpired:
        process.kill()
        output = process.communicate()[0]
    if process.returncode not in (0, -15):
        print(output, file=sys.stderr)
    provider.shutdown()
