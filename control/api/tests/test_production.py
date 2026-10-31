"""Production-readiness behavior: trace propagation, access log, keep-alive, 405/HEAD/CORS,
rate limits, stream quotas, graceful shutdown, body-size boundary and capacity limits."""
import concurrent.futures
import http.client
import json
import secrets
import time
import urllib.error
import urllib.request

import pytest
from websockets.exceptions import ConnectionClosed
from websockets.sync.client import connect

from conftest import Server


def probe(server, path, method="GET", headers=None):
    request = urllib.request.Request(f"http://127.0.0.1:{server.port}{path}", method=method)
    for name, value in (headers or {}).items():
        request.add_header(name, value)
    try:
        response = urllib.request.urlopen(request, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    raw = response.read()
    return response.status, json.loads(raw) if raw else None, response.headers


def open_stream(server, token, channel="control"):
    socket = connect(f"ws://127.0.0.1:{server.port}/ws/{channel}", open_timeout=3)
    socket.send(json.dumps({"type": "hello", "access_token": token, "last_sequence": 0}))
    return socket


def test_inbound_trace_propagation_and_access_log(server):
    request_id = "req-0123456789"
    trace_id = "a" * 32
    status, body, headers = probe(
        server,
        "/health/live",
        headers={
            "X-Request-ID": request_id,
            "traceparent": f"00-{trace_id}-{'b' * 16}-01",
        },
    )
    assert status == 200 and body == {"state": "HEALTHY"}
    assert headers["X-Request-ID"] == request_id
    assert headers["X-Trace-ID"] == trace_id
    token = server.login()["access_token"]
    status, body, headers = server.request(
        "/health", token=token, headers={"X-Request-ID": "req-authenticated-1"})
    assert status == 200
    assert headers["X-Request-ID"] == "req-authenticated-1"
    # The access log records the request id, trace id, path, status and duration as JSON.
    line = server.wait_log(lambda entry: '"request_id":"req-0123456789"' in entry)
    record = json.loads(line)
    assert record["component"] == "control-api"
    assert record["operation"] == "http_request"
    assert record["method"] == "GET"
    assert record["path"] == "/health/live"
    assert record["status"] == 200
    assert record["trace_id"] == trace_id
    assert record["duration_ms"] >= 0
    # An invalid traceparent is ignored instead of being reflected.
    status, _, headers = probe(server, "/health/live", headers={"traceparent": "00-not-a-trace"})
    assert status == 200 and headers["X-Trace-ID"] != "00-not-a-trace"


def test_keep_alive_head_and_method_semantics(server):
    token = server.login()["access_token"]
    connection = http.client.HTTPConnection("127.0.0.1", server.port, timeout=5)
    try:
        connection.request("GET", "/health/live")
        first = connection.getresponse()
        assert first.status == 200
        first.read()
        # The same TCP connection serves a second request without reconnecting.
        connection.request("HEAD", "/api/v1/health", headers={"Authorization": f"Bearer {token}"})
        second = connection.getresponse()
        assert second.status == 200 and second.read() == b""
        connection.request(
            "DELETE", "/api/v1/plugins", headers={"Authorization": f"Bearer {token}"})
        third = connection.getresponse()
        assert third.status == 405
        assert "GET" in third.getheader("Allow")
        third.read()
        connection.request("PUT", "/api/v1/auth/login")
        fourth = connection.getresponse()
        assert fourth.status == 405
        fourth.read()
        connection.request("GET", "/api/v1/not-a-route")
        fifth = connection.getresponse()
        assert fifth.status == 404
        fifth.read()
    finally:
        connection.close()


def test_cors_preflight_and_same_origin(server):
    origin = "http://localhost:5173"
    status, _, headers = probe(
        server, "/api/v1/plugins", "OPTIONS", {"Origin": origin,
                                               "Access-Control-Request-Method": "GET"})
    assert status == 204
    assert headers["Access-Control-Allow-Origin"] == origin
    assert "GET" in headers["Access-Control-Allow-Methods"]
    assert "Authorization" in headers["Access-Control-Allow-Headers"]
    # A disallowed cross-origin caller receives no CORS grant.
    status, _, headers = probe(server, "/api/v1/plugins", "OPTIONS",
                               {"Origin": "http://evil.example"})
    assert status == 204 and headers["Access-Control-Allow-Origin"] is None
    status, _, headers = probe(server, "/health/live", headers={"Origin": origin})
    assert status == 200 and headers["Access-Control-Allow-Origin"] == origin


def test_public_rate_limit(tmp_path):
    server = Server(tmp_path, settings={
        "limits": {"public_requests_per_second": 1, "public_burst": 5}})
    try:
        statuses = [probe(server, "/health/live")[0] for _ in range(12)]
        assert statuses.count(200) == 5
        assert statuses[-1] == 429
        status, body, headers = probe(server, "/health/live")
        assert status == 429 and body["code"] == "RATE_LIMITED"
        assert int(headers["Retry-After"]) >= 1
        assert body["request_id"]
    finally:
        server.stop()


def test_websocket_stream_quota_and_handshake_timeout(server):
    token = server.login()["access_token"]
    sockets = [open_stream(server, token) for _ in range(16)]
    try:
        for socket in sockets:
            assert json.loads(socket.recv(timeout=3))["type"] == "SESSION_READY"
        overflow = open_stream(server, token)
        with pytest.raises(ConnectionClosed) as failure:
            overflow.recv(timeout=3)
        assert failure.value.rcvd.code == 1013
        overflow.close()
    finally:
        for socket in sockets:
            socket.close()
    # A connection that never sends a first frame is closed with the invalid-first-frame code.
    silent = connect(f"ws://127.0.0.1:{server.port}/ws/control", open_timeout=3)
    started = time.monotonic()
    with pytest.raises(ConnectionClosed) as failure:
        silent.recv(timeout=8)
    assert failure.value.rcvd.code == 4400
    assert time.monotonic() - started < 7
    silent.close()


def test_graceful_shutdown_closes_websockets(server):
    token = server.login()["access_token"]
    socket = open_stream(server, token)
    assert json.loads(socket.recv(timeout=3))["type"] == "SESSION_READY"
    server.stop()
    with pytest.raises(ConnectionClosed) as failure:
        socket.recv(timeout=3)
    assert failure.value.rcvd.code == 1001
    assert server.process.returncode == 0
    socket.close()


def test_concurrent_idempotency_key_has_single_effect(server):
    token = server.login()["access_token"]
    version = server.request("/plugins/test.plugin/config", token=token)[1]["version"]
    body = {"expected_version": version, "values": {"enabled": True, "limit": 7}}
    key = secrets.token_hex(16)

    def write(_):
        return server.request("/plugins/test.plugin/config", "PUT", body, token=token, key=key)

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        results = list(executor.map(write, range(8)))
    assert all(status == 200 for status, _, _ in results)
    committed = results[0][1]
    assert all(value == committed for _, value, _ in results)
    assert server.request("/plugins/test.plugin/config", token=token)[1]["version"] == \
        committed["version"]


def test_request_body_limit_boundary(server):
    def login_with_body(size):
        payload = json.dumps({"username": "admin", "password": server.password}).encode()
        assert size >= len(payload)
        body = payload + b" " * (size - len(payload))
        connection = http.client.HTTPConnection("127.0.0.1", server.port, timeout=8)
        try:
            connection.request("POST", "/api/v1/auth/login", body=body, headers={
                "Content-Type": "application/json",
                "Idempotency-Key": secrets.token_hex(16)})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    status, raw = login_with_body(65536)
    assert status == 200 and json.loads(raw)["access_token"]
    status, raw = login_with_body(65537)
    assert status == 400 and json.loads(raw)["code"] == "INVALID_ARGUMENT"


def test_pagination_cursor_capacity_returns_rate_limited(server):
    token = server.login()["access_token"]
    server.login()  # a second session so page_size=1 produces a next token
    headers = {"Authorization": f"Bearer {token}"}
    connection = None
    try:
        for index in range(1025):
            if index % 400 == 0:
                if connection is not None:
                    connection.close()
                connection = http.client.HTTPConnection("127.0.0.1", server.port, timeout=10)
            connection.request("GET", "/api/v1/auth/sessions?page_size=1", headers=headers)
            response = connection.getresponse()
            body = json.loads(response.read())
            if index < 1024:
                assert response.status == 200, body
                assert "next_page_token" in body["page"]
            else:
                assert response.status == 429, body
                assert body["code"] == "RATE_LIMITED"
    finally:
        if connection is not None:
            connection.close()


def test_concurrent_read_load_served(server):
    token = server.login()["access_token"]

    def read(_):
        return server.request("/plugins", token=token)[0]

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        statuses = list(executor.map(read, range(8 * 15)))
    assert statuses.count(200) == len(statuses)
    assert time.monotonic() - started < 10
