import json
import os
from pathlib import Path
import secrets
import select
import subprocess
import threading
import time
import urllib.error
import urllib.request

import pytest
import yaml
from jsonschema import Draft7Validator, FormatChecker
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(os.environ.get("KIARABOT_ACCEPTANCE_BUILD", ROOT / "build/proto"))
SPEC = yaml.safe_load((ROOT / "api/openapi.yaml").read_text())


class Server:
    def __init__(self, directory, mock=False, plugin=False, settings=None):
        self.mock = mock
        self.plugin_token = secrets.token_hex(32) if plugin else None
        self.directory = directory
        self.config = directory / "control.json"
        self.password = secrets.token_urlsafe(24)
        credentials = BUILD / "control/api/kiarabot-control-credentials"
        hashed = subprocess.check_output([credentials], input=(self.password + "\n").encode()).decode().strip()
        self.settings = {"listen": "127.0.0.1:0", "encryption_key": secrets.token_hex(32), "users": [
            {"id": role, "username": role, "password_hash": hashed, "role": role}
            for role in ("admin", "operator", "viewer")], "plugins": [
            {"id": "test.plugin", "name": "Test", "version": "1.0.0", "api_version": "kiarabot.plugin.v1", "runtime": "python-rpc"}],
            "cors_origins": ["http://localhost:5173"]}
        if settings:
            self.settings.update(settings)
        self.stderr_lines = []
        self._stderr_thread = None
        self.config.write_text(json.dumps(self.settings))
        self.config.chmod(0o600)
        self.process = None
        self.start()

    def start(self):
        for attribute in ("base", "endpoint"):
            self.__dict__.pop(attribute, None)
        env = {key: value for key, value in os.environ.items() if not key.startswith("KIARABOT_")}
        env.update(KIARABOT_RUNTIME_DB=str(self.directory / "runtime.db"),
                   KIARABOT_ADAPTER_ID="test.adapter", KIARABOT_ADAPTER_TOKEN=secrets.token_hex(32),
                   KIARABOT_ADAPTER_ACCOUNTS="one,two", KIARABOT_CONTROL_CONFIG=str(self.config))
        if self.plugin_token:
            env.update(KIARABOT_PLUGIN_ID="test.plugin", KIARABOT_PLUGIN_TOKEN=self.plugin_token,
                       KIARABOT_PLUGIN_PERMISSIONS="storage.namespace")
        command = [BUILD / "control/api/kiarabot-control-mock"] if self.mock else [BUILD / "core/kiarabot-core", "--grpc-listen", "127.0.0.1:0"]
        self.process = subprocess.Popen(command,
                                        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.stderr_lines = []
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()
        deadline = time.monotonic() + 10
        collected = b""
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self._stderr_thread.join(timeout=2)
                raise AssertionError("Core startup failed: " + self.stderr_text())
            if select.select([self.process.stdout], [], [], 0.1)[0]:
                collected += os.read(self.process.stdout.fileno(), 4096)
                for line in collected.decode().splitlines():
                    if line.startswith("operation=control_start port="):
                        self.port = int(line.split("port=")[1])
                        self.base = f"http://127.0.0.1:{self.port}/api/v1"
                    if line.startswith("transport_listening=true port="):
                        self.endpoint = "127.0.0.1:" + line.split("port=")[1].split()[0]
                if hasattr(self, "base") and (not self.plugin_token or hasattr(self, "endpoint")):
                    return
        self.stop()
        raise AssertionError("Core startup timed out")

    def _drain_stderr(self):
        for line in self.process.stderr:
            self.stderr_lines.append(line.decode(errors="replace"))

    def stderr_text(self):
        return "".join(self.stderr_lines)

    def wait_log(self, predicate, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in list(self.stderr_lines):
                if predicate(line):
                    return line
            time.sleep(0.02)
        raise AssertionError("expected log line not observed; stderr=" + self.stderr_text()[-2000:])

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("Core did not stop within deadline")
            if self._stderr_thread:
                self._stderr_thread.join(timeout=2)
            assert self.process.returncode == 0, self.stderr_text()

    def request(self, path, method="GET", body=None, token=None, key=None, headers=None):
        request_headers = {"Accept": "application/json", **(headers or {})}
        if token:
            request_headers["Authorization"] = f"Bearer {token}"
        if method != "GET":
            request_headers["Idempotency-Key"] = key or secrets.token_hex(16)
        if body is not None:
            request_headers["Content-Type"] = "application/json"
        request = urllib.request.Request(self.base + path, method=method, headers=request_headers,
                                         data=json.dumps(body).encode() if body is not None else None)
        try:
            response = urllib.request.urlopen(request, timeout=6)
        except urllib.error.HTTPError as error:
            response = error
        raw = response.read()
        result = json.loads(raw) if raw else None
        return response.status, result, response.headers

    def login(self, role="admin", key=None):
        status, value, _ = self.request("/auth/login", "POST", {"username": role, "password": self.password}, key=key)
        assert status == 200, value
        validate(value, "TokenPair")
        return value


def validate(value, schema):
    document = {"$schema": "http://json-schema.org/draft-07/schema#", **SPEC}
    registry = Registry().with_resource("urn:kiarabot:openapi", Resource.from_contents(document))
    Draft7Validator({"$ref": "urn:kiarabot:openapi#/components/schemas/" + schema},
                    registry=registry, format_checker=FormatChecker()).validate(value)


@pytest.fixture
def server(tmp_path):
    instance = Server(tmp_path)
    try:
        yield instance
    finally:
        instance.stop()


@pytest.fixture(params=["mock", "real"])
def contract_server(tmp_path, request):
    instance = Server(tmp_path, mock=request.param == "mock")
    try:
        yield instance
    finally:
        instance.stop()
