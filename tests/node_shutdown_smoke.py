"""Linux process smoke test; synthetic credentials and loopback only."""
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import time


def run(stalled, secure=False):
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="flexedge-shutdown-") as directory:
        credentials = pathlib.Path(directory) / "credentials"
        credentials.write_text("node_id=" + "a" * 32 + "\nsecret=" + "b" * 32 + "\n")
        credentials.chmod(0o600)
        # Reserve a loopback port; optionally accept TCP handshakes without
        # answering WebSocket requests. Neither mode contacts a real service.
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            if stalled:
                reserved.listen(16)
            scheme = "wss" if secure else "ws"
            address = f"{scheme}://127.0.0.1:{reserved.getsockname()[1]}"
            process = subprocess.Popen(
                [executable, address, str(credentials)], cwd=directory,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            peer = None
            try:
                if stalled:
                    reserved.settimeout(5)
                    peer, _ = reserved.accept()
                    peer.settimeout(5)
                    if not peer.recv(4096):
                        raise RuntimeError("connection closed before handshake bytes")
                else:
                    time.sleep(1)
                if process.poll() is not None:
                    raise RuntimeError("node exited before shutdown signal")
                start = time.monotonic()
                process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=15)
                elapsed = time.monotonic() - start
                if elapsed > 3:
                    raise RuntimeError(f"shutdown cancellation took {elapsed:.3f}s")
                if process.returncode != 0 or "shutdown requested" not in stderr:
                    raise RuntimeError(f"exit={process.returncode}\n{stdout}\n{stderr}")
                mode = "TLS handshake" if secure else "WebSocket handshake" if stalled else "connection refused"
                print(f"SIGTERM clean exit in {elapsed:.3f}s ({mode})")
            finally:
                if peer is not None:
                    peer.close()
                if process.poll() is None:
                    process.kill()
                    process.communicate()


if __name__ == "__main__":
    run(False)
    run(True)
    run(True, secure=True)
