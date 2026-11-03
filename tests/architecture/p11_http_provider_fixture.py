#!/usr/bin/env python3
"""P11h real HTTP Provider fixture (loopback TCP server, not a fake port/patch).

The fixture implements the P11 OpenAI-compatible chat-completions boundary as a real
``ThreadingHTTPServer`` so the Model process performs actual HTTP/TLS-free loopback I/O against a
live socket. It records each request and can switch response modes for the P11 acceptance matrix.
Run ``--self-test`` to verify the fixture contract without the P11 managed-plugin chain
(P05b-4 dependency is not merged yet; do not use this self-test as positive-chain evidence).
"""
from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


@dataclass
class ProviderRequest:
    method: str
    path: str
    authorization: str
    body: dict[str, Any]


@dataclass
class ProviderReply:
    status: int
    body: bytes
    content_type: str = 'application/json'
    delay: float = 0.0
    headers: dict[str, str] = field(default_factory=dict)


def chat_completion(model: str = 'provider-model', text: str = 'provider-result') -> dict[str, Any]:
    return {
        'id': 'chatcmpl-p11h',
        'object': 'chat.completion',
        'created': int(time.time()),
        'model': model,
        'choices': [{'index': 0, 'message': {'role': 'assistant', 'content': text},
                     'finish_reason': 'stop'}],
        'usage': {'prompt_tokens': 7, 'completion_tokens': 5, 'total_tokens': 12},
    }


class ProviderFixture:
    """Live loopback HTTP Provider with switchable response modes.

    Modes: success, unauthorized, bad_request, not_found, rate_limited, server_error,
    timeout, non_json, invalid_usage, stream_rejected. ``stream_rejected`` returns 400 unless the
    request explicitly sets ``stream: false``; success mode always records the request values so
    the e2e can assert stream=false/messages/model forwarding.
    """

    def __init__(self, *, mode: str = 'success', timeout_seconds: float = 1.0):
        self.mode = mode
        self.timeout_seconds = timeout_seconds
        self.requests: list[ProviderRequest] = []
        self._server = ThreadingHTTPServer(('127.0.0.1', 0), self._handler())
        self.port = self._server.server_address[1]
        self._thread: threading.Thread | None = None

    @property
    def endpoint(self) -> str:
        return f'http://127.0.0.1:{self.port}/v1/chat/completions'

    def start(self):
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self

    def close(self):
        self._server.shutdown()
        self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=5)

    def _reply(self, request: ProviderRequest) -> ProviderReply:
        body = request.body
        if self.mode == 'success':
            return ProviderReply(200, json.dumps(chat_completion(
                model=str(body.get('model', 'provider-model')))).encode())
        if self.mode == 'stream_rejected' and body.get('stream') is not False:
            return ProviderReply(400, json.dumps({'error': {'code': 'stream_not_allowed'}}).encode())
        if self.mode == 'stream_rejected':
            return ProviderReply(200, json.dumps(chat_completion()).encode())
        if self.mode == 'unauthorized':
            return ProviderReply(401, json.dumps({'error': {'code': 'invalid_api_key'}}).encode())
        if self.mode == 'bad_request':
            return ProviderReply(400, json.dumps({'error': {'code': 'invalid_request'}}).encode())
        if self.mode == 'not_found':
            return ProviderReply(404, json.dumps({'error': {'code': 'not_found'}}).encode())
        if self.mode == 'rate_limited':
            return ProviderReply(429, json.dumps({'error': {'code': 'rate_limited'}}).encode(),
                                 headers={'Retry-After': '1'})
        if self.mode == 'server_error':
            return ProviderReply(500, json.dumps({'error': {'code': 'server_error'}}).encode())
        if self.mode == 'timeout':
            return ProviderReply(200, json.dumps(chat_completion()).encode(),
                                 delay=self.timeout_seconds)
        if self.mode == 'non_json':
            return ProviderReply(200, b'<html>not a completion</html>', 'text/html')
        if self.mode == 'invalid_usage':
            payload = chat_completion()
            payload['usage'] = {'prompt_tokens': -1, 'completion_tokens': 0,
                                'total_tokens': -1}
            return ProviderReply(200, json.dumps(payload).encode())
        raise ValueError(f'unknown provider mode: {self.mode}')

    def _handler(self):
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802 - http.server API
                length = int(self.headers.get('Content-Length', '0') or 0)
                raw = self.rfile.read(length)
                try:
                    body = json.loads(raw or b'{}')
                except json.JSONDecodeError:
                    body = {'invalid_json': True}
                request = ProviderRequest('POST', self.path,
                                          self.headers.get('Authorization', ''), body)
                fixture.requests.append(request)
                reply = fixture._reply(request)
                if reply.delay:
                    time.sleep(reply.delay)
                self.send_response(reply.status)
                self.send_header('Content-Type', reply.content_type)
                self.send_header('Content-Length', str(len(reply.body)))
                for key, value in reply.headers.items():
                    self.send_header(key, value)
                self.end_headers()
                self.wfile.write(reply.body)

            def log_message(self, *_args):
                return

        return Handler


def self_test():
    import urllib.error
    import urllib.request

    fixture = ProviderFixture().start()
    try:
        def call(mode):
            fixture.mode = mode
            request = urllib.request.Request(
                fixture.endpoint, data=json.dumps({
                    'model': 'provider-model', 'stream': False,
                    'messages': [{'role': 'user', 'content': 'hello'}]}).encode(),
                headers={'Authorization': 'Bearer provider-secret',
                         'Content-Type': 'application/json'}, method='POST')
            try:
                with urllib.request.urlopen(request, timeout=3) as response:
                    return response.status, response.read(), response.headers
            except urllib.error.HTTPError as error:
                return error.code, error.read(), error.headers

        status, body, _ = call('success')
        assert status == 200 and json.loads(body)['usage']['total_tokens'] == 12
        status, body, _ = call('unauthorized')
        assert status == 401
        status, body, _ = call('rate_limited')
        assert status == 429 and body
        status, body, _ = call('server_error')
        assert status == 500
        status, body, _ = call('non_json')
        assert status == 200 and b'<html>' in body
        status, body, _ = call('invalid_usage')
        assert status == 200 and json.loads(body)['usage']['prompt_tokens'] == -1
        status, body, _ = call('stream_rejected')
        assert status == 200
        fixture.mode = 'stream_rejected'
        request = urllib.request.Request(
            fixture.endpoint, data=json.dumps({'model': 'm', 'stream': True}).encode(),
            headers={'Authorization': 'Bearer provider-secret'}, method='POST')
        try:
            urllib.request.urlopen(request, timeout=3)
            raise AssertionError('stream=true was accepted')
        except urllib.error.HTTPError as error:
            assert error.code == 400
        assert fixture.requests and all(item.method == 'POST' for item in fixture.requests)
        assert fixture.requests[0].body['stream'] is False
        assert fixture.requests[0].body['messages'][0]['content'] == 'hello'
        assert fixture.requests[0].authorization == 'Bearer provider-secret'
    finally:
        fixture.close()
    print('P11 HTTP Provider fixture self-test passed')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        return 0
    fixture = ProviderFixture().start()
    try:
        print(f'provider_endpoint={fixture.endpoint}', flush=True)
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        return 0
    finally:
        fixture.close()


if __name__ == '__main__':
    raise SystemExit(main())
