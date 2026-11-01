"""Real browser TLS -> Control -> mutually authenticated generated Identity fixture.

This verifies the transport mapping; it does not claim to implement Identity policy/PG.
All certificates, keys and local configuration are temporary and never committed.
"""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import select
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'core/tests/platform'))
from service_config_test import certificates

KEY = 'client-retry-key-0001'
SESSION = '11111111-1111-4111-8111-111111111111'
OTHER = '22222222-2222-4222-8222-222222222222'
ACCESS = 'secret-access-credential-login'


def line(child):
    deadline = time.monotonic() + 10
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
    return child.communicate(timeout=5)


def main(control, fixture):
    with tempfile.TemporaryDirectory(prefix='p04-control-') as folder:
        root = Path(folder)
        config = certificates(root, 'ec')
        tls = config['tls']
        identity = subprocess.Popen([fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        child = None
        secrets = ['never-print-password', 'never-print-upstream-secret', ACCESS,
                   'secret-refresh-credential-login', 'secret-access-credential-refresh',
                   'fixture-proof:', 'PRIVATE KEY']
        logs = ''
        try:
            identity_port = int(line(identity))
            config['service'] = 'control'
            config.pop('grpc_listen')
            config['control_api'] = {'listen': '127.0.0.1:0', 'identity_endpoint': f'localhost:{identity_port}'}
            path = root / 'control.json'
            path.write_text(json.dumps(config))
            child = subprocess.Popen([control, '--config', str(path)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            first = line(child)
            assert first.startswith('control_https_port='), first
            port = int(first.split('=')[1])
            state = dict(item.split('=', 1) for item in line(child).split())
            assert state['ready'] == 'false', state
            context = ssl.create_default_context(cafile=tls['ca_file'])

            def request(method, target, body=None, headers=None, raw=False):
                connection = http.client.HTTPSConnection('localhost', port, context=context, timeout=5)
                supplied = headers if headers is not None else {}
                if body is not None and not raw:
                    body = json.dumps(body)
                try:
                    connection.request(method, target, body, supplied)
                    response = connection.getresponse()
                    content = response.read()
                    assert response.getheader('Cache-Control') == 'no-store'
                    assert response.getheader('Access-Control-Allow-Origin') is None
                    parsed = json.loads(content) if content else None
                    if response.status >= 400:
                        assert set(parsed) >= {'code', 'message', 'request_id'}, parsed
                        assert parsed['request_id']
                        assert all(secret not in content.decode() for secret in secrets)
                    return response.status, parsed, dict(response.getheaders())
                finally:
                    connection.close()

            def login(username='alice', **extra):
                return request('POST', '/api/v1/auth/login', {'username': username, 'password': 'never-print-password', **extra},
                               {'Content-Type': 'application/json', 'Idempotency-Key': KEY})

            status, tokens, _ = login()
            assert status == 200, (status, tokens)
            assert tokens['expires_in'] == 899, tokens
            assert tokens['checked_at'] == '2023-11-14T22:13:20.500Z'
            assert tokens['access_token'] == ACCESS
            # Same caller key is preserved over a repeated/lost-response attempt.
            assert login()[1] == tokens
            assert request('POST', '/api/v1/auth/login', {'username': 'alice', 'password': 'never-print-password'},
                           {'Content-Type': 'Application/JSON; Charset="UTF-8"', 'Idempotency-Key': KEY})[0] == 200
            refreshed = request('POST', '/api/v1/auth/refresh', {'refresh_token': tokens['refresh_token']},
                                {'Content-Type': 'application/json', 'Idempotency-Key': KEY})
            assert refreshed[0] == 200, refreshed
            assert request('POST', '/api/v1/auth/refresh', {'refresh_token': tokens['refresh_token']},
                           {'Content-Type': 'application/json', 'Idempotency-Key': KEY})[1] == refreshed[1]
            auth = {'Authorization': 'Bearer ' + ACCESS}
            current = request('GET', '/api/v1/auth/me', headers=auth)
            assert current[0] == 200, current
            assert request('GET', '/api/v1/auth/me', headers={'Authorization': 'bEaReR ' + ACCESS})[0] == 200
            assert current[1]['authorization_version'] == '18446744073709551615'
            assert current[1]['assignments'][0]['version'] == '18446744073709551615'
            assert current[1]['roles'] == ['viewer']
            for mode in ['disabled', 'revoked', 'expired', 'bad-time', 'future-created', 'bad-id',
                         'bad-scope', 'bad-assignment', 'zero-version']:
                invalid = request('GET', '/api/v1/auth/me', headers={'Authorization': 'Bearer fixture-' + mode})
                assert invalid[0] == 500 and invalid[1]['code'] == 'INTERNAL', (mode, invalid)
            page = request('GET', '/api/v1/auth/sessions?page_size=3', headers=auth)
            assert page[0] == 200, page
            assert [item['state'] for item in page[1]['items']] == ['ACTIVE', 'EXPIRED', 'REVOKED']
            assert page[1]['page'] == {'page_size': 3, 'next_page_token': 'opaque-next-token'}
            page2 = request('GET', '/api/v1/auth/sessions?page_size=3&page_token=opaque-next-token', headers=auth)
            assert page2[0] == 200 and 'next_page_token' not in page2[1]['page'], page2
            assert request('GET', '/api/v1/auth/sessions?page_token=bad', headers=auth)[0] == 400
            for mode in ['missing_page_time', 'bad_snapshot', 'bad_page_size', 'duplicate_session',
                         'foreign_session', 'bad_session_id', 'stale_page', 'missing_checked']:
                invalid = request('GET', '/api/v1/auth/sessions?page_size=3&page_token=' + mode, headers=auth)
                assert invalid[0] == 500 and invalid[1]['code'] == 'INTERNAL', (mode, invalid)
            for username, expected, code in [('denied', 403, 'PERMISSION_DENIED'),
                                             ('limited', 429, 'RATE_LIMITED'),
                                             ('unavailable', 503, 'UPSTREAM_UNAVAILABLE'),
                                             ('no_detail', 500, 'INTERNAL'),
                                             ('no_context', 500, 'INTERNAL'),
                                             ('unknown_context', 500, 'INTERNAL'),
                                             ('bad_status', 500, 'INTERNAL'),
                                             ('missing_time', 500, 'INTERNAL'),
                                             *[(mode, 500, 'INTERNAL') for mode in ['expired_tokens', 'reverse_expiry', 'bad_token_time', 'bad_token_id', 'zero_rotation']]]:
                result = login(username)
                assert result[0] == expected and result[1]['code'] == code, result
                if expected == 429:
                    assert result[2]['Retry-After'] == '1'
            for mode, expected, code in [('plugin_failure', 503, 'PLUGIN_UNAVAILABLE'), ('typed_timeout', 504, 'TIMEOUT')]:
                typed = login(mode)
                assert typed[0] == expected and typed[1]['code'] == code, typed
                facts = typed[1]['failure_context']
                assert facts['acceptance'] == 'ACCEPTANCE_UNKNOWN' and facts['recovery'] == 'QUERY_SAME_INTENT', typed
                assert facts['operation']['resource']['id'] == SESSION and facts['operation']['owner_service'] == 'runtime'
                assert facts['current_version'] == '18446744073709551615' and facts['retry_after_ms'] == 2500
                assert typed[1]['retryable'] is True and typed[1]['trace_id'] == typed[1]['request_id']
            transport = login('unavailable')
            assert 'failure_context' not in transport[1], transport
            started = time.monotonic()
            assert login('slow')[0] == 504
            assert time.monotonic() - started < 3.5
            started = time.monotonic()
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                statuses = list(pool.map(lambda _: login('slow')[0], range(16)))
            assert statuses == [504] * 16, statuses
            assert time.monotonic() - started < 3.5, 'queue wait escaped HTTP budget'
            stalled_connections = [socket.create_connection(('localhost', port), timeout=2) for _ in range(70)]
            try:
                ready = select.select(stalled_connections, [], [], 0.5)[0]
                closed = sum(connection.recv(1) == b'' for connection in ready)
                assert closed >= 6, ('connection cap not enforced', closed)
            finally:
                for connection in stalled_connections:
                    connection.close()
            time.sleep(0.1)
            assert login()[0] == 200
            # Async deadlines close both a stalled handshake and an incomplete HTTP header.
            handshake_stall = socket.create_connection(('localhost', port), timeout=7)
            read_stall = context.wrap_socket(socket.create_connection(('localhost', port), timeout=7), server_hostname='localhost')
            read_stall.sendall(b'GET /api/v1/auth/me HTTP/1.1\r\nHost:')
            started = time.monotonic()
            assert handshake_stall.recv(1) == b''
            assert read_stall.recv(1) == b''
            assert 4 < time.monotonic() - started < 6.5, 'async I/O deadline missing'
            handshake_stall.close()
            read_stall.close()
            assert login(actor='forged')[0] == 400
            for body in ['{"username":"alice","username":"other","password":"x"}',
                         '{"username":true,"password":"x"}', '{invalid',
                         '{"username":[],"password":"x"}',
                         '{"username":' + '[' * 10000 + '0' + ']' * 10000 + ',"password":"x"}', '{"username":"alice","password":"x","context":{}}']:
                assert request('POST', '/api/v1/auth/login', body,
                               {'Content-Type': 'application/json', 'Idempotency-Key': KEY}, raw=True)[0] == 400
            assert request('POST', '/api/v1/auth/login', {'username': 'alice', 'password': 'x'},
                           {'Content-Type': 'application/json'})[0] == 400
            for query in ['page_size=0', 'page_size=101', 'page_size=1.0', 'page_size=1&page_size=2',
                          'page_token=%00', 'page_token=%ZZ', 'actor=x', 'principal_id=forged']:
                assert request('GET', '/api/v1/auth/sessions?' + query, headers=auth)[0] == 400, query
            assert request('GET', '/api/v1/auth/me')[0] == 401
            assert request('GET', '/api/v1/auth/me?actor=forged', headers=auth)[0] == 400
            assert request('GET', '/api/v1/auth/me', headers={'Authorization': 'Bearer dependency-down'})[0] == 503
            assert request('POST', '/api/v1/auth/sessions/not-a-uuid/revoke', headers={**auth, 'Idempotency-Key': KEY})[0] == 400
            result = request('GET', '/api/v1/auth/login')
            assert result[0] == 405 and result[2]['Allow'] == 'POST'
            assert request('GET', '/api/v1/unknown')[0] == 404
            assert request('OPTIONS', '/api/v1/auth/me')[0] == 405
            # Duplicate Authorization must not be silently merged or replaced.
            duplicate = context.wrap_socket(socket.create_connection(('localhost', port), timeout=5), server_hostname='localhost')
            duplicate.sendall(('GET /api/v1/auth/me HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer ' + ACCESS +
                               '\r\nAuthorization: Bearer forged\r\nConnection: close\r\n\r\n').encode())
            assert b'400' in duplicate.recv(8192).split(b'\r\n', 1)[0]
            duplicate.close()
            malformed = context.wrap_socket(socket.create_connection(('localhost', port), timeout=5), server_hostname='localhost')
            malformed.sendall(b'POST /api/v1/auth/login HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\nx')
            assert b'400' in malformed.recv(8192).split(b'\r\n', 1)[0]
            malformed.close()
            # An unfinished chunk-extension line must hit a retained-buffer cap before the I/O deadline.
            chunk_metadata = context.wrap_socket(socket.create_connection(('localhost', port), timeout=3), server_hostname='localhost')
            started = time.monotonic()
            chunk_metadata.sendall(b'POST /api/v1/auth/login HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n1;' + b'x' * (1048576 + 8193))
            assert b'400' in chunk_metadata.recv(8192).split(b'\r\n', 1)[0]
            assert time.monotonic() - started < 3, 'retained framing buffer was unbounded'
            chunk_metadata.close()
            # Header and body limits are enforced while parsing, before reaching Identity.
            oversized = context.wrap_socket(socket.create_connection(('localhost', port), timeout=5), server_hostname='localhost')
            oversized.sendall(b'POST /api/v1/auth/login HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048577\r\n\r\n')
            assert b'400' in oversized.recv(8192).split(b'\r\n', 1)[0]
            oversized.close()
            assert request('GET', '/api/v1/auth/me', headers={**auth, 'X-Large': 'x' * 9000})[0] == 400
            assert request('POST', f'/api/v1/auth/sessions/{OTHER}/revoke', headers={**auth, 'Idempotency-Key': KEY})[0] == 404
            assert request('POST', f'/api/v1/auth/sessions/{SESSION}/revoke', headers={**auth, 'Idempotency-Key': KEY})[0] == 204
            # Fresh ValidateSession on every REST call observes revocation immediately.
            assert request('GET', '/api/v1/auth/me', headers=auth)[0] == 401
            # A separate endpoint must cancel an active upstream RPC during shutdown.
            closing = subprocess.Popen([control, '--config', str(path)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                closing_port = int(line(closing).split('=')[1])
                line(closing)
                def active_rpc():
                    connection = http.client.HTTPSConnection('localhost', closing_port, context=context, timeout=4)
                    try:
                        connection.request('POST', '/api/v1/auth/login',
                                           json.dumps({'username': 'slow', 'password': 'never-print-password'}),
                                           {'Content-Type': 'application/json', 'Idempotency-Key': KEY})
                        response = connection.getresponse()
                        response.read()
                    except (OSError, http.client.HTTPException):
                        pass  # Cancellation closes the client transport as part of endpoint teardown.
                    finally:
                        connection.close()
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    pending = pool.submit(active_rpc)
                    time.sleep(0.15)
                    started = time.monotonic()
                    out, err = stop(closing)
                    assert closing.returncode == 0 and time.monotonic() - started < 1.5, err
                    pending.result(timeout=3)
                    logs += out + err
            finally:
                if closing.poll() is None:
                    stop(closing)
            # Trust failure on the outbound channel is unavailable, never authenticated success.
            rogue_root = root / 'rogue'
            rogue_root.mkdir()
            rogue_tls = certificates(rogue_root, 'ec')['tls']
            rogue_identity = subprocess.Popen([fixture, rogue_tls['ca_file'], rogue_tls['certificate_file'],
                                               rogue_tls['private_key_file']], stdout=subprocess.PIPE,
                                              stderr=subprocess.PIPE, text=True)
            wrong_trust = None
            try:
                rogue_port = int(line(rogue_identity))
                wrong = {**config, 'control_api': {'listen': '127.0.0.1:0', 'identity_endpoint': f'localhost:{rogue_port}'}}
                wrong_path = root / 'wrong-trust.json'
                wrong_path.write_text(json.dumps(wrong))
                wrong_trust = subprocess.Popen([control, '--config', str(wrong_path)],
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                wrong_port = int(line(wrong_trust).split('=')[1])
                line(wrong_trust)
                connection = http.client.HTTPSConnection('localhost', wrong_port, context=context, timeout=4)
                try:
                    connection.request('POST', '/api/v1/auth/login',
                                       json.dumps({'username': 'alice', 'password': 'never-print-password'}),
                                       {'Content-Type': 'application/json', 'Idempotency-Key': KEY})
                    response = connection.getresponse()
                    assert response.status == 503 and json.loads(response.read())['code'] == 'UPSTREAM_UNAVAILABLE'
                finally:
                    connection.close()
                out, err = stop(wrong_trust)
                assert wrong_trust.returncode == 0, err
                logs += out + err
            finally:
                if wrong_trust is not None and wrong_trust.poll() is None:
                    stop(wrong_trust)
                stop(rogue_identity)
            stop(identity)
            assert request('GET', '/api/v1/auth/me', headers=auth)[0] == 503
            # Plain TCP and stalled TLS do not block service shutdown.
            stalled = socket.create_connection(('localhost', port), timeout=2)
            started = time.monotonic()
            out, err = stop(child)
            assert child.returncode == 0, err
            assert time.monotonic() - started < 3, 'unbounded shutdown'
            stalled.close()
            logs += out + err
            assert all(secret not in logs for secret in secrets), logs
            print('P04 HTTPS/mTLS auth mapping, strict input, deadline, revocation, dependency and shutdown passed')
        finally:
            if child is not None and child.poll() is None:
                stop(child)
            if identity.poll() is None:
                stop(identity)


if __name__ == '__main__':
    main(*sys.argv[1:])
