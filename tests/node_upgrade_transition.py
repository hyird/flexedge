"""Real Linux node upgrade acceptance, using only Python's standard library.

Run: python3 tests/node_upgrade_transition.py NODE_BINARY FIXTURE_BINARY
Both binaries are copied into a TemporaryDirectory before execution. The fixture
is built from tests/support/node_upgrade_fixture.cpp with the same includes and
libraries as http2_runtime (flexedge_node_protocol, flexedge_route_policy,
ruvia::core, ruvia::http, OpenSSL::SSL, OpenSSL::Crypto).

Fixture contract:
  state ROOT HTTP_PORT HTTPS_PORT ORIGIN_PORT writes encrypted ROOT/state and
    ROOT/credentials; prints node_id/secret/revision/release_id/manifest_digest JSON.
  h2 HTTP_PORT performs one public-Ruvia cleartext H2 request; prints JSON lines
    first (>=4096 body bytes), goaway (NO_ERROR), complete (65536 verified bytes).

No shell replacement, installed service, public network, or real state is used.
The connection checks cover this bounded local transition, not unlimited backlog
capacity or a zero-latency upgrade guarantee.
"""
import contextlib
import errno
import hashlib
import http.client
import json
import pathlib
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from node_control_shutdown import fields, handshake, number, receive, text


BODY = b"x" * 65536
CHUNK = 256 * 1024
TIMEOUT = 45


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def send(peer, payload):
    """Server binary WS frame, including the required 64-bit large-frame form."""
    size = len(payload)
    if size < 126:
        header = bytes((0x82, size))
    elif size <= 65535:
        header = b"\x82\x7e" + struct.pack("!H", size)
    else:
        header = b"\x82\x7f" + struct.pack("!Q", size)
    peer.sendall(header + payload)


def listener():
    result = socket.socket()
    result.bind(("127.0.0.1", 0))
    result.listen(64)
    result.settimeout(0.1)
    return result


def wait_for(predicate, failures, description, timeout=TIMEOUT):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if failures:
            raise failures[0]
        if predicate():
            return
        time.sleep(0.01)
    if failures:
        raise failures[0]
    raise AssertionError("Timed out: " + description)


class Origin:
    def __init__(self, failures):
        self.failures = failures
        self.socket = listener()
        self.port = self.socket.getsockname()[1]
        self.release = threading.Event()
        self.stopping = threading.Event()
        self.peers = []
        self.threads = []
        self.released_at = []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        try:
            while not self.stopping.is_set():
                try:
                    peer, _ = self.socket.accept()
                except socket.timeout:
                    continue
                self.peers.append(peer)
                thread = threading.Thread(target=self.serve, args=(peer,), daemon=True)
                self.threads.append(thread)
                thread.start()
        except OSError as error:
            if not self.stopping.is_set():
                self.failures.append(error)

    def serve(self, peer):
        try:
            with peer:
                peer.settimeout(TIMEOUT)
                request = bytearray()
                while not request.endswith(b"\r\n\r\n"):
                    block = peer.recv(1)
                    if not block:
                        return
                    request.extend(block)
                    require(len(request) < 65536, "oversized origin request")
                path = request.split(b" ", 2)[1]
                slow = path in (b"/slow-h1", b"/slow-h2")
                body = BODY if slow else b"ready"
                peer.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                             b"Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                             + str(len(body)).encode() + b"\r\n\r\n" + body[:4096])
                if slow:
                    require(self.release.wait(TIMEOUT), "origin body was never released")
                    if self.stopping.is_set():
                        return
                    self.released_at.append(time.monotonic())
                    peer.sendall(body[4096:])
        except AssertionError as error:
            self.failures.append(error)
        except OSError as error:
            if not self.stopping.is_set():
                self.failures.append(error)

    def close(self):
        self.stopping.set()
        self.release.set()
        self.socket.close()
        for peer in self.peers:
            with contextlib.suppress(OSError):
                peer.shutdown(socket.SHUT_RDWR)
        self.thread.join(2)
        for thread in self.threads:
            thread.join(2)
        require(not self.thread.is_alive() and not any(t.is_alive() for t in self.threads),
                "origin threads did not terminate")


class Control:
    def __init__(self, metadata, original_digest, candidate, failures, valid, startup_recovery=False, desired=b""):
        self.metadata = metadata
        self.original_digest = original_digest
        self.candidate = candidate
        self.digest = hashlib.sha256(candidate).hexdigest()
        self.failures = failures
        self.valid = valid
        self.startup_recovery = startup_recovery
        self.desired = desired
        self.withdrawn = threading.Event()
        self.socket = listener()
        self.port = self.socket.getsockname()[1]
        self.offer = threading.Event()
        self.held = threading.Event()
        self.resume = threading.Event()
        self.sent_all = threading.Event()
        self.stopping = threading.Event()
        self.authenticated = []
        self.heartbeats = []
        self.logs = []
        self.offsets = []
        self.held_at = None
        self.resumed_at = None
        self.peer = None
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def advertised_digest(self):
        if self.withdrawn.is_set():
            return self.original_digest
        if self.offer.is_set() and (self.valid or not self.sent_all.is_set()):
            return self.digest
        return self.original_digest

    def revisions(self):
        m = self.metadata
        return number(1, m["revision"]) + text(2, m["release_id"]) + text(3, m["manifest_digest"])

    def chunk(self, peer, request_id, offset):
        data = self.candidate[offset:offset + CHUNK]
        require(data and len(data) <= CHUNK, "invalid fixture release chunk")
        chunk = (text(1, self.metadata["version"]) + text(2, self.digest) + number(3, len(self.candidate))
                 + number(4, offset) + text(5, data))
        send(peer, text(1, request_id) + text(18, chunk))
        if offset + len(data) == len(self.candidate):
            self.sent_all.set()

    def serve(self):
        try:
            while not self.stopping.is_set():
                try:
                    peer, _ = self.socket.accept()
                except socket.timeout:
                    continue
                self.peer = peer
                with peer:
                    peer.settimeout(5)
                    handshake(peer)
                    auth = receive(peer)
                    require(10 in auth, "control connection did not authenticate")
                    values = fields(auth[10])
                    m = self.metadata
                    require(values[1].decode() == m["node_id"] and values[2].decode() == m["secret"],
                            "authentication changed credentials")
                    restored = (values.get(5) == m["revision"] and values.get(6, b"").decode() == m["release_id"]
                                and values.get(7, b"").decode() == m["manifest_digest"])
                    require(restored or (self.startup_recovery and self.authenticated and not values.get(5)),
                            "node did not restore fixture state before authentication")
                    self.authenticated.append(time.monotonic())
                    # Local state/listeners are restored before Authenticate. Delay
                    # Welcome until both old responses are already in flight, so
                    # startup node logs cannot be ACKed before the held download.
                    deadline = time.monotonic() + TIMEOUT
                    while not self.offer.wait(0.02):
                        if self.stopping.is_set():
                            return
                        require(time.monotonic() < deadline, "old responses were never prepared")
                    welcome = (text(1, m["node_id"]) + number(2, m["revision"])
                               + text(3, m["release_id"]) + text(4, m["manifest_digest"])
                               + number(5, 1) + text(6, self.advertised_digest()) + number(7, 1))
                    send(peer, text(1, auth[1]) + text(11, welcome))
                    if not restored:
                        send(peer, text(1, auth[1]) + text(12, self.desired))
                    try:
                        self.session(peer)
                    except (EOFError, ConnectionResetError, BrokenPipeError):
                        # A successful exec closes the old authenticated transport.
                        # Any unexpected disconnect is exposed by the scenario assertions.
                        pass
        except AssertionError as error:
            self.failures.append(error)
        except (OSError, EOFError) as error:
            if not self.stopping.is_set():
                self.failures.append(error)
        except BaseException as error:
            self.failures.append(error)

    def session(self, peer):
        pending_chunk = None
        pending_logs = []
        expected_offset = 0
        while not self.stopping.is_set():
            if pending_chunk is not None and self.resume.is_set():
                self.resumed_at = time.monotonic()
                self.chunk(peer, *pending_chunk)
                pending_chunk = None
            if self.held.is_set() or self.sent_all.is_set():
                for request_id in pending_logs:
                    send(peer, text(1, request_id) + text(16, b""))
                    self.logs.append(time.monotonic())
                pending_logs.clear()
            if not select.select([peer], [], [], 0.02)[0]:
                continue
            request = receive(peer)
            request_id = request[1]
            if 12 in request:
                self.heartbeats.append(time.monotonic())
                ack = self.revisions() + number(4, 1) + text(5, self.advertised_digest())
                send(peer, text(1, request_id) + text(15, ack))
            elif 16 in request:
                ack = text(1, self.advertised_digest())
                ack += number(2, self.metadata["revision"]) + text(3, self.metadata["release_id"])
                ack += text(4, self.metadata["manifest_digest"])
                send(peer, text(1, request_id) + text(17, ack))
            elif 15 in request:
                # Deliberately acknowledge startup node logs while a release chunk
                # is outstanding, proving WS request-ID multiplexing (not access logs).
                pending_logs.append(request_id)
            elif 17 in request:
                require(self.offer.is_set(), "unsolicited binary download")
                release = fields(request[17])
                offset = release.get(3, 0)
                require(release[2].decode() == self.digest, "wrong requested release SHA")
                require(pending_chunk is None, "parallel release request while chunk is withheld")
                require(offset == expected_offset, "duplicate offset zero or nonsequential release download")
                self.offsets.append(offset)
                expected_offset += min(CHUNK, len(self.candidate) - offset)
                if not self.held.is_set() and (offset > 0 or len(self.candidate) <= CHUNK):
                    self.held_at = time.monotonic()
                    self.held.set()
                    pending_chunk = (request_id, offset)
                else:
                    self.chunk(peer, request_id, offset)
            elif 14 in request:
                result = fields(request[14])
                require(result.get(5) == 1, "rollback node did not apply recovered desired state")
                ack = (text(1, self.metadata["node_id"]) + number(2, self.metadata["revision"])
                       + text(3, self.metadata["release_id"]) + text(4, self.metadata["manifest_digest"])
                       + number(5, 1))
                send(peer, text(1, request_id) + text(19, ack))
            else:
                raise AssertionError("Unexpected control request: " + repr(request.keys()))

    def close(self):
        self.stopping.set()
        self.socket.close()
        if self.peer is not None:
            with contextlib.suppress(OSError):
                self.peer.shutdown(socket.SHUT_RDWR)
        self.thread.join(3)
        require(not self.thread.is_alive(), "control thread did not terminate")


def h1_request(port, path, first=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=TIMEOUT)
    try:
        connection.request("GET", path, headers={"Host": "upgrade.invalid", "Connection": "close"})
        response = connection.getresponse()
        require(response.status == 200, "unexpected H1 status " + str(response.status))
        prefix = response.read(4096) if first else b""
        if first:
            require(prefix == BODY[:4096], "H1 did not deliver initial 4096 bytes")
            first.set()
        body = prefix + response.read()
        require(body == (BODY if path == "/slow-h1" else b"ready"), "H1 response was truncated/corrupted")
    finally:
        connection.close()


def stop_process(process):
    if process is not None and process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(12)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(5)


def run(node_source, fixture_source, valid, failure_mode=None):
    with tempfile.TemporaryDirectory(prefix="flexedge-native-upgrade-") as directory:
        root = pathlib.Path(directory)
        executable = root / "node"
        fixture = root / "node_upgrade_fixture"
        shutil.copy2(node_source, executable)
        shutil.copy2(fixture_source, fixture)
        executable.chmod(0o700)
        fixture.chmod(0o700)
        original = executable.read_bytes()
        original_digest = hashlib.sha256(original).hexdigest()
        # ELF permits trailing data. This changes the SHA without replacing the
        # native implementation or touching either source build artifact.
        candidate = original + b"\nflexedge isolated transition candidate\n" if valid else b"not an executable node\n"
        failures = []
        origin = Origin(failures)
        control = None
        process = h2 = None
        h1_thread = h2_thread = None
        h1_first = threading.Event()
        h1_done = threading.Event()
        h2_first = threading.Event()
        h2_goaway = threading.Event()
        h2_done = threading.Event()
        h2_events = []
        # Reserve both ports while constructing state, then release immediately
        # before spawn. Neither TLS nor an external bind address is configured.
        reservation, tls_reservation = listener(), listener()
        port = reservation.getsockname()[1]
        tls_port = tls_reservation.getsockname()[1]
        log_path = root / "node.log"
        try:
            result = subprocess.run([str(fixture), "state", str(root), str(port), str(tls_port),
                                     str(origin.port)], capture_output=True, text=True, timeout=10, check=True)
            metadata = json.loads(result.stdout)
            credentials = (root / "credentials").read_bytes()
            control = Control(metadata, original_digest, candidate, failures, valid,
                              failure_mode == "startup", (root / "fixture-active.pb").read_bytes())
            reservation.close()
            tls_reservation.close()
            with log_path.open("wb") as node_log:
                process = subprocess.Popen([str(executable), f"ws://127.0.0.1:{control.port}",
                                            str(root / "credentials")], cwd=root,
                                           stdout=node_log, stderr=node_log)
            pid = process.pid
            wait_for(lambda: bool(control.authenticated), failures, "initial authentication")
            h1_request(port, "/probe")

            def slow_h1():
                try:
                    h1_request(port, "/slow-h1", h1_first)
                    h1_done.set()
                except BaseException as error:
                    failures.append(error)

            h1_thread = threading.Thread(target=slow_h1, daemon=True)
            h1_thread.start()
            wait_for(h1_first.is_set, failures, "initial H1 body")
            if valid:
                h2 = subprocess.Popen([str(fixture), "h2", str(port)], cwd=root,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

                def read_h2():
                    try:
                        for line in h2.stdout:
                            event = json.loads(line)
                            h2_events.append((event, time.monotonic()))
                            if event["event"] == "first":
                                h2_first.set()
                            elif event["event"] == "goaway":
                                h2_goaway.set()
                            elif event["event"] == "complete":
                                h2_done.set()
                        require(h2.wait(3) == 0, "native H2 client failed: " + h2.stderr.read())
                    except BaseException as error:
                        failures.append(error)

                h2_thread = threading.Thread(target=read_h2, daemon=True)
                h2_thread.start()
                wait_for(h2_first.is_set, failures, "initial H2 body")
            control.offer.set()
            wait_for(control.held.is_set, failures, "withheld native release chunk")
            # At least three one-second heartbeats must complete while the exact
            # same release request remains unanswered. Keep the full interval bounded.
            wait_for(lambda: len([t for t in control.heartbeats if t >= control.held_at]) >= 3,
                     failures, "three heartbeats during withheld chunk", timeout=7)
            held_heartbeats = [t for t in control.heartbeats if t >= control.held_at]
            require(held_heartbeats[-1] - held_heartbeats[0] >= 1.5,
                    "heartbeats were not separated by the configured one-second cadence")
            require(not h1_done.is_set() and process.poll() is None, "node stopped during download")
            require(control.offsets.count(0) == 1, "duplicate parallel offset-zero download")
            # Startup node logs were queued until Welcome; their ACK must be
            # multiplexed while the same binary-chunk request is still pending.
            require(any(t >= control.held_at for t in control.logs),
                    "no log-delivery ACK flowed while the release chunk was withheld")
            control.resume.set()
            wait_for(control.sent_all.is_set, failures, "complete release transfer")
            if valid:
                wait_for(h2_goaway.is_set, failures, "NO_ERROR GOAWAY during upgrade")
                goaway_at = next(t for event, t in h2_events if event["event"] == "goaway")
                # Connect specifically after stop-accept/GOAWAY and before allowing
                # old responses to finish. The connection waits in the retained
                # kernel backlog; its request is serviced by the new native image.
                with socket.socket() as queued:
                    queued.settimeout(15)
                    started = time.monotonic()
                    try:
                        queued.connect(("127.0.0.1", port))
                    except OSError as error:
                        require(error.errno != errno.ECONNREFUSED, "listener disappeared during exec transition")
                        raise
                    require(time.monotonic() - started < 2, "backlog connection stalled before drain")
                    queued.sendall(b"GET /probe HTTP/1.1\r\nHost: upgrade.invalid\r\nConnection: close\r\n\r\n")
                    if failure_mode:
                        control.withdrawn.set()
                        if failure_mode == "exec":
                            # The check already passed; simulate losing the installed image before exec.
                            executable.write_bytes(b"invalidated after candidate check\n")
                        else:
                            # New startup must retain evidence and roll back; the original build
                            # can then recover this isolated state through the real desired-state path.
                            (root / "state" / "active" / "state.pb").write_bytes(b"invalidated active state\n")
                    # Deliberately release the original bodies 200ms after observing
                    # drain, not before GOAWAY. Both old protocol sessions must survive.
                    time.sleep(max(0, goaway_at + 0.2 - time.monotonic()))
                    origin.release.set()
                    response = http.client.HTTPResponse(queued)
                    response.begin()
                    require(response.status == 200 and response.read() == b"ready",
                            "queued transition connection did not recover service")
                wait_for(h1_done.is_set, failures, "complete old H1 response")
                wait_for(h2_done.is_set, failures, "complete old H2 stream after GOAWAY")
                wait_for(lambda: len(control.authenticated) >= 2, failures, "new image authentication")
                require(len(origin.released_at) == 2 and all(t >= goaway_at + 0.15 for t in origin.released_at),
                        "origin bodies were not delayed into drain")
                h2_thread.join(3)
                require(not h2_thread.is_alive(), "native H2 observer did not finish")
                expected_digest = original_digest if failure_mode else control.digest
                require(hashlib.sha256(executable.read_bytes()).hexdigest() == expected_digest,
                        "installed executable SHA is not the expected image")
                require(hashlib.sha256(pathlib.Path(f"/proc/{pid}/exe").read_bytes()).hexdigest() == expected_digest,
                        "same-PID running image is not the expected image")
                require(not (root / ".node.previous").exists() and not (root / "state" / "pending-upgrade").exists(),
                        "upgrade or rollback left an unconfirmed binary transaction")
            else:
                # Candidate SHA is correct, but exec cannot start it. Observe the
                # actual old data plane, rather than coupling to checker log text.
                end = time.monotonic() + 4
                baseline = len(control.heartbeats)
                while time.monotonic() < end:
                    require(not h1_done.is_set(), "invalid candidate stopped the old slow response")
                    require(process.poll() is None, "invalid candidate exited the node")
                    h1_request(port, "/probe")
                    if failures:
                        raise failures[0]
                    time.sleep(0.1)
                require(len(control.heartbeats) >= baseline + 3, "invalid candidate stopped heartbeats")
                require(hashlib.sha256(executable.read_bytes()).hexdigest() == original_digest,
                        "invalid candidate replaced the original file")
                require(hashlib.sha256(pathlib.Path(f"/proc/{pid}/exe").read_bytes()).hexdigest() == original_digest,
                        "invalid candidate replaced the running native image")
                origin.release.set()
                wait_for(h1_done.is_set, failures, "old response after rejected candidate")
            require(process.pid == pid and process.poll() is None, "upgrade did not preserve the main PID")
            require(control.offsets.count(0) == 1, "release transfer restarted from offset zero")
            require((root / "credentials").read_bytes() == credentials, "upgrade changed installation credentials")
            h1_request(port, "/probe")
            if failures:
                raise failures[0]
            print("native transition: " + (failure_mode + " failure restored old native image and backlog" if failure_mode
                  else "drain, GOAWAY, backlog and same-PID exec passed" if valid
                  else "invalid candidate preserved old binary and data plane"))
        except BaseException:
            if log_path.exists():
                print(log_path.read_text(errors="replace"), file=sys.stderr)
            raise
        finally:
            reservation.close()
            tls_reservation.close()
            origin.stopping.set()
            origin.release.set()
            if h2 is not None:
                stop_process(h2)
            stop_process(process)
            if h1_thread is not None:
                h1_thread.join(3)
            if h2_thread is not None:
                h2_thread.join(3)
            if h2 is not None:
                h2.stdout.close()
                h2.stderr.close()
            if control is not None:
                control.close()
            origin.close()
            for thread in (h1_thread, h2_thread):
                require(thread is None or not thread.is_alive(), "client thread did not terminate")
            if sys.exc_info()[0] is None and failures:
                raise failures[0]


if __name__ == "__main__":
    require(sys.platform == "linux", "native transition acceptance requires Linux /proc")
    require(len(sys.argv) == 3, "usage: node_upgrade_transition.py NODE_BINARY FIXTURE_BINARY")
    node_source, fixture_source = (pathlib.Path(value).resolve(strict=True) for value in sys.argv[1:])
    run(node_source, fixture_source, True)
    run(node_source, fixture_source, False)
    run(node_source, fixture_source, True, "exec")
    run(node_source, fixture_source, True, "startup")
