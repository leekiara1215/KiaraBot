"""TLS termination is outside Core; verify a trusted local HTTPS/WSS proxy path."""
import json
import select
import socket
import socketserver
import ssl
import subprocess
import threading
import urllib.request

from websockets.sync.client import connect


def test_https_and_wss_with_verified_local_tls(server, tmp_path):
    certificate = tmp_path / "test-cert.pem"
    key = tmp_path / "test-key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                    "-keyout", str(key), "-out", str(certificate)], check=True, capture_output=True)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(certificate, key)
    trusted = ssl.create_default_context(cafile=certificate)

    class Relay(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.settimeout(3)
            try:
                with tls.wrap_socket(self.request, server_side=True) as downstream:
                    with socket.create_connection(("127.0.0.1", server.port), timeout=3) as upstream:
                        while True:
                            readable, _, _ = select.select([downstream, upstream], [], [], 3)
                            if not readable:
                                return
                            for source in readable:
                                data = source.recv(65536)
                                if not data:
                                    return
                                (upstream if source is downstream else downstream).sendall(data)
            except (OSError, ssl.SSLError):
                return

    class Proxy(socketserver.ThreadingTCPServer):
        allow_reuse_address = True

    with Proxy(("127.0.0.1", 0), Relay) as proxy:
        worker = threading.Thread(target=proxy.serve_forever)
        worker.start()
        try:
            port = proxy.server_address[1]
            request = urllib.request.Request(f"https://127.0.0.1:{port}/api/v1/auth/login", method="POST",
                headers={"Content-Type": "application/json", "Idempotency-Key": "tls-login-idempotency"},
                data=json.dumps({"username": "admin", "password": server.password}).encode())
            with urllib.request.urlopen(request, context=trusted, timeout=4) as response:
                tokens = json.load(response)
            with connect(f"wss://127.0.0.1:{port}/ws/plugins", ssl=trusted, open_timeout=4) as websocket:
                websocket.send(json.dumps({"type": "hello", "access_token": tokens["access_token"], "last_sequence": 0}))
                assert json.loads(websocket.recv(timeout=3))["type"] == "SESSION_READY"
                assert server.request("/plugins/test.plugin/disable", "POST", token=tokens["access_token"])[0] == 202
                event = json.loads(websocket.recv(timeout=3))
                assert event["event_type"] == "plugin.updated" and event["payload"]["state"] == "PLUGIN_STATE_DISABLED"
        finally:
            proxy.shutdown()
            worker.join(timeout=4)
            assert not worker.is_alive()
