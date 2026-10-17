"""Authenticated Linux shutdown and shared synthetic control-protocol helpers."""
import base64
import hashlib
import pathlib
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def number(field, value):
    return varint(field << 3) + varint(value)


def text(field, value):
    if isinstance(value, str):
        value = value.encode()
    return varint((field << 3) | 2) + varint(len(value)) + value


def fields(data):
    def integer(offset):
        value = shift = 0
        while True:
            part = data[offset]
            offset += 1
            value |= (part & 127) << shift
            if part < 128:
                return value, offset
            shift += 7

    result = {}
    offset = 0
    while offset < len(data):
        tag, offset = integer(offset)
        wire = tag & 7
        if wire == 0:
            value, offset = integer(offset)
        elif wire == 2:
            size, offset = integer(offset)
            value = data[offset:offset + size]
            offset += size
        elif wire in (1, 5):
            size = 8 if wire == 1 else 4
            value = data[offset:offset + size]
            offset += size
        else:
            raise RuntimeError("Unexpected protobuf wire type")
        result[tag >> 3] = value
    return result


def receive_exact(peer, size):
    result = bytearray()
    while len(result) < size:
        block = peer.recv(size - len(result))
        if not block:
            raise EOFError
        result.extend(block)
    return bytes(result)


def send(peer, payload, opcode=2):
    size = len(payload)
    header = bytes([128 | opcode, size]) if size < 126 else bytes([128 | opcode, 126]) + struct.pack("!H", size)
    peer.sendall(header + payload)


def receive(peer):
    while True:
        first, second = receive_exact(peer, 2)
        assert first & 128 and second & 128
        size = second & 127
        if size == 126:
            size = struct.unpack("!H", receive_exact(peer, 2))[0]
        elif size == 127:
            size = struct.unpack("!Q", receive_exact(peer, 8))[0]
        assert size <= 4 * 1024 * 1024
        mask = receive_exact(peer, 4)
        payload = receive_exact(peer, size)
        payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        opcode = first & 15
        if opcode == 8:
            send(peer, payload, 8)
            raise EOFError
        if opcode == 9:
            send(peer, payload, 10)
        else:
            assert opcode == 2
            return fields(payload)


def handshake(peer):
    request = bytearray()
    while not request.endswith(b"\r\n\r\n"):
        request.extend(receive_exact(peer, 1))
        assert len(request) < 16384
    headers = dict(line.split(":", 1) for line in request.decode().split("\r\n")[1:] if ":" in line)
    key = headers["Sec-WebSocket-Key"].strip()
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    peer.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Accept: " + accept + "\r\nSec-WebSocket-Protocol: flexedge.node.v2\r\n\r\n").encode())


def serve(listener, current_digest, ready, failures):
    node_id = "11111111-1111-4111-8111-111111111111"
    release_id = "22222222-2222-4222-8222-222222222222"
    spec_content = text(1, node_id) + number(2, 2) + number(3, 1) + text(4, "shutdown-test")
    release_content = text(1, "33333333-3333-4333-8333-333333333333") + text(2, release_id) + number(3, 1) + text(4, "shutdown.invalid") + number(7, 6)
    manifest_digest = hashlib.sha256(release_content).hexdigest()
    spec = text(1, hashlib.sha256(spec_content).hexdigest()) + text(2, spec_content)
    release = text(1, manifest_digest) + text(2, release_content)
    digest = current_digest
    try:
        peer, _ = listener.accept()
        with peer:
            peer.settimeout(20)
            handshake(peer)
            authenticate = receive(peer)
            assert 10 in authenticate
            request_id = authenticate[1]
            welcome = text(1, node_id) + number(2, 1) + text(3, release_id) + text(4, manifest_digest) + number(5, 10) + text(6, digest) + number(7, 1)
            send(peer, text(1, request_id) + text(11, welcome))
            send(peer, text(1, request_id) + text(12, text(1, spec) + text(2, release)))
            while True:
                request = receive(peer)
                request_id = request[1]
                if 14 in request:
                    result = fields(request[14])
                    assert result[5] == 1, result.get(8, b"apply failed").decode()
                    ack = text(1, node_id) + number(2, 1) + text(3, release_id) + text(4, manifest_digest) + number(5, 1)
                    send(peer, text(1, request_id) + text(19, ack))
                elif 12 in request:
                    ack = number(1, 1) + text(2, release_id) + text(3, manifest_digest) + number(4, 10) + text(5, digest)
                    send(peer, text(1, request_id) + text(15, ack))
                    ready.set()
                elif 15 in request:
                    send(peer, text(1, request_id) + text(16, b""))
                elif 16 in request:
                    ack = text(1, digest) + number(2, 1) + text(3, release_id) + text(4, manifest_digest)
                    send(peer, text(1, request_id) + text(17, ack))
                else:
                    raise RuntimeError("Unexpected node request")
    except (EOFError, ConnectionResetError, BrokenPipeError):
        pass
    except BaseException as error:
        failures.append(error)
        ready.set()


def run():
    with tempfile.TemporaryDirectory(prefix="flexedge-upgrade-") as directory:
        root = pathlib.Path(directory)
        executable = root / "node"
        shutil.copy2(pathlib.Path(sys.argv[1]).resolve(), executable)
        executable.chmod(0o700)
        credentials = root / "credentials"
        credentials.write_text("node_id=" + "a" * 32 + "\nsecret=" + "b" * 32 + "\n")
        credentials.chmod(0o600)
        ready = threading.Event()
        failures = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(20)
            peer = threading.Thread(target=serve, args=(listener, hashlib.sha256(executable.read_bytes()).hexdigest(), ready, failures), daemon=True)
            peer.start()
            process = subprocess.Popen([str(executable), f"ws://127.0.0.1:{listener.getsockname()[1]}", str(credentials)],
                cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                if not ready.wait(15):
                    raise RuntimeError("Authenticated node did not become ready")
                if failures:
                    raise failures[0]
                started = time.monotonic()
                process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=15)
                elapsed = time.monotonic() - started
                peer.join(2)
                if peer.is_alive():
                    raise RuntimeError("Control peer did not retire after node exit")
                if failures:
                    raise failures[0]
                if process.returncode != 0 or elapsed > 3:
                    raise RuntimeError(f"exit={process.returncode} elapsed={elapsed:.3f}s\n{stdout}\n{stderr}")
                assert credentials.read_text() == "node_id=" + "a" * 32 + "\nsecret=" + "b" * 32 + "\n"
                print(f"Authenticated shutdown completed in {elapsed:.3f}s")
            except BaseException:
                if process.poll() is None:
                    process.kill()
                    stdout, stderr = process.communicate()
                    print(stdout + stderr, file=sys.stderr)
                raise
            finally:
                peer.join(2)


if __name__ == "__main__":
    run()
