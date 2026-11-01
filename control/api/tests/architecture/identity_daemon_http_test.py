#!/usr/bin/env python3
"""Owned PG + offline bootstrap + production Identity and Control HTTPS regression.

Invoked only by infrastructure/postgres/tests/run_database_test.py with its owned
cluster and compiled initializer/bootstrap/daemon binaries. No external DSN accepted.
All material is temporary and removed by the ownership-checking fixture.
"""
from datetime import datetime, timezone
import http.client
import json
import os
import secrets
import select
import signal
import socket
import copy
import ssl
import subprocess
import sys
import time
import uuid
from pathlib import Path
REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
import identity_bootstrap_test as bootstrap_fixture

KEYS_CATALOG = str(uuid.uuid4())


def keys_client_path():
    tail = sys.argv[9:]
    if '--keys-client' not in tail:
        return None
    assert tail.count('--keys-client') == 1, 'duplicate Keys client argument'
    index = tail.index('--keys-client')
    assert index + 1 < len(tail), 'Keys client argument missing'
    path = Path(tail[index + 1]).resolve()
    assert path.is_file(), 'compiled production Keys client required'
    return str(path)

def line(child):
    end = time.monotonic() + 12
    value = b''
    while not value.endswith(b'\n'):
        assert time.monotonic() < end and select.select([child.stdout], [], [], max(0, end - time.monotonic()))[0], 'daemon startup timeout'
        byte = os.read(child.stdout.fileno(), 1)
        assert byte, 'daemon stopped before startup'
        value += byte
    return value.decode().strip()

def daemon_http(root):
    (identity, control) = (str(Path(value).resolve()) for value in sys.argv[7:9])
    config = json.loads((root / 'identity.json').read_text())
    material = {}
    for name in ['token', 'request', 'receipt']:
        path = root / f'{name}.hex'
        path.write_text(secrets.token_hex(32) + '\n')
        path.chmod(0o600)
        material[name + '_key'] = {'id': name + '-v1', 'file': str(path)}

    def openssl(*args):
        result = subprocess.run(['openssl', *args], capture_output=True, timeout=5)
        assert result.returncode == 0, 'key generation failed'
    private = root / 'delegation.key'
    public = root / 'delegation.pem'
    openssl('genpkey', '-algorithm', 'ED25519', '-out', str(private))
    private.chmod(0o600)
    openssl('pkey', '-in', str(private), '-pubout', '-out', str(public))
    public.chmod(0o600)
    material.update(delegation_key={'id': 'delegation-v1', 'public_key_file': str(public), 'private_key_file': str(private)}, issuer='identity-owned-test', clock={'trusted': True, 'maximum_known_skew_ms': 2000})
    keys_client = keys_client_path()
    material['verification'] = {'catalog_id': KEYS_CATALOG if keys_client else str(uuid.uuid4()),
                                'version': 7 if keys_client else 1,
                                'key_version': 11 if keys_client else 1,
                                'valid_until_epoch_seconds': int(time.time()) + 3600}
    config['identity_security'] = material
    (root / 'identity.json').write_text(json.dumps(config))
    # Valid key files with an expired declared public key must fail before any PG contact.
    with socket.socket() as trap:
        trap.bind(('127.0.0.1', 0))
        trap.listen(1)
        expired = copy.deepcopy(config)
        expired['database']['host'] = '127.0.0.1'
        expired['database']['port'] = trap.getsockname()[1]
        expired['identity_security']['verification']['valid_until_epoch_seconds'] = 1
        expired_path = root / 'expired-key.json'
        expired_path.write_text(json.dumps(expired))
        expired_path.chmod(0o600)
        refused = subprocess.run([identity, '--config', str(expired_path)],
                                 capture_output=True, timeout=5)
        assert refused.returncode == 1 and b'probe_port=' not in refused.stdout
        assert not select.select([trap], [], [], 0)[0], 'expired key contacted PG'
    cross_expiry = '--own-tls-cross-expiry' in sys.argv[9:]
    short_lifetime = '--own-tls-expiry' in sys.argv[9:] or cross_expiry
    expires = None
    if short_lifetime:
        tls = root / 'bootstrap-tls'
        expires = int(time.time()) + 8
        begin = datetime.fromtimestamp(expires - 60, timezone.utc).strftime('%y%m%d%H%M%SZ')
        end = datetime.fromtimestamp(expires, timezone.utc).strftime('%y%m%d%H%M%SZ')
        generated = subprocess.run(
            ['openssl', 'ca', '-batch', '-config', 'ca.cnf', '-in', 'server.csr',
             '-out', 'short-server.pem', '-subj', '/CN=ShortIdentity', '-startdate', begin, '-enddate', end,
             '-extfile', 'extensions'], cwd=tls, capture_output=True, timeout=5)
        assert generated.returncode == 0, 'short server certificate generation failed'
        config['tls']['certificate_file'] = str(tls / 'short-server.pem')
        (root / 'identity.json').write_text(json.dumps(config))
    children = []
    log = ''
    sensitive = [(root / 'admin-password').read_text().strip(), 'PRIVATE KEY']
    sensitive.extend((root / f'{name}.hex').read_text().strip()
                     for name in ['token', 'request', 'receipt'])
    sensitive.append(private.read_text().strip())
    try:
        daemon = subprocess.Popen([identity, '--config', str(root / 'identity.json')], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        children.append(daemon)
        state = dict((item.split('=', 1) for item in line(daemon).split()))
        assert state['ready'] == 'false' and int(state['grpc_port']) > 0, 'Identity readiness/startup mismatch'
        tls = root / 'bootstrap-tls'
        if keys_client:
            allowed = 'denied' if '--keys-no-grant' in sys.argv[9:] else 'allowed'
            modes = [('control.pem', 'control.key', allowed),
                     ('server.pem', 'server.key', 'unregistered')]
            for certificate, key_file, mode in modes:
                metadata = material['verification']
                result = subprocess.run(
                    [keys_client, 'localhost:' + state['grpc_port'], str(tls / 'ca.pem'),
                     str(tls / certificate), str(tls / key_file), str(public),
                     material['delegation_key']['id'], str(metadata['version']),
                     str(metadata['key_version']), str(metadata['valid_until_epoch_seconds']), mode],
                    capture_output=True, text=True, timeout=12)
                assert result.returncode == 0, 'real production Keys client failed; diagnostics redacted'
                log += result.stdout + result.stderr
                assert all(secret not in log for secret in sensitive), 'Keys client leaked material'
            print('Real Identity daemon Keys client verified no-JWT bootstrap, actual material and grant rejection')
        config = {'schema_version': 1, 'service': 'control', 'probe_listen': '127.0.0.1:0', 'tls': {'certificate_file': str(tls / 'control.pem'), 'private_key_file': str(tls / 'control.key'), 'ca_file': str(tls / 'ca.pem')}, 'control_api': {'listen': '127.0.0.1:0', 'identity_endpoint': 'localhost:' + state['grpc_port']}}
        (tls / 'control.key').chmod(0o600)
        path = root / 'control.json'
        path.write_text(json.dumps(config))
        path.chmod(0o600)
        browser = subprocess.Popen([control, '--config', str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        children.append(browser)
        port = int(line(browser).split('=')[1])
        assert 'ready=false' in line(browser)
        context = ssl.create_default_context(cafile=str(tls / 'ca.pem'))

        def request(method, target, body=None, token=None, key=None):
            headers = {}
            if token:
                headers['Authorization'] = 'Bearer ' + token
            if key:
                headers['Idempotency-Key'] = key
            if body is not None:
                headers['Content-Type'] = 'application/json'
            connection = http.client.HTTPSConnection('localhost', port, context=context, timeout=5)
            try:
                connection.request(method, target, None if body is None else json.dumps(body), headers)
                response = connection.getresponse()
                data = response.read()
                assert response.getheader('Cache-Control') == 'no-store'
                parsed = json.loads(data) if data else None
                if response.status >= 400:
                    assert parsed['request_id'] and all((secret not in data.decode() for secret in sensitive))
                return (response.status, parsed)
            finally:
                connection.close()

        def expect(reply, status):
            assert reply[0] == status, f"HTTP status={reply[0]} expected={status} code={(reply[1].get('code') if isinstance(reply[1], dict) else None)}"
            return reply[1]
        login = {'username': 'administrator', 'password': sensitive[0]}
        key = 'login-' + str(uuid.uuid4())
        login_started = time.monotonic()
        tokens = expect(request('POST', '/api/v1/auth/login', login, key=key), 200)
        login_elapsed = time.monotonic() - login_started
        sensitive.extend([tokens['access_token'], tokens['refresh_token']])
        replay = expect(request('POST', '/api/v1/auth/login', login, key=key), 200)
        for name in ['access_token', 'refresh_token', 'access_expires_at', 'refresh_expires_at']:
            assert replay[name] == tokens[name]
        expect(request('POST', '/api/v1/auth/login', {**login, 'password': 'wrong'}, key='denied-' + str(uuid.uuid4())), 401)
        principal = expect(request('GET', '/api/v1/auth/me', token=tokens['access_token']), 200)
        assert principal['user_id'] == json.loads((root / 'enrollment.json').read_text())['administrator_id']
        page = expect(request('GET', '/api/v1/auth/sessions?page_size=1', token=tokens['access_token']), 200)
        assert len(page['items']) == 1
        refreshkey = 'refresh-' + str(uuid.uuid4())
        refreshed = expect(request('POST', '/api/v1/auth/refresh', {'refresh_token': tokens['refresh_token']}, key=refreshkey), 200)
        sensitive.extend([refreshed['access_token'], refreshed['refresh_token']])
        replay = expect(request('POST', '/api/v1/auth/refresh', {'refresh_token': tokens['refresh_token']}, key=refreshkey), 200)
        assert replay['refresh_token'] == refreshed['refresh_token']
        expect(request('GET', '/api/v1/auth/me', token=refreshed['access_token']), 200)
        if cross_expiry:
            # Read-only inspection of the same verified owned backend after daemon exit.
            def session_count():
                environment = {key: value for key, value in os.environ.items()
                               if not key.startswith('PG')}
                environment.update(
                    PGHOST='localhost', PGPORT=sys.argv[2], PGUSER='p03_admin',
                    PGDATABASE='p03_identity',
                    PGPASSWORD=(root / 'password').read_text().strip(),
                    PGSSLMODE='verify-full', PGSSLROOTCERT=str(root / 'server.pem'),
                    PGCHANNELBINDING='require', PGCONNECT_TIMEOUT='2')
                result = subprocess.run(
                    [str(Path(sys.argv[6]) / 'psql'), '-X', '-qAt', '-v', 'ON_ERROR_STOP=1'],
                    input='SHOW data_directory; SELECT count(*) FROM kiarabot.sessions;',
                    text=True, env=environment, capture_output=True, timeout=5)
                assert result.returncode == 0, 'owned backend inspection failed'
                directory, count = result.stdout.strip().splitlines()
                assert Path(directory).resolve() == root / 'data', 'owned backend changed'
                return int(count)

            before = session_count()
            # Start a fresh password-verified login while credentials are still valid,
            # then verify the in-flight operation finishes after their absolute expiry.
            margin = max(0.01, min(0.1, login_elapsed / 3))
            while time.time() < expires - margin:
                time.sleep(min(0.005, expires - margin - time.time()))
            assert time.time() < expires and daemon.poll() is None
            crossed = request('POST', '/api/v1/auth/login', login,
                              key='cross-expiry-' + str(uuid.uuid4()))
            assert time.time() >= expires, 'login did not cross the real expiry boundary'
            assert crossed[0] in (503, 504), 'cross-expiry login was accepted'
            assert daemon.wait(timeout=4) == 1
            assert session_count() == before, 'cross-expiry request committed a new session'
            print('Real in-flight login crossed own TLS expiry without a new PG session commit')
            return
        if short_lifetime:
            # Existing Control IdentityAuthClient retains its established gRPC channel.
            # No system clock manipulation or replacement peer is used.
            while time.time() < expires:
                time.sleep(min(0.05, expires - time.time()))
            assert daemon.poll() is None, 'expiry observation missed the live daemon window'
            refused = request('GET', '/api/v1/auth/me', token=refreshed['access_token'])
            assert refused[0] == 503, 'expired own TLS accepted on established channel'
            assert daemon.wait(timeout=4) == 1, 'expired credentials did not stop RPC'
            print('Actual short server chain expiry rejected established-channel operations and stopped Identity')
            return
        expect(request('POST', '/api/v1/auth/sessions/' + page['items'][0]['session_id'] + '/revoke', token=refreshed['access_token'], key='revoke-' + str(uuid.uuid4())), 204)
        expect(request('GET', '/api/v1/auth/me', token=refreshed['access_token']), 401)
        print('Production daemon HTTPS -> mTLS -> current Admin -> Core -> real PG login/replay/me/list/refresh/revoke passed')
    finally:
        shutdown_failures = []
        for child in reversed(children):
            if child.poll() is None:
                try:
                    child.send_signal(signal.SIGTERM)
                except ProcessLookupError:
                    pass
            try:
                (output, error) = child.communicate(timeout=6)
            except subprocess.TimeoutExpired:
                child.kill()
                (output, error) = child.communicate(timeout=6)
                shutdown_failures.append('daemon shutdown timed out')
            log += output.decode() + error.decode()
            expected = 1 if short_lifetime and child is daemon and expires is not None and time.time() >= expires else 0
            if child.returncode != expected:
                shutdown_failures.append('daemon shutdown failed')
        # Reap every owned daemon before reporting any single child's failure.
        assert all((secret not in log for secret in sensitive)), 'daemon leaked secret'
        assert not shutdown_failures, '; '.join(shutdown_failures)
if __name__ == '__main__':
    try:
        catalog = KEYS_CATALOG if keys_client_path() and '--keys-no-grant' not in sys.argv[9:] else None
        bootstrap_fixture.main(control_server_certificate=True, after_bootstrap=daemon_http,
                               verification_catalog=catalog)
    except Exception:
        import traceback
        frames = traceback.extract_tb(sys.exc_info()[2])
        locations = ','.join((str(frame.lineno) for frame in frames[-3:]))
        print(f'P03 assertion: daemon HTTP regression lines {locations}; diagnostics redacted', file=sys.stderr)
        sys.exit(1)
