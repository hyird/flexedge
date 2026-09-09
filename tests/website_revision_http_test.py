"""Verify revision error priority against an isolated local server and valid website fixture.

Set WEBSITE_QA_URL, WEBSITE_QA_ID and AUTH_QA_PASSWORD. The test updates and restores the website name; the fixture must be disposable
and have a valid, round-trippable configuration.
"""
import http.cookiejar
import json
import os
import urllib.error
import urllib.parse
import urllib.request
import uuid


def main():
    base = os.environ["WEBSITE_QA_URL"].rstrip("/")
    url = urllib.parse.urlsplit(base)
    if url.scheme != "http" or url.hostname not in {"127.0.0.1", "localhost", "::1"} or url.path:
        raise SystemExit("an isolated loopback HTTP server is required")
    website = str(uuid.UUID(os.environ["WEBSITE_QA_ID"]))
    client = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def request(path, status=200, code=0, method="GET", body=None, revision=None, etag=None):
        headers = {"Content-Type": "application/json"}
        if revision is not None:
            headers["If-Match"] = f'"{revision}"'
        req = urllib.request.Request(base + path, method=method, headers=headers,
            data=None if body is None else json.dumps(body).encode())
        try:
            response = client.open(req, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = json.load(response)
            if (response.status, data.get("code")) != (status, code):
                raise AssertionError((path, response.status, data.get("code"), status, code))
            if etag is not None and response.headers.get("ETag") != f'"{etag}"':
                raise AssertionError("unexpected revision ETag")
            return data.get("data")

    request("/api/auth/login", method="POST", body={
        "username": "admin", "password": os.environ["AUTH_QA_PASSWORD"]})
    try:
        before = request(f"/api/websites/{website}")
        body = {"status": before["status"], "config": before["config"]}
        revision = before["revision"]
        missing_cluster = str(uuid.uuid4())
        path = f"/api/websites/{website}?cluster_id={missing_cluster}"
        request(path, 412, 16614, "PUT", body, revision + 1)
        request(path, 422, 16604, "PUT", body, revision)
        request(f"/api/websites/{uuid.uuid4()}?cluster_id={missing_cluster}",
            404, 16601, "PUT", body, revision)
        after = request(f"/api/websites/{website}")
        for key in ("revision", "config", "status", "cluster_id", "updated_at"):
            if before[key] != after[key]:
                raise AssertionError(f"rejected updates changed {key}")
        valid_path = f"/api/websites/{website}?cluster_id={before['cluster_id']}"
        changed = {"status": before["status"], "config": dict(before["config"])}
        changed["config"]["name"] = "revision-http-" + str(uuid.uuid4())
        request(valid_path, method="PUT", body=changed, revision=revision, etag=revision + 1)
        updated = request(f"/api/websites/{website}", etag=revision + 1)
        if updated["revision"] != revision + 1 or updated["config"] != changed["config"]:
            raise AssertionError("successful update did not persist the expected revision and config")
        request(valid_path, 412, 16614, "PUT", body, revision)
        request(valid_path, method="PUT", body=body, revision=revision + 1, etag=revision + 2)
        restored = request(f"/api/websites/{website}", etag=revision + 2)
        if restored["revision"] != revision + 2 or restored["config"] != before["config"]:
            raise AssertionError("restoring the fixture did not preserve revision progression")
    finally:
        request("/api/auth/logout", method="POST", body={})
    print("Website HTTP revision priority, successful updates, ETags and stale-write rejection passed")


if __name__ == "__main__":
    main()