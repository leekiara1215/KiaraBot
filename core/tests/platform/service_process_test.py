"""Five-process real socket/TLS checks, without any database or external service."""
import http.client
import json
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

from service_config_test import certificates


def main(*arguments):
    names = ["identity", "runtime", "host", "scheduler", "model"]
    if len(arguments) == 2:
        directory, rpc_client = arguments
        binaries = {name: str(Path(directory) / f"p02-{name}") for name in names}
    elif len(arguments) == 6:
        binaries = dict(zip(names, arguments[:5]))
        rpc_client = arguments[5]
    else:
        raise ValueError("expected five service executable paths and one RPC client path")
    children = []
    with tempfile.TemporaryDirectory(prefix="p02-process-") as directory:
        root = Path(directory)
        base = certificates(root, "ec")

        def config_for(name, probe=0, grpc=0):
            value = {**base, "service": "plugin-host" if name == "host" else name,
                     "probe_listen": f"127.0.0.1:{probe}", "grpc_listen": f"127.0.0.1:{grpc}"}
            path = root / f"{name}-{probe}-{grpc}.json"
            path.write_text(json.dumps(value))
            return path

        def start(name, path):
            child = subprocess.Popen([binaries[name], "--config", str(path)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            children.append(child)
            assert select.select([child.stdout], [], [], 10)[0], "startup timed out"
            line = child.stdout.readline()
            assert "ready=false" in line, (line, child.poll(), child.stderr.read() if child.poll() is not None else "")
            return child, dict(item.split("=", 1) for item in line.strip().split())

        def probe(port, path, expected, body=None, method="GET"):
            connection = http.client.HTTPConnection("127.0.0.1", int(port), timeout=2)
            try:
                connection.request(method, path)
                response = connection.getresponse()
                payload = json.loads(response.read())
                assert response.status == expected, (response.status, payload)
                if body is not None:
                    assert payload == {"state": body}, payload
                if expected == 400:
                    assert payload.get("code") == "INVALID_ARGUMENT", payload
                    assert isinstance(payload.get("message"), str) and payload["message"], payload
                    assert isinstance(payload.get("request_id"), str) and payload["request_id"], payload
                    assert set(payload) == {"code", "message", "request_id"}, payload
            finally:
                connection.close()

        try:
            running = []
            for name in ["identity", "runtime", "host", "scheduler", "model"]:
                binary = binaries[name]
                assert subprocess.run([binary, "--help"], capture_output=True).returncode == 0
                assert subprocess.run([binary], capture_output=True).returncode == 2
                assert subprocess.run([binary, "--unknown"], capture_output=True).returncode == 2
                child, state = start(name, config_for(name))
                running.append((name, child, state))
                probe(state["probe_port"], "/health/live", 200, "HEALTHY")
                probe(state["probe_port"], "/health/ready", 503, "UNHEALTHY")
                probe(state["probe_port"], "/missing", 404)
                for path in ["/health/live", "/health/ready"]:
                    for method in ["POST", "PUT", "OPTIONS"]:
                        probe(state["probe_port"], path, 400, method=method)
                for mode in ["authenticated", "no-client-certificate"]:
                    subprocess.run([rpc_client, name, f'127.0.0.1:{state["grpc_port"]}',
                                    str(root / "ca.pem"), str(root / "server.pem"),
                                    str(root / "server.key"), mode], check=True, timeout=5,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            runtime = running[1][2]
            for bridge in ["adapter-bridge", "plugin-bridge", "host-bridge"]:
                subprocess.run([rpc_client, bridge, f'127.0.0.1:{runtime["grpc_port"]}',
                                str(root / "ca.pem"), str(root / "server.pem"),
                                str(root / "server.key"), "authenticated"], check=True, timeout=5)
            first = running[0][2]
            for probe_port, grpc_port in [(int(first["probe_port"]), 0), (0, int(first["grpc_port"]))]:
                collision = subprocess.run([binaries["identity"], "--config",
                                           str(config_for("identity", probe_port, grpc_port))],
                                          capture_output=True, text=True, timeout=5)
                assert collision.returncode == 1, collision
            # An incomplete HTTP request cannot hold signal shutdown indefinitely.
            slow = socket.create_connection(("127.0.0.1", int(first["probe_port"])), timeout=2)
            slow.sendall(b"GET /health/live HTTP/1.1\r\n")
            started = time.monotonic()
            running[0][1].send_signal(signal.SIGTERM)
            assert running[0][1].wait(timeout=5) == 0
            assert time.monotonic() - started < 5
            slow.close()
            for _, child, state in running[1:]:
                assert child.poll() is None, "one service exit must not stop another"
                probe(state["probe_port"], "/health/live", 200, "HEALTHY")
                child.send_signal(signal.SIGINT)
                assert child.wait(timeout=5) == 0
            print("Five-service probes, mTLS, UNIMPLEMENTED, collisions and bounded shutdown passed")
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.communicate(timeout=5)


if __name__ == "__main__":
    main(*sys.argv[1:])
