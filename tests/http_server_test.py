#!/usr/bin/env python3
"""Exercise server dotenv configuration and real HTTP/TLS/SSE/WebSocket traffic."""

import contextlib
import http.client
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def request(port, path="/api/health", *, context=None, method="GET", host=None):
    connection = (
        http.client.HTTPSConnection("127.0.0.1", port, context=context, timeout=3)
        if context else http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    )
    try:
        headers = {"Host": host} if host else {}
        connection.request(method, path, body=b"payload" if method == "POST" else None, headers=headers)
        response = connection.getresponse()
        return response.status, dict((key.lower(), value) for key, value in response.getheaders()), response.read()
    finally:
        connection.close()


def websocket(port, context):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as transport:
        with context.wrap_socket(transport, server_hostname="localhost") as connection:
            connection.sendall((
                f"GET /socket HTTP/1.1\r\nHost: localhost:{port}\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
            ).encode("ascii"))
            with connection.makefile("rb") as stream:
                require(stream.readline().startswith(b"HTTP/1.1 101 "), "WSS upgrade failed")
                headers = {}
                while line := stream.readline().strip():
                    key, value = line.split(b":", 1)
                    headers[key.lower()] = value.strip()
                require(headers.get(b"sec-websocket-accept") == b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "WSS accept mismatch")
                require(stream.read(2) == b"\x81\x05" and stream.read(5) == b"ready", "WSS data missing")
                # Finish the close handshake rather than leave a fixture connection alive.
                connection.sendall(b"\x88\x82\x00\x00\x00\x00\x03\xe8")


def main():
    binary = str(Path(sys.argv[1]).resolve())
    environment = os.environ.copy()
    for name in ("HOST", "PORT", "HTTPS_CERT_FILE", "HTTPS_KEY_FILE", "HTTPS_PORT", "HTTP_REDIRECT_TO_HTTPS",
                 "WORKER_THREADS", "BLOCKING_THREADS", "BLOCKING_QUEUE_CAPACITY", "COMPRESSION_MIN_BYTES"):
        environment.pop(name, None)
    with tempfile.TemporaryDirectory(prefix="flexedge-https-") as temporary:
        directory = Path(temporary)
        env_path = directory / ".env"

        def run(mode, *, check=True):
            return subprocess.run([binary, mode, str(env_path)], env=environment, cwd=directory.parent,
                                  capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=8, check=check)

        def write_env(**values):
            env_path.write_text("".join(f'{name}="{value}"\n' for name, value in values.items()), encoding="utf-8")

        def reject(expected, *, mode="--inspect", **values):
            write_env(**values)
            result = run(mode, check=False)
            require(result.returncode != 0 and expected.lower() in result.stderr.lower(),
                    f"invalid configuration accepted or wrong error: {values!r}: {result.stderr}")

        write_env()
        require("http=1102 https=0 redirect=0" in run("--inspect").stdout, "HTTP defaults changed")
        subprocess.run([binary, "--generate-identity", str(directory)], env=environment, check=True, timeout=8)
        certificates = directory / "证书"
        certificates.mkdir()
        for name in ("certificate.pem", "key.pem"):
            (directory / name).rename(certificates / name)
        tls = {"HTTPS_CERT_FILE": "证书/certificate.pem", "HTTPS_KEY_FILE": "证书/key.pem"}
        write_env(**tls)
        require("http=1102 https=443 redirect=0" in run("--check").stdout, "HTTPS 443/default redirect or UTF-8 relative path failed")
        write_env(HTTPS_CERT_FILE=(certificates / "certificate.pem").as_posix(), HTTPS_KEY_FILE=(certificates / "key.pem").as_posix())
        run("--check")

        for name in ("HTTPS_CERT_FILE", "HTTPS_KEY_FILE"):
            reject("must both be configured", **{name: tls[name]})
        reject("must be true or false", HTTP_REDIRECT_TO_HTTPS="1")
        reject("require HTTPS_CERT_FILE", HTTP_REDIRECT_TO_HTTPS="true")
        reject("require HTTPS_CERT_FILE", HTTPS_PORT="443")
        reject("different from PORT", **tls, HTTPS_PORT="1102")
        for value in ("0", "65536", "-1", "https", ""):
            reject("HTTPS_PORT", **tls, HTTPS_PORT=value)
        reject("PORT", PORT="0")
        reject("PORT", PORT="65536")
        reject("HOST", HOST="")
        reject("certificate", mode="--check", HTTPS_CERT_FILE="missing.pem", HTTPS_KEY_FILE=tls["HTTPS_KEY_FILE"])
        (directory / "invalid.pem").write_text("not a PEM file", encoding="ascii")
        reject("certificate", mode="--check", HTTPS_CERT_FILE="invalid.pem", HTTPS_KEY_FILE=tls["HTTPS_KEY_FILE"])
        subprocess.run([binary, "--generate-identity", str(directory)], env=environment, check=True, timeout=8)
        reject("key", mode="--check", HTTPS_CERT_FILE=tls["HTTPS_CERT_FILE"], HTTPS_KEY_FILE="key.pem")

        (directory / "index.html").write_text("https-static-ready", encoding="ascii")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.load_verify_locations(cafile=str(certificates / "certificate.pem"))

        @contextlib.contextmanager
        def serve(*, secure=False, redirect=None):
            # Hold both reservations together so HTTP and HTTPS cannot select the same port.
            with socket.socket() as http_reservation, socket.socket() as https_reservation:
                http_reservation.bind(("127.0.0.1", 0))
                https_reservation.bind(("127.0.0.1", 0))
                http_port, https_port = http_reservation.getsockname()[1], https_reservation.getsockname()[1]
            values = {"HOST": "127.0.0.1", "PORT": http_port}
            if secure:
                values.update(tls, HTTPS_PORT=https_port)
            if redirect is not None:
                values["HTTP_REDIRECT_TO_HTTPS"] = redirect
            write_env(**values)
            with tempfile.TemporaryFile(mode="w+b") as output:
                process = subprocess.Popen([binary, "--serve", str(env_path)], env=environment,
                                           cwd=directory.parent, stdin=subprocess.DEVNULL, stdout=output, stderr=output)
                try:
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            output.seek(0)
                            raise RuntimeError("fixture exited: " + output.read().decode("utf-8", errors="replace"))
                        try:
                            status, _, body = request(https_port if secure else http_port, context=context if secure else None)
                            if status == 200 and body == b"ready":
                                break
                        except (OSError, http.client.HTTPException):
                            pass
                        time.sleep(0.04)  # Startup readiness only.
                    else:
                        raise RuntimeError("fixture startup timed out")
                    yield http_port, https_port
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)

        with serve() as (http_port, _):
            status, headers, body = request(http_port)
            require(status == 200 and body == b"ready" and "location" not in headers, "plain HTTP regressed")

        for redirect in (None, "false", "true"):
            with serve(secure=True, redirect=redirect) as (http_port, https_port):
                status, _, body = request(https_port, context=context)
                require(status == 200 and body == b"ready", "verified TLS health failed")
                require(request(https_port, "/", context=context)[2] == b"https-static-ready", "HTTPS static file failed")
                status, headers, body = request(https_port, "/events", context=context)
                require(status == 200 and headers["content-type"].startswith("text/event-stream") and
                        b"event: snapshot" in body and b"data: ready" in body, "HTTPS SSE failed")
                websocket(https_port, context)
                if redirect != "true":
                    require(request(http_port)[0] == 200, "HTTP redirected despite default/false switch")
                    require(request(http_port, "/echo", method="POST")[2] == b"payload", "HTTP POST regressed")
                    continue
                for method in ("GET", "HEAD", "POST"):
                    for path in ("/api/health?name=a%2Fb&empty=", "/echo?", "/"):
                        status, headers, _ = request(http_port, path, method=method, host=f"example.test:{http_port}")
                        require(status == 308 and headers.get("location") == f"https://example.test:{https_port}{path}",
                                f"{method} redirect lost authority/path/query: {status}, {headers}")
                status, headers, _ = request(http_port, host="[::1]:80")
                require(status == 308 and headers.get("location") == f"https://[::1]:{https_port}/api/health", "IPv6 redirect invalid")
                require(request(https_port, "/echo", method="POST", context=context)[2] == b"payload", "HTTPS POST failed")

    print("HTTP defaults, HTTPS 443 configuration, redirect default/false/true, TLS verification, UTF-8 paths, static files, SSE, WSS and startup failures passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
