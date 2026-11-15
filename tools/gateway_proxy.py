"""Local gateway fault injection for clock scenarios; signed response bytes stay unchanged."""

import socket
import socketserver
import threading
import time
import urllib.parse

PROFILES = {"normal", "slow_snapshot", "slow_basemap", "stall_snapshot", "truncate_basemap"}


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class _Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(15)
        request = bytearray()
        try:
            while b"\r\n\r\n" not in request and len(request) < 4096:
                data = self.request.recv(1024)
                if not data:
                    return
                request.extend(data)
            if b"\r\n\r\n" not in request:
                return
            path = request.split(b" ", 2)[1].decode("ascii")
            parsed = urllib.parse.urlsplit(path)
            if parsed.path == "/_network":
                profile = urllib.parse.parse_qs(parsed.query).get("profile", [""])[0]
                if profile in PROFILES:
                    self.server.profile = profile
                    self.request.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n")
                else:
                    self.request.sendall(b"HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                return
            profile = self.server.profile
            with socket.create_connection(self.server.upstream, timeout=10) as upstream:
                upstream.sendall(request)
                response = bytearray()
                while True:
                    data = upstream.recv(4096)
                    if not data:
                        break
                    response.extend(data)
            split = response.find(b"\r\n\r\n") + 4
            shaped = ((profile == "slow_snapshot" or profile == "stall_snapshot")
                      and parsed.path == "/v1/snapshot") or (
                          (profile == "slow_basemap" or profile == "truncate_basemap")
                          and parsed.path == "/v1/basemap")
            if not shaped or split < 4:
                self.request.sendall(response)
                return
            if profile == "stall_snapshot":
                self.request.sendall(response[:split + 1])
                time.sleep(6)
                return
            if profile == "truncate_basemap":
                self.request.sendall(response[:-100])
                return
            time.sleep(0.2)
            # Split the head as well as the body, including CRLF boundaries.
            for offset in range(0, split, 7):
                self.request.sendall(response[offset:min(offset + 7, split)])
                time.sleep(0.005)
            size = 17 if profile == "slow_snapshot" else 128
            for offset in range(split, len(response), size):
                self.request.sendall(response[offset:offset + size])
                time.sleep(0.01)
        except (OSError, ValueError, IndexError):
            # Cancellation and timeout deliberately close a client midway through a reply.
            return


class GatewayProxy:
    def __init__(self, upstream_port):
        self.server = _Server(("127.0.0.1", 0), _Handler)
        self.server.upstream = ("127.0.0.1", upstream_port)
        self.server.profile = "normal"
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)
