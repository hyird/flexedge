"""Exercise real auth endpoints on an explicitly supplied loopback QA server.

Usage: python auth_http_test.py http://127.0.0.1:51102
AUTH_QA_PASSWORD must identify the isolated server's admin fixture.
The final scenario locks that fixture: recreate the isolated database or clear
its admin throttle record before running again.
"""
import http.cookiejar
import http.cookies
import concurrent.futures
import threading
import json
import os
import sys
import uuid
import urllib.error
import urllib.parse
import urllib.request


def main():
    base = sys.argv[1].rstrip("/")
    url = urllib.parse.urlsplit(base)
    if url.scheme != "http" or url.hostname not in {"localhost", "127.0.0.1", "::1"} or url.path:
        raise SystemExit("an isolated loopback HTTP server is required")
    password = os.environ["AUTH_QA_PASSWORD"]
    jar = http.cookiejar.CookieJar()
    client = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))

    def request(path, data=None, cookie=None):
        headers = {"Content-Type": "application/json"}
        if cookie is not None:
            headers["Cookie"] = cookie
        req = urllib.request.Request(base + "/api/auth/" + path,
            data=None if data is None else json.dumps(data).encode(), headers=headers)
        opener = client if cookie is None else urllib.request.build_opener()
        try:
            response = opener.open(req, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def expect(path, data, status, cookie=None):
        actual, body = request(path, data, cookie)
        if actual != status or (status == 200 and body.get("code") != 0):
            raise AssertionError(f"{path}: expected HTTP {status}, got {actual}, code={body.get('code')}")
        return body

    expect("me", None, 401)
    expect("login", {"username": "admin", "password": "incorrect-password"}, 401)
    credentials = {"username": "admin", "password": password}
    expect("login", credentials, 200)
    session = next(c for c in jar if c.name == "flexedge_session")
    if session.path != "/api" or not session.has_nonstandard_attr("HttpOnly"):
        raise AssertionError("session cookie scope or HttpOnly attribute")
    old_cookie = "flexedge_session=" + session.value
    expect("me", None, 200)
    expect("refresh", {}, 200)
    if next(c.value for c in jar if c.name == "flexedge_session") == session.value:
        raise AssertionError("refresh failed to rotate credential")
    expect("me", None, 200)
    expect("refresh", {}, 401, old_cookie)
    expect("me", None, 401)
    expect("login", credentials, 200)
    expect("logout", {}, 200)
    if any(c.name == "flexedge_session" for c in jar):
        raise AssertionError("logout did not clear cookie")
    expect("me", None, 401)
    expect("login", credentials, 200)
    concurrent_cookie = "flexedge_session=" + next(c.value for c in jar if c.name == "flexedge_session")
    barrier = threading.Barrier(2)

    def refresh_concurrently():
        req = urllib.request.Request(base + "/api/auth/refresh", data=b"{}",
            headers={"Content-Type": "application/json", "Cookie": concurrent_cookie})
        barrier.wait(timeout=5)
        try:
            response = urllib.request.urlopen(req, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            response.read()
            return response.status, response.headers.get("Set-Cookie", "")

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(refresh_concurrently) for _ in range(2)]
        replies = [future.result() for future in futures]
    if sorted(status for status, _ in replies) != [200, 401]:
        raise AssertionError("concurrent refresh must have one success and one rejection")
    winner = http.cookies.SimpleCookie(next(cookie for status, cookie in replies if status == 200))
    expect("me", None, 401, "flexedge_session=" + winner["flexedge_session"].value)
    jar.clear()
    # Give the missing-user throttle scenario a fresh identity on every run.
    locked_identity = {"username": "qa-" + uuid.uuid4().hex, "password": "incorrect-password"}
    for _ in range(4):
        expect("login", locked_identity, 401)
    expect("login", locked_identity, 429)
    expect("login", locked_identity, 429)
    for _ in range(4):
        expect("login", {"username": "admin", "password": "incorrect-password"}, 401)
    expect("login", {"username": "admin", "password": "incorrect-password"}, 429)
    expect("login", credentials, 429)
    print("Auth HTTP passed: login, cookies, rotation, concurrent replay, logout and login locking.")


if __name__ == "__main__":
    main()
