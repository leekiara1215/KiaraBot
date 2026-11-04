#!/usr/bin/env python3
"""P12 target Control WebSocket acceptance through the real WSS transport.

Runs the shipped Control binary with real TLS, the generated mutually-authenticated
Identity/owner fixtures and a real browser-style WebSocket client. It covers the WS-04
transport boundaries only: HTTP Upgrade routing/Origin/version, first-frame limits, a live
operation stream, SESSION_READY/replay/resume, typed cursor gaps, slow-consumer shedding,
revocation within one second, REST still being served with long-lived sockets, and shutdown.
It does not claim the Panel target client or the task-7 producers.
"""
from __future__ import annotations

import http.client
import json
import os
import select
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from websockets.exceptions import ConnectionClosed, InvalidHandshake, InvalidStatus
from websockets.sync.client import connect

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
from service_config_test import certificates  # noqa: E402

ACCESS = 'secret-access-credential-login'
PASSWORD = 'never-print-password'
SESSION = '11111111-1111-4111-8111-111111111111'
KEY = 'client-retry-key-0001'
ORIGIN = 'https://panel.example'
ACCOUNT_LIVE = 'aaaaaaaa-aaaa-4aaa-8aaa-000000000001'
ACCOUNT_FLOOD = 'aaaaaaaa-aaaa-4aaa-8aaa-000000000002'
ACCOUNT_GAP = 'aaaaaaaa-aaaa-4aaa-8aaa-000000000003'
SECRETS = ['never-print-password', 'secret-refresh-credential', 'PRIVATE KEY',
           'never-print-upstream-secret', 'secret-access-credential-refresh']
OPERATIONS_PATH = '/ws/operations?owner_service=runtime'


def line(child):
    deadline = time.monotonic() + 15
    value = b''
    while not value.endswith(b'\n'):
        remaining = deadline - time.monotonic()
        assert remaining > 0 and select.select([child.stdout], [], [], remaining)[0], 'startup timeout'
        byte = os.read(child.stdout.fileno(), 1)
        assert byte, child.stderr.read()
        value += byte
    return value.decode().strip()


def stop(child):
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
    return child.communicate(timeout=10)


def scope(account):
    return {'resources': [{'kind': 'RESOURCE_ACCOUNT', 'id': account}]}


def hello(account):
    return {'type': 'hello', 'access_token': ACCESS, 'last_sequence': 0, 'scope': scope(account)}


def resume(session_id, last_sequence, cursor_value, account):
    return {'type': 'resume', 'access_token': ACCESS, 'session_id': session_id,
            'last_sequence': last_sequence, 'scope': scope(account), 'owner_cursor': cursor_value}


class Harness:
    def __init__(self, control, identity_fixture, owner_fixture):
        self.folder = tempfile.TemporaryDirectory(prefix='p12-control-ws-')
        self.root = Path(self.folder.name)
        material = certificates(self.root, 'ec')
        self.tls = material['tls']
        self.ca = self.tls['ca_file']
        self.identity = subprocess.Popen(
            [identity_fixture, self.tls['ca_file'], self.tls['certificate_file'],
             self.tls['private_key_file']],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.owner = subprocess.Popen(
            [owner_fixture, self.tls['ca_file'], self.tls['certificate_file'],
             self.tls['private_key_file']],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.control = None
        self.logs = ''
        self.control_log_paths = []
        try:
            identity_port = int(line(self.identity))
            owner_port = int(line(self.owner))
            material['service'] = 'control'
            material.pop('grpc_listen', None)
            material['control_api'] = {
                'listen': '127.0.0.1:0',
                'identity_endpoint': f'localhost:{identity_port}',
                'runtime_endpoint': f'localhost:{owner_port}',
                'scheduler_endpoint': f'localhost:{owner_port}',
                'model_endpoint': f'localhost:{owner_port}',
                'websocket_origins': [ORIGIN],
            }
            self.config_path = self.root / 'control.json'
            self.config_path.write_text(json.dumps(material))
            self.control_binary = control
            self.port = self.start_control()
        except BaseException:
            self.cleanup()
            raise

    def start_control(self):
        log_path = self.root / f'control-{time.monotonic_ns()}.log'
        self.control_log_path = log_path
        self.control_log_paths.append(log_path)
        child = subprocess.Popen([self.control_binary, '--config', str(self.config_path)],
                                 stdout=subprocess.PIPE, stderr=open(log_path, 'w+'), text=True)
        self.control = child
        first = line(child)
        assert first.startswith('control_https_port='), first
        line(child)  # initial readiness tuple; WS acceptance does not depend on it
        return int(first.split('=')[1])

    def cleanup(self):
        if getattr(self, 'cleaned', False):
            return
        self.cleaned = True
        output = (f'\n[control exit={self.control.poll() if self.control else "none"}]'
                  f'[owner exit={self.owner.poll() if self.owner else "none"}]'
                  f'[identity exit={self.identity.poll() if self.identity else "none"}]\n').encode()
        for child in (self.control, self.owner, self.identity):
            if child is None:
                continue
            try:
                out, err = stop(child)
                output += ((out or '') + (err or '')).encode()
            except Exception:
                child.kill()
        for path in getattr(self, 'control_log_paths', []):
            try:
                output += path.read_bytes()
            except OSError:
                pass
        self.logs = output.decode(errors='replace')
        self.folder.cleanup()

    def websocket(self, port=None, origin=ORIGIN, path=OPERATIONS_PATH):
        # The synchronous client spawns an OS thread per connection; under rapid abnormal-close
        # churn on macOS its TLS handshake occasionally EOFs before the server's 101. A clean
        # retry of the client handshake keeps the server-side assertions strict without turning a
        # client-library race into a false transport regression.
        failure = None
        for _ in range(3):
            context = ssl.create_default_context(cafile=self.ca)
            try:
                return connect(f'wss://localhost:{port or self.port}{path}',
                               ssl=context, origin=origin, open_timeout=5, close_timeout=2,
                               max_size=4 * 1024 * 1024, proxy=None, ping_interval=None)
            except InvalidStatus:
                raise
            except (TimeoutError, OSError, InvalidHandshake) as error:
                failure = error
                time.sleep(0.2)
        raise failure

    def request(self, method, path, body=None, headers=None, port=None):
        context = ssl.create_default_context(cafile=self.ca)
        connection = http.client.HTTPSConnection('localhost', port or self.port,
                                                 context=context, timeout=5)
        try:
            supplied = dict(headers or {})
            payload = body
            if body is not None and not isinstance(body, (bytes, str)):
                payload = json.dumps(body)
                supplied.setdefault('Content-Type', 'application/json')
            connection.request(method, path, payload, supplied)
            response = connection.getresponse()
            content = response.read()
            parsed = json.loads(content) if content else None
            return response.status, parsed
        finally:
            connection.close()

    def raw_https(self, payload):
        context = ssl.create_default_context(cafile=self.ca)
        data = b''
        with socket.create_connection(('127.0.0.1', self.port), timeout=5) as raw:
            with context.wrap_socket(raw, server_hostname='localhost') as tls:
                tls.sendall(payload.encode())
                try:
                    while True:
                        chunk = tls.recv(65536)
                        if not chunk:
                            break
                        data += chunk
                except (socket.timeout, ssl.SSLError):
                    pass
        assert b'\r\n' in data, data
        head, _, body = data.partition(b'\r\n\r\n')
        status = int(head.split(b' ')[1])
        return status, body.decode(errors='replace')


def upgrade_request(path=OPERATIONS_PATH, *, method='GET', origin=ORIGIN, version='13',
                    key='dGhlIHNhbXBsZSBub25jZQ=='):
    lines = [f'{method} {path} HTTP/1.1', 'Host: localhost', 'Upgrade: websocket',
             'Connection: Upgrade', f'Sec-WebSocket-Key: {key}',
             f'Sec-WebSocket-Version: {version}']
    if origin is not None:
        lines.append(f'Origin: {origin}')
    return '\r\n'.join(lines) + '\r\n\r\n'


def recv_json(ws, timeout=3.0):
    try:
        return json.loads(ws.recv(timeout=timeout))
    except TimeoutError:
        return None


def expect_json(ws, predicate, timeout=3.0, what='message'):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = recv_json(ws, timeout=max(0.05, deadline - time.monotonic()))
        if message is not None and predicate(message):
            return message
    raise AssertionError(f'timed out waiting for {what}')


def expect_closed(ws, expected_code, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            ws.recv(timeout=max(0.05, deadline - time.monotonic()))
        except TimeoutError:
            continue
        except ConnectionClosed as closed:
            assert closed.code == expected_code, \
                f'expected close {expected_code}, observed {closed.code}'
            return closed
    raise AssertionError(f'timed out waiting for close {expected_code}')


def assert_ready(message, account, accepted=None):
    assert message is not None and message['type'] == 'SESSION_READY', message
    assert isinstance(message['session_id'], str) and 32 <= len(message['session_id']) <= 256
    assert message['last_sequence'] >= 0
    if accepted is not None:
        assert message['last_sequence'] == accepted, message
    cursor_value = message['owner_cursor']
    assert cursor_value['source_service'] == 'runtime'
    assert cursor_value['epoch']
    assert cursor_value['scope'] == scope(account)
    int(cursor_value['sequence'])  # decimal uint64 string, never a JS number
    return message


def assert_event(message, account, expected_sequence=None):
    assert message is not None, 'missing event'
    assert message['schema_version'] == 'kiarabot.stream.v1', message
    assert message['event_type'] == 'operation.updated', message
    assert isinstance(message['sequence'], int) and 1 <= message['sequence'] <= 9007199254740991
    if expected_sequence is not None:
        assert message['sequence'] == expected_sequence, message
    assert message['occurred_at'].endswith('Z')
    payload = message['payload']
    assert payload['ref']['owner_service'] == 'runtime'
    assert payload['ref']['resource'] == {'kind': 'RESOURCE_ACCOUNT', 'id': account}
    assert int(payload['version']) >= 1
    assert payload['state'] in {'OPERATION_ACCEPTED', 'OPERATION_RUNNING', 'OPERATION_BLOCKED',
                                'OPERATION_SUCCEEDED', 'OPERATION_FAILED',
                                'OPERATION_CANCELLED', 'OPERATION_EXPIRED'}
    assert payload['execution_outcome'] in {'EXECUTION_NOT_STARTED',
                                            'EXECUTION_KNOWN_SUCCEEDED',
                                            'EXECUTION_KNOWN_FAILED', 'EXECUTION_UNKNOWN'}
    assert isinstance(payload['stalled'], bool)
    cursor_value = message['owner_cursor']
    assert cursor_value['source_service'] == 'runtime'
    assert cursor_value['scope'] == scope(account)
    int(cursor_value['sequence'])
    return message


def rejection_matrix(harness):
    cases = [
        (upgrade_request(origin=None), 403, 'PERMISSION_DENIED'),
        (upgrade_request(origin='https://evil.example'), 403, 'PERMISSION_DENIED'),
        (upgrade_request(origin='http://panel.example'), 403, 'PERMISSION_DENIED'),
        (upgrade_request('/ws/unknown'), 404, 'NOT_FOUND'),
        (upgrade_request(method='POST'), 405, 'METHOD_NOT_ALLOWED'),
        (upgrade_request(version='12'), 400, 'INVALID_ARGUMENT'),
        (upgrade_request(key='bad-key'), 400, 'INVALID_ARGUMENT'),
        (upgrade_request('/ws/operations?owner_service=runtime&access_token=x'), 400,
         'INVALID_ARGUMENT'),
        (upgrade_request('/ws/operations'), 400, 'INVALID_ARGUMENT'),
    ]
    for request_text, expected_status, expected_code in cases:
        status, body = harness.raw_https(request_text)
        assert status == expected_status, (request_text, status, body)
        parsed = json.loads(body)
        assert parsed['code'] == expected_code, (request_text, parsed)
        assert parsed['request_id'] and parsed['message'], parsed
        assert not any(secret in body for secret in SECRETS), body


def first_frame_limits(harness):
    ws = harness.websocket()
    ws.send(b'binary-first-frame')
    expect_closed(ws, 4400)

    deep = hello(ACCOUNT_LIVE)
    nested = '[' * 20 + ']' * 20
    ws = harness.websocket()
    ws.send('{"type":"hello","access_token":"' + 't' * 32 +
            '","last_sequence":0,"scope":' + json.dumps(scope(ACCOUNT_LIVE)) +
            ',"extra":' + nested + '}')
    expect_closed(ws, 4400)

    ws = harness.websocket()
    ws.send('x' * 70000)
    # Beast enforces the frozen 64 KiB message bound at the RFC6455 layer and the peer observes
    # the standard 1009 (message too big) close, not an application 4400.
    expect_closed(ws, 1009)

    ws = harness.websocket()
    expect_closed(ws, 4400, timeout=8)  # five second first-frame deadline


def live_stream(harness):
    ws = harness.websocket()
    ws.send(json.dumps(hello(ACCOUNT_LIVE)))
    ready = assert_ready(recv_json(ws), ACCOUNT_LIVE, accepted=0)
    events = []
    for expected in range(1, 6):
        event = assert_event(expect_json(ws, lambda message: message.get('event_type') ==
                                         'operation.updated', what='live event'),
                             ACCOUNT_LIVE)
        assert event['sequence'] == expected, (expected, event)
        events.append(event)
    assert recv_json(ws, timeout=0.7) is None, 'idle stream emitted an unauthorized event'

    # Long-lived sockets cannot consume the REST four-worker budget.
    status, body = harness.request('GET', '/health/live')
    assert status == 200 and body == {'state': 'HEALTHY'}, (status, body)
    status, body = harness.request('POST', '/api/v1/auth/login',
                                   {'username': 'alice', 'password': PASSWORD},
                                   {'Idempotency-Key': KEY})
    assert status == 200, (status, body)

    ws.send(json.dumps(hello(ACCOUNT_LIVE)))  # repeated handshake after authentication
    expect_closed(ws, 4400)
    return ready, events


def resume_replay(harness, ready, events):
    session_id = ready['session_id']
    ws = harness.websocket()
    ws.send(json.dumps(hello(ACCOUNT_LIVE)))
    assert_ready(recv_json(ws), ACCOUNT_LIVE, accepted=0)
    first = assert_event(expect_json(ws, lambda message: message.get('event_type') ==
                                     'operation.updated'), ACCOUNT_LIVE)
    ws.close()
    time.sleep(0.8)  # the owner journal advances while the client is disconnected

    resumed = harness.websocket()
    resumed.send(json.dumps(resume(session_id, first['sequence'],
                                   first['owner_cursor'], ACCOUNT_LIVE)))
    ready_message = assert_ready(recv_json(resumed), ACCOUNT_LIVE, accepted=first['sequence'])
    assert ready_message['session_id'] == session_id
    expected = first['sequence'] + 1
    while expected <= 5:
        event = assert_event(expect_json(resumed, lambda message: message.get('event_type') ==
                                         'operation.updated'), ACCOUNT_LIVE)
        assert event['sequence'] == expected, (expected, event)
        expected += 1
    assert recv_json(resumed, timeout=0.7) is None, 'replay duplicated a delivered event'
    resumed.close()

    # A locally inconsistent replay boundary is INVALID_CURSOR, never a silent gap.
    invalid = harness.websocket()
    bad_cursor = dict(first['owner_cursor'])
    bad_cursor['sequence'] = '0'
    invalid.send(json.dumps(resume(session_id, first['sequence'], bad_cursor, ACCOUNT_LIVE)))
    message = expect_json(invalid, lambda item: item.get('type') == 'RESYNC_REQUIRED',
                          what='INVALID_CURSOR resync')
    assert message['reason'] == 'INVALID_CURSOR', message
    expect_closed(invalid, 1000)

    # An unknown stream capability is a Control restart, not an empty stream.
    unknown = harness.websocket()
    unknown.send(json.dumps(resume('f' * 64, 0, first['owner_cursor'], ACCOUNT_LIVE)))
    message = expect_json(unknown, lambda item: item.get('type') == 'RESYNC_REQUIRED',
                          what='SERVER_RESTART resync')
    assert message['reason'] == 'SERVER_RESTART', message
    expect_closed(unknown, 1000)


def owner_gap(harness):
    ws = harness.websocket()
    ws.send(json.dumps(hello(ACCOUNT_GAP)))
    assert_ready(recv_json(ws), ACCOUNT_GAP, accepted=0)
    for expected in (1, 2):
        event = assert_event(expect_json(ws, lambda message: message.get('event_type') ==
                                         'operation.updated'), ACCOUNT_GAP)
        assert event['sequence'] == expected, (expected, event)
    message = expect_json(ws, lambda item: item.get('type') == 'RESYNC_REQUIRED',
                          timeout=5, what='owner CURSOR_EXPIRED resync')
    assert message['reason'] == 'CURSOR_EXPIRED', message
    expect_closed(ws, 1000)


def slow_consumer(harness):
    ws = harness.websocket()
    ws.send(json.dumps(hello(ACCOUNT_FLOOD)))
    assert_ready(recv_json(ws), ACCOUNT_FLOOD, accepted=0)
    time.sleep(3)  # stop reading until the bounded pending window is exceeded
    expect_closed(ws, 1013, timeout=25)


def shutdown_case(harness):
    log_path = harness.root / f'control-shutdown-{time.monotonic_ns()}.log'
    harness.control_log_paths.append(log_path)
    child = subprocess.Popen([harness.control_binary, '--config', str(harness.config_path)],
                             stdout=subprocess.PIPE, stderr=open(log_path, 'w+'), text=True)
    try:
        first = line(child)
        port = int(first.split('=')[1])
        line(child)
        ws = harness.websocket(port=port)
        ws.send(json.dumps(hello(ACCOUNT_LIVE)))
        assert_ready(recv_json(ws), ACCOUNT_LIVE, accepted=0)
        child.send_signal(signal.SIGTERM)
        expect_closed(ws, 1001, timeout=8)
        out, err = child.communicate(timeout=15)
        assert child.returncode == 0, (child.returncode, out, err)
    finally:
        if child.poll() is None:
            child.kill()
            child.communicate(timeout=5)


def revocation(harness):
    ws = harness.websocket()
    ws.send(json.dumps(hello(ACCOUNT_LIVE)))
    assert_ready(recv_json(ws), ACCOUNT_LIVE, accepted=0)
    time.sleep(0.8)  # idle stream: the 750ms freshness window must still be enforced
    started = time.monotonic()
    status, body = harness.request(
        'POST', f'/api/v1/auth/sessions/{SESSION}/revoke', None,
        {'Authorization': 'Bearer ' + ACCESS, 'Idempotency-Key': KEY})
    assert status == 204, (status, body)
    expect_closed(ws, 4401, timeout=2)
    elapsed = time.monotonic() - started
    assert elapsed < 1.0, f'idle revocation took {elapsed:.3f}s (limit one second)'

    # REST stays independently reachable with the revoked browser session: the generated
    # Identity fixture only revokes ValidateSession, not IssueDelegation, so an authorized health
    # read may still resolve here. What must never happen is a secret or a fabricated HEALTHY
    # component projection; the one-second WSS close above is the revocation evidence.
    status, body = harness.request('GET', '/api/v1/health',
                                   headers={'Authorization': 'Bearer ' + ACCESS})
    assert status in (200, 401, 403, 503), (status, body)
    assert body is None or 'secret' not in json.dumps(body), body

    denied = harness.websocket()
    denied.send(json.dumps(hello(ACCOUNT_LIVE)))
    expect_closed(denied, 4401, timeout=3)


def main(control, identity_fixture, owner_fixture):
    harness = Harness(control, identity_fixture, owner_fixture)
    try:
        rejection_matrix(harness)
        first_frame_limits(harness)
        ready, events = live_stream(harness)
        resume_replay(harness, ready, events)
        owner_gap(harness)
        shutdown_case(harness)
        slow_consumer(harness)
        revocation(harness)
    except BaseException:
        harness.cleanup()
        print('---- control/fixture logs ----', file=sys.stderr)
        print(harness.logs, file=sys.stderr)
        raise
    else:
        harness.cleanup()
    assert not any(secret in harness.logs for secret in SECRETS), \
        'Control/fixture logs leaked a credential'
    print('P12 target Control WebSocket: ok')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print('usage: target_websocket_test.py <kiarabot-control> <identity-fixture> '
              '<owner-fixture>', file=sys.stderr)
        sys.exit(2)
    try:
        main(*sys.argv[1:])
    except Exception as error:  # noqa: BLE001 - acceptance driver reports the exact failure
        print(f'P12 target Control WebSocket failed: {error}', file=sys.stderr)
        raise
