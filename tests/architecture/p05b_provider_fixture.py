#!/usr/bin/env python3
"""P05b-4 real local HTTP Provider fixture (OpenAI-compatible chat completions shape).

This is a real HTTP/1.1 server used by the P11 single-chain e2e. It is deliberately a local
fixture, never an external provider: it validates fixture authentication (`Authorization:
Bearer`), insists the Model provider protocol uses `stream=false`, records every request for
assertions, and can emulate the full upstream matrix requested by P05b-4:

  success / success_no_usage / invalid_json / bad_choices / tool_calls / bad_usage
  status_400 / status_401 / status_404 / status_429 (Retry-After) / status_500 / status_503
  timeout / assert_stream_false

Mode is selected per request by writing the mode into `--mode-file`. The optional JSONL state
file records `{mode,path,auth_ok,stream,model,content_length}` for the e2e harness. `--self-test`
runs this matrix in-process (no CTest dependencies) and is also the runnable prep evidence before
P05b-2 lands.

Usage (e2e orchestration):
  python3 p05b_provider_fixture.py --host 127.0.0.1 --port <port> \
      --secret <fixture-secret> --mode-file <path> --state-file <path>
Usage (self-test):
  python3 p05b_provider_fixture.py --self-test
"""
from __future__ import annotations

import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import tempfile
import threading
import time

DEFAULT_SECRET = 'p05b-fixture-secret'
MODES = (
    'success',
    'success_no_usage',
    'invalid_json',
    'bad_choices',
    'tool_calls',
    'bad_usage',
    'status_400',
    'status_401',
    'status_404',
    'status_429',
    'status_500',
    'status_503',
    'timeout',
    'assert_stream_false',
)
SUCCESS_BODY = {
    'id': 'chatcmpl-p05b-fixture',
    'object': 'chat.completion',
    'choices': [{
        'index': 0,
        'message': {'role': 'assistant', 'content': 'p05b-fixture reply'},
        'finish_reason': 'stop',
    }],
    'usage': {'prompt_tokens': 7, 'completion_tokens': 5, 'total_tokens': 12},
}


class FixtureState:
    def __init__(self, mode_file: str | None, state_file: str | None, secret: str):
        self.mode_file = mode_file
        self.state_file = state_file
        self.secret = secret
        self.lock = threading.Lock()
        self.requests: list[dict] = []

    def mode(self) -> str:
        if self.mode_file and os.path.exists(self.mode_file):
            with open(self.mode_file, encoding='utf-8') as handle:
                text = handle.read().strip()
            if text:
                return text
        return 'success'

    def record(self, entry: dict) -> None:
        with self.lock:
            self.requests.append(entry)
            if self.state_file:
                with open(self.state_file, 'a', encoding='utf-8') as handle:
                    handle.write(json.dumps(entry, sort_keys=True) + '\n')

    def count(self) -> int:
        with self.lock:
            return len(self.requests)


def json_bytes(value) -> bytes:
    return json.dumps(value, separators=(',', ':')).encode('utf-8')


class Handler(BaseHTTPRequestHandler):
    server_version = 'p05b-provider-fixture/1.0'
    protocol_version = 'HTTP/1.1'

    @property
    def state(self) -> FixtureState:
        return self.server.fixture_state  # type: ignore[attr-defined]

    def log_message(self, fmt, *args):  # keep stderr quiet; the harness owns diagnostics
        return

    def _send(self, status: int, body: bytes, content_type: str = 'application/json',
              extra: dict[str, str] | None = None) -> None:
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        for name, value in (extra or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 (stdlib handler name)
        if self.path == '/__health':
            self._send(200, json_bytes({'ok': True, 'requests': self.state.count()}))
            return
        self._send(404, json_bytes({'error': 'not_found'}))

    def do_POST(self) -> None:  # noqa: N802 (stdlib handler name)
        mode = self.state.mode()
        length = int(self.headers.get('Content-Length', '0'))
        raw = self.rfile.read(length) if length else b''
        try:
            payload = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            payload = {}
        auth = self.headers.get('Authorization', '')
        auth_ok = auth == f'Bearer {self.state.secret}'
        entry = {
            'mode': mode,
            'path': self.path,
            'auth_ok': auth_ok,
            'stream': payload.get('stream') if isinstance(payload, dict) else None,
            'model': payload.get('model') if isinstance(payload, dict) else None,
            'content_length': length,
        }
        self.state.record(entry)
        if self.path != '/v1/chat/completions':
            self._send(404, json_bytes({'error': 'not_found'}))
            return
        if not auth_ok:
            self._send(401, json_bytes({'error': 'fixture_auth_rejected'}))
            return
        if not isinstance(payload, dict) or not isinstance(payload.get('messages'), list):
            self._send(400, json_bytes({'error': 'fixture_invalid_request'}))
            return
        if mode == 'timeout':
            time.sleep(4.0)
            self._send(200, json_bytes(SUCCESS_BODY))
            return
        if mode == 'assert_stream_false':
            if payload.get('stream') is not False:
                self._send(400, json_bytes({'error': 'stream_must_be_false'}))
                return
            self._send(200, json_bytes(SUCCESS_BODY))
            return
        if mode == 'success':
            self._send(200, json_bytes(SUCCESS_BODY))
            return
        if mode == 'success_no_usage':
            body = dict(SUCCESS_BODY)
            body.pop('usage')
            self._send(200, json_bytes(body))
            return
        if mode == 'invalid_json':
            self._send(200, b'<html>not-json</html>', content_type='text/html')
            return
        if mode == 'bad_choices':
            body = dict(SUCCESS_BODY)
            body['choices'] = []
            self._send(200, json_bytes(body))
            return
        if mode == 'tool_calls':
            body = {
                'id': 'chatcmpl-tool',
                'choices': [{
                    'index': 0,
                    'message': {
                        'role': 'assistant',
                        'content': None,
                        'tool_calls': [{'id': 'call-1', 'type': 'function',
                                        'function': {'name': 'x', 'arguments': '{}'}}],
                    },
                }],
            }
            self._send(200, json_bytes(body))
            return
        if mode == 'bad_usage':
            body = dict(SUCCESS_BODY)
            body['usage'] = {'prompt_tokens': -1, 'completion_tokens': 0}
            self._send(200, json_bytes(body))
            return
        status = {
            'status_400': 400,
            'status_401': 401,
            'status_404': 404,
            'status_429': 429,
            'status_500': 500,
            'status_503': 503,
        }.get(mode)
        if status is not None:
            extra = {'Retry-After': '3'} if status == 429 else None
            self._send(status, json_bytes({'error': f'fixture_{status}'}), extra=extra)
            return
        self._send(500, json_bytes({'error': 'fixture_unknown_mode', 'mode': mode}))


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def start_server(host: str, port: int, state: FixtureState) -> FixtureServer:
    server = FixtureServer((host, port), Handler)
    server.fixture_state = state  # type: ignore[attr-defined]
    return server


def endpoint(host: str, port: int) -> str:
    return f'http://{host}:{port}/v1/chat/completions'


def write_mode(mode_file: str, mode: str) -> None:
    if mode not in MODES:
        raise ValueError(f'unknown provider fixture mode: {mode}')
    with open(mode_file, 'w', encoding='utf-8') as handle:
        handle.write(mode + '\n')


def read_requests(state_file: str) -> list[dict]:
    if not os.path.exists(state_file):
        return []
    entries = []
    with open(state_file, encoding='utf-8') as handle:
        for line in handle:
            line = line.strip()
            if line:
                entries.append(json.loads(line))
    return entries


def _request(host: str, port: int, secret: str, body: dict,
             include_auth: bool = True) -> tuple[int, dict[str, str], bytes]:
    import http.client
    connection = http.client.HTTPConnection(host, port, timeout=10)
    headers = {'Content-Type': 'application/json'}
    if include_auth:
        headers['Authorization'] = f'Bearer {secret}'
    connection.request('POST', '/v1/chat/completions', body=json.dumps(body), headers=headers)
    response = connection.getresponse()
    payload = response.read()
    result_headers = {name.lower(): value for name, value in response.getheaders()}
    connection.close()
    return response.status, result_headers, payload


def self_test() -> int:
    secret = DEFAULT_SECRET
    with tempfile.TemporaryDirectory(prefix='p05b-provider-') as root:
        mode_file = os.path.join(root, 'mode')
        state_file = os.path.join(root, 'requests.jsonl')
        state = FixtureState(mode_file, state_file, secret)
        server = start_server('127.0.0.1', 0, state)
        port = server.server_address[1]
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        request = {'model': 'fixture-model', 'stream': False,
                   'messages': [{'role': 'user', 'content': 'hello'}]}
        checks = 0
        try:
            # Fixture authenticity: the Model must send the configured bearer secret.
            status, _, _ = _request('127.0.0.1', port, secret, request, include_auth=False)
            assert status == 401, f'fixture auth missing expected 401, got {status}'
            checks += 1
            # The real provider contract must send stream=false; fixture asserts this mode.
            write_mode(mode_file, 'assert_stream_false')
            status, _, _ = _request('127.0.0.1', port, secret, request)
            assert status == 200, f'stream=false expected 200, got {status}'
            checks += 1
            streaming = dict(request, stream=True)
            status, _, _ = _request('127.0.0.1', port, secret, streaming)
            assert status == 400, f'stream=true must be rejected, got {status}'
            checks += 1
            # Full upstream mode matrix expected statuses for a valid request.
            expected = {
                'success': 200,
                'success_no_usage': 200,
                'invalid_json': 200,
                'bad_choices': 200,
                'tool_calls': 200,
                'bad_usage': 200,
                'status_400': 400,
                'status_401': 401,
                'status_404': 404,
                'status_429': 429,
                'status_500': 500,
                'status_503': 503,
            }
            for mode, want in expected.items():
                write_mode(mode_file, mode)
                status, headers, body = _request('127.0.0.1', port, secret, request)
                assert status == want, f'{mode}: expected {want}, got {status}'
                if mode == 'status_429':
                    assert headers.get('retry-after') == '3', '429 Retry-After passthrough'
                if mode == 'success':
                    decoded = json.loads(body)
                    assert decoded['usage']['prompt_tokens'] == 7, 'usage details'
                checks += 1
            # timeout mode sleeps longer than the Model provider timeout used by the e2e.
            write_mode(mode_file, 'timeout')
            started = time.monotonic()
            status, _, _ = _request('127.0.0.1', port, secret, request)
            elapsed = time.monotonic() - started
            assert status == 200 and elapsed >= 3.0, 'timeout mode must be slow'
            checks += 1
            recorded = read_requests(state_file)
            assert len(recorded) == checks, f'recorded {len(recorded)} requests, expected {checks}'
            assert all(entry['stream'] is False or entry['stream'] is True
                       for entry in recorded), 'stream recorded'
            assert any(entry['auth_ok'] is False for entry in recorded), 'auth failure recorded'
        finally:
            server.shutdown()
            server.server_close()
    print(f'P05b provider fixture self-test passed: {checks} checks '
          f'({len(MODES)} modes, auth, stream=false, usage, Retry-After)')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--secret', default=os.environ.get('P05B_PROVIDER_SECRET', DEFAULT_SECRET))
    parser.add_argument('--mode-file')
    parser.add_argument('--state-file')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--print-ready', action='store_true',
                        help='print one JSON line with the bound host/port when serving')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    state = FixtureState(args.mode_file, args.state_file, args.secret)
    server = start_server(args.host, args.port, state)
    port = server.server_address[1]
    if args.print_ready:
        print(json.dumps({'host': args.host, 'port': port,
                          'endpoint': endpoint(args.host, port)}), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
