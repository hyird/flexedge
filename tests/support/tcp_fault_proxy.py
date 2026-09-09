"""Loopback-only TCP bridge for interrupting a real client's network connection."""
import select
import socket
import socketserver
import threading


class TcpFaultProxy:
    def __init__(self, target_port):
        self._lock = threading.Lock()
        self._enabled = True
        self._sockets = set()
        self.connections = 0
        owner = self
        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                upstream = None
                try:
                    with owner._lock:
                        if not owner._enabled:
                            return
                    upstream = socket.create_connection(("127.0.0.1", target_port), timeout=2)
                    self.request.settimeout(2)
                    with owner._lock:
                        if not owner._enabled:
                            return
                        owner._sockets.update((self.request, upstream))
                        owner.connections += 1
                    peers = {self.request: upstream, upstream: self.request}
                    while True:
                        ready, _, _ = select.select(list(peers), [], [], 0.25)
                        for connection in ready:
                            data = connection.recv(65536)
                            if not data:
                                return
                            peers[connection].sendall(data)
                except (OSError, ValueError):
                    pass
                finally:
                    with owner._lock:
                        owner._sockets.discard(self.request)
                        owner._sockets.discard(upstream)
                    if upstream is not None:
                        upstream.close()
        class Server(socketserver.ThreadingTCPServer):
            daemon_threads = True
        self._server = Server(("127.0.0.1", 0), Handler)
        self.port = self._server.server_address[1]
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()

    def cut(self):
        with self._lock:
            self._enabled = False
            connections = list(self._sockets)
        for connection in connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        return len(connections) // 2

    def restore(self):
        with self._lock:
            self._enabled = True

    def close(self):
        self.cut()
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=5)