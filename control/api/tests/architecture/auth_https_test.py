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
import urllib.parse

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


def main(control, fixture, model_fixture=None, owner_fixture=None, runtime_fixture=None):
    with tempfile.TemporaryDirectory(prefix='p04-control-') as folder:
        root = Path(folder)
        config = certificates(root, 'ec')
        tls = config['tls']
        identity = subprocess.Popen([fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        model = None
        owner_process = None
        runtime = None
        child = None
        secrets = ['never-print-password', 'never-print-upstream-secret', ACCESS,
                   'secret-refresh-credential-login', 'secret-access-credential-refresh',
                   'fixture-proof:', 'PRIVATE KEY']
        logs = ''
        try:
            identity_port = int(line(identity))
            runtime_port = None
            if runtime_fixture:
                runtime = subprocess.Popen([runtime_fixture, tls['ca_file'], tls['certificate_file'],
                                            tls['private_key_file']],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                runtime_port = int(line(runtime))
            model_port = None
            if model_fixture:
                model = subprocess.Popen([model_fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                model_port = int(line(model))
            owner_port = None
            if owner_fixture:
                owner_process = subprocess.Popen([owner_fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
                                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                owner_port = int(line(owner_process))
            config['service'] = 'control'
            config.pop('grpc_listen')
            config['control_api'] = {'listen': '127.0.0.1:0', 'identity_endpoint': f'localhost:{identity_port}'}
            if runtime_port is not None:
                config['control_api']['runtime_endpoint'] = f'localhost:{runtime_port}'
            if model_port is not None:
                config['control_api']['model_endpoint'] = f'localhost:{model_port}'
            if owner_port is not None:
                config['control_api']['runtime_endpoint'] = f'localhost:{owner_port}'
                config['control_api']['scheduler_endpoint'] = f'localhost:{owner_port}'
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
            replay_after_access_expiry = login('expired_access_replay')
            assert replay_after_access_expiry[0] == 200, replay_after_access_expiry
            replayed_pair = replay_after_access_expiry[1]
            assert replayed_pair['expires_in'] == 0
            for field in ['access_token', 'refresh_token', 'access_expires_at', 'refresh_expires_at']:
                assert replayed_pair[field] == tokens[field], (field, replayed_pair)
            assert replayed_pair['checked_at'] == '2023-11-14T22:28:21.500Z'
            assert request('POST', '/api/v1/auth/login', {'username': 'alice', 'password': 'never-print-password'},
                           {'Content-Type': 'Application/JSON; Charset="UTF-8"', 'Idempotency-Key': KEY})[0] == 200
            refreshed = request('POST', '/api/v1/auth/refresh', {'refresh_token': tokens['refresh_token']},
                                {'Content-Type': 'application/json', 'Idempotency-Key': KEY})
            assert refreshed[0] == 200, refreshed
            assert request('POST', '/api/v1/auth/refresh', {'refresh_token': tokens['refresh_token']},
                           {'Content-Type': 'application/json', 'Idempotency-Key': KEY})[1] == refreshed[1]
            # P12c: health is local; log/audit and owner-backed reads never fabricate success when
            # no authoritative source is registered in this minimal fixture configuration.
            status, body, _ = request('GET', '/health/live')
            assert status == 200 and body == {'state': 'HEALTHY'}, (status, body)
            for sensitive_path in ('/api/v1/logs', '/api/v1/audit'):
                status, body, _ = request('GET', sensitive_path,
                                          headers={'Authorization': 'Bearer ' + ACCESS})
                assert status in (200, 403, 503), (sensitive_path, status, body)
                if status == 200:
                    assert body['items'], body
                else:
                    assert body['code'] in ('PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE'), body
            for anonymous_path in ('/api/v1/logs', '/api/v1/audit'):
                status, body, _ = request('GET', anonymous_path)
                assert status == 401 and body['code'] == 'UNAUTHENTICATED', (anonymous_path, body)
            for invalid_target in (
                    '/api/v1/logs?page_token=not-a-cursor',
                    '/api/v1/logs?occurred_from=not-a-time',
                    '/api/v1/audit?occurred_until=not-a-time',
                    '/api/v1/audit?actor=' + 'a' * 300,
                    '/api/v1/logs?unexpected=1'):
                status, body, _ = request('GET', invalid_target,
                                          headers={'Authorization': 'Bearer ' + ACCESS})
                assert status == 400 and body['code'] == 'INVALID_ARGUMENT', (
                    invalid_target, status, body)
            if runtime_fixture is not None:
                # Cross-owner success aggregation over real Control->Identity/Runtime mTLS:
                # composite keyset token, owner labels, DESC ordering and two-page paging.
                observed = []
                owners = set()
                token = ''
                pages = 0
                while True:
                    target = '/api/v1/logs?page_size=2' + (
                        '' if not token else '&page_token=' + urllib.parse.quote(token, safe=''))
                    status, body, _ = request('GET', target,
                                              headers={'Authorization': 'Bearer ' + ACCESS})
                    assert status == 200, (status, body)
                    pages += 1
                    for item in body['items']:
                        observed.append(item['id'])
                        owners.add(item['owner'])
                        assert item['message'] == 'request.recorded', item
                    token = body['page'].get('next_page_token', '')
                    if not token:
                        break
                    assert pages < 8, 'pagination did not terminate'
                assert pages == 4, pages
                assert observed == ['10', '20', '9', '30', '40', '19', '8'], observed
                assert owners == {'identity', 'runtime'}, owners
                status, body, _ = request(
                    'GET', '/api/v1/audit?page_size=20&actor=alice',
                    headers={'Authorization': 'Bearer ' + ACCESS})
                assert status == 200, (status, body)
                assert body['items'] and all(item['actor'] == 'alice' for item in body['items']), body
                assert all(item['owner'] == 'identity' for item in body['items']), body
            status, body, _ = request('GET', '/api/v1/health',
                                      headers={'Authorization': 'Bearer ' + ACCESS})
            assert status in (200, 403, 503), (status, body)
            if status == 200:
                assert body['state'] in ('HEALTHY', 'DEGRADED'), body
                assert {item['component'] for item in body['components']} == {
                    'identity', 'runtime', 'scheduler', 'model'}, body
            else:
                assert body['code'] in ('PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE',
                                            'CAPABILITY_NOT_SUPPORTED'), body
            status, body, _ = request('GET', '/api/v1/health')
            assert status == 401, (status, body)
            installation = '11111111-1111-4111-8111-111111111111'
            instance = '22222222-2222-4222-8222-222222222222'
            instance_base = f'/api/v1/installations/{installation}/instances/{instance}'
            status, body, _ = request(
                'PUT', instance_base + '/plugin-state',
                {'installation_id': installation, 'instance_id': instance,
                 'desired': 'DESIRED_RUNNING', 'expected_version': '1'},
                headers={'Authorization': 'Bearer ' + ACCESS, 'Idempotency-Key': KEY})
            assert status in (400, 403, 404, 409, 503), (status, body)
            assert body['code'] in ('INVALID_ARGUMENT', 'PERMISSION_DENIED', 'NOT_FOUND',
                                    'CONFLICT', 'UPSTREAM_UNAVAILABLE',
                                            'CAPABILITY_NOT_SUPPORTED'), body
            status, body, _ = request(
                'GET', instance_base + '/configuration',
                headers={'Authorization': 'Bearer ' + ACCESS})
            assert status in (200, 400, 403, 404, 503), (status, body)
            if status == 200:
                assert 'runtime' in body and 'plugin' in body, body
            else:
                assert body['code'] in ('INVALID_ARGUMENT', 'PERMISSION_DENIED', 'NOT_FOUND',
                                        'UPSTREAM_UNAVAILABLE',
                                            'CAPABILITY_NOT_SUPPORTED'), body
            lifecycle_resource = 'bbbbbbbb-1111-4111-8111-bbbbbbbbbbbb'
            lifecycle_base = f'/api/v1/resources/RESOURCE_INSTANCE/{lifecycle_resource}'
            lifecycle_body = {'resource': {'kind': 'RESOURCE_INSTANCE',
                                           'id': lifecycle_resource},
                              'expected_version': '1'}
            for lifecycle_action in ('delete', 'restore'):
                status, body, _ = request(
                    'POST', f'{lifecycle_base}/{lifecycle_action}', lifecycle_body,
                    headers={'Authorization': 'Bearer ' + ACCESS,
                             'Idempotency-Key': KEY})
                assert status in (202, 403, 404, 409, 503), (lifecycle_action, status, body)
                if status == 202:
                    assert body['operation']['operation_id'], body
                else:
                    assert body['code'] in ('PERMISSION_DENIED', 'NOT_FOUND', 'CONFLICT',
                                            'UPSTREAM_UNAVAILABLE',
                                            'CAPABILITY_NOT_SUPPORTED'), body
            status, body, _ = request(
                'POST', lifecycle_base + '/delete',
                {'resource': {'kind': 'RESOURCE_INSTANCE', 'id': lifecycle_resource}},
                headers={'Authorization': 'Bearer ' + ACCESS, 'Idempotency-Key': KEY})
            assert status == 400 and body['code'] == 'INVALID_ARGUMENT', (status, body)
            status, body, _ = request(
                'POST', lifecycle_base + '/delete', lifecycle_body,
                headers={'Authorization': 'Bearer ' + ACCESS})
            assert status == 400 and body['code'] == 'INVALID_ARGUMENT', (status, body)
            status, body, _ = request('POST', lifecycle_base + '/delete', lifecycle_body)
            assert status == 401 and body['code'] == 'UNAUTHENTICATED', (status, body)
            status, body, _ = request('GET', '/api/v1/principals',
                                      headers={'Authorization': 'Bearer ' + ACCESS})
            assert status in (200, 403, 503), (status, body)
            if status == 200:
                assert body.get('items') or body.get('principals'), body
            else:
                assert body['code'] in ('PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE',
                                            'CAPABILITY_NOT_SUPPORTED'), body
            for target_path in ('/api/v1/tasks',
                                f'/api/v1/accounts/{SESSION}/action-policy'):
                status, body, _ = request('GET', target_path,
                                          headers={'Authorization': 'Bearer ' + ACCESS})
                assert status in (200, 403, 503), (target_path, status, body)
                if status == 200:
                    assert body, body
                else:
                    assert body['code'] in ('PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE',
                                            'CAPABILITY_NOT_SUPPORTED'), body
            status, body, _ = request(
                'PUT', f'/api/v1/adapter-bindings/{SESSION}',
                {'binding': {'account_id': SESSION, 'adapter_instance_id': 'adapter-1'},
                 'expected_version': '1'},
                headers={'Authorization': 'Bearer ' + ACCESS, 'Idempotency-Key': KEY})
            assert status in (200, 403, 503), (status, body)
            if status == 200:
                assert body['operation']['operation_id'], body
            else:
                assert body['code'] in ('PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE',
                                            'CAPABILITY_NOT_SUPPORTED'), body
            operation_scope = urllib.parse.quote(json.dumps({'resources': [
                {'kind': 'RESOURCE_ACCOUNT', 'id': SESSION}]}))
            operation_owners = ('runtime', 'scheduler') if model_port is not None else (
                'runtime', 'scheduler', 'model')
            for owner_service in operation_owners:
                status, body, _ = request(
                    'GET', f'/api/v1/operations/{owner_service}/{SESSION}?scope={operation_scope}',
                    headers={'Authorization': 'Bearer ' + ACCESS})
                assert status in (200, 400, 403, 503), (owner_service, status, body)
                if status == 200:
                    assert body['operation'], body
                else:
                    assert body['code'] in ('INVALID_ARGUMENT', 'PERMISSION_DENIED',
                                            'UPSTREAM_UNAVAILABLE',
                                            'CAPABILITY_NOT_SUPPORTED'), (owner_service, body)
            model_id = '11111111-1111-4111-8111-111111111111'
            model_scope = urllib.parse.quote(json.dumps({'resources': [
                {'kind': 'RESOURCE_MODEL', 'id': model_id}]}))
            if model_port is not None:
                auth_header = {'Authorization': 'Bearer ' + ACCESS}
                status, body, _ = request('GET', '/api/v1/models/' + model_id + '?scope=' + model_scope,
                                          headers=auth_header)
                assert status == 200 and body['model']['model_id'] == model_id, (status, body)
                status, body, _ = request('GET', '/api/v1/models?scope=' + model_scope,
                                          headers=auth_header)
                assert status == 200 and body['items'][0]['id'] == model_id and \
                    body['items'][0]['name'] == 'default' and \
                    body['items'][0]['state'] == 'LIFECYCLE_ACTIVE' and \
                    'page' in body and 'models' not in body, (status, body)
                update = {'config': {
                    'model_id': model_id, 'alias': 'default', 'provider_kind': 'openai_chat_completions',
                    'endpoint': 'https://api.example.invalid/v1', 'provider_model': 'fixture-model',
                    'timeout_ms': 30000, 'version': '3'}, 'expected_version': '3'}
                status, body, _ = request('PUT', '/api/v1/models/' + model_id, update,
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'model-update-key-1'})
                assert status == 200 and body['model']['model_id'] == model_id, (status, body)
                status, body, _ = request('POST', '/api/v1/models/' + model_id + '/disable',
                                          {'expected_version': '3'},
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'model-disable-key-1'})
                assert status == 200 and body['model']['state'] == 'LIFECYCLE_DISABLED', (status, body)
                status, body, _ = request('POST', '/api/v1/models/' + model_id + '/enable',
                                          {'expected_version': '3'},
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'model-enable-key-1'})
                assert status == 200 and body['model']['state'] == 'LIFECYCLE_ACTIVE', (status, body)
                secret = __import__('base64').b64encode(b'fixture-provider-secret').decode()
                status, body, _ = request('PUT', '/api/v1/models/' + model_id + '/secret',
                                          {'model_id': model_id, 'new_secret': secret,
                                           'expected_version': '3'},
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'model-secret-key-1'})
                assert status == 200 and body == {'version': '9'}, (status, body)
                assert 'fixture-provider-secret' not in json.dumps(body)
                status, body, _ = request('GET', '/api/v1/models/' + model_id +
                                          '/configuration?scope=' + model_scope, headers=auth_header)
                assert status == 200 and body['secret_configured'] is True and \
                    body['secret_version'] == '9' and body['observed_at'], (status, body)
                assert 'fixture-provider-secret' not in json.dumps(body), body
                status, body, _ = request('GET', '/api/v1/models/' + model_id +
                                          '/calls/call-1?scope=' + model_scope, headers=auth_header)
                assert status == 200 and body['call']['execution_outcome'] == 'EXECUTION_UNKNOWN' and \
                    body['call']['may_have_billed'] is True, (status, body)
                status, body, _ = request('POST', '/api/v1/models/' + model_id +
                                          '/calls/call-1/resolve',
                                          {'disposition': 'EXECUTED_BILLED', 'expected_version': '3'},
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'model-resolve-key-1'})
                assert status == 200 and body['call']['disposition'] == 'EXECUTED_BILLED', (status, body)
                status, body, _ = request('GET', '/api/v1/operations/model/operation-1?scope=' +
                                          model_scope, headers=auth_header)
                assert status == 200 and body['operation']['state'] == 'OPERATION_SUCCEEDED', (status, body)
            if owner_port is not None:
                auth_header = {'Authorization': 'Bearer ' + ACCESS}
                account_scope = urllib.parse.quote(json.dumps({'resources': [
                    {'kind': 'RESOURCE_ACCOUNT', 'id': SESSION}]}))
                deadline = time.monotonic() + 3
                status = 0
                while time.monotonic() < deadline:
                    status, body, _ = request('GET', '/api/v1/health', headers=auth_header)
                    if status == 200:
                        break
                    time.sleep(0.05)
                assert status == 200 and body['state'] in ('HEALTHY', 'DEGRADED'), (status, body)
                assert {item['component'] for item in body['components']} == {
                    'identity', 'runtime', 'scheduler', 'model'}, body
                status, body, _ = request('GET', '/api/v1/actions/' + SESSION + '?scope=' + account_scope,
                                          headers=auth_header)
                assert status == 200 and body['action_id'] == SESSION and \
                    body['status'] == 'SUCCEEDED' and body['execution_outcome'] == \
                    'EXECUTION_KNOWN_SUCCEEDED', (status, body)
                status, body, _ = request('GET', '/api/v1/principals', headers=auth_header)
                assert status == 200 and (body.get('items') or body.get('principals')), (status, body)
                for sensitive_endpoint in ('/api/v1/logs', '/api/v1/audit'):
                    status, body, _ = request('GET', sensitive_endpoint, headers=auth_header)
                    assert status == 200 and body['items'], (sensitive_endpoint, status, body)
                    assert {item.get('owner') for item in body['items']} >= {'identity', 'runtime'}, body
                account = 'fixture-account'
                binding = {'binding': {'account_id': account, 'adapter_instance_id': 'adapter-fixture'},
                           'expected_version': '1'}
                status, body, _ = request('PUT', f'/api/v1/adapter-bindings/{account}', binding,
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'matrix-bind-1'})
                assert status == 200 and body['operation']['operation_id'] == 'bind-operation', (status, body)
                status, body, _ = request('DELETE', f'/api/v1/adapter-bindings/{account}', binding,
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'matrix-unbind-1'})
                assert status == 200, (status, body)
                installation = {'catalog': {'kind': 'RESOURCE_CATALOG', 'id': 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'},
                                'plugin_type_id': 'plugin-fixture',
                                'manifest': {'id': 'plugin-fixture', 'name': 'fixture',
                                             'version': '1.0.0', 'api_version': 'kiarabot.plugin.v1',
                                             'runtime': 'python-rpc', 'isolation': 'process',
                                             'entrypoint': 'fixture:create'}}
                status, body, _ = request('POST', '/api/v1/installations', installation,
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'matrix-install-1'})
                assert status == 200 and body['installation'], (status, body)
                catalog = {'kind': 'RESOURCE_CATALOG', 'id': 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'}
                catalog_query = urllib.parse.urlencode({'catalog': json.dumps(catalog), 'page_size': 2})
                account_id = '11111111-1111-4111-8111-111111111111'
                for route, field, expected in [('accounts', 'accounts', [account_id]),
                                               ('installations', 'installations', ['33333333-3333-4333-8333-333333333333']),
                                               ('instances', 'instances', ['44444444-4444-4444-8444-444444444444']),
                                               ('adapter-instances', 'adapters', [])]:
                    result = request('GET', '/api/v1/' + route + '?' + catalog_query, headers=auth_header)
                    assert result[0] == 200, (route, result)
                    assert len(result[1][field]) == len(expected), result
                    if expected:
                        assert list(result[1][field][0].values())[0] == expected[0], result
                    replay = request('GET', '/api/v1/' + route + '?' + catalog_query, headers=auth_header)
                    assert replay[:2] == result[:2], (result, replay)
                default_query = urllib.parse.urlencode({'catalog': json.dumps(catalog)})
                for route in ['accounts', 'installations', 'instances', 'adapter-instances']:
                    default_page = request('GET', '/api/v1/' + route + '?' + default_query, headers=auth_header)
                    assert default_page[0] == 200 and default_page[1]['snapshot']['page']['page_size'] == 20, default_page
                assert request('GET', '/api/v1/hosts?' + catalog_query, headers=auth_header)[0] == 503
                selection = {'resources': [{'kind': 'RESOURCE_ACCOUNT', 'id': account_id}]}
                selected_query = catalog_query + '&' + urllib.parse.urlencode({'scope': json.dumps(selection)})
                assert request('GET', '/api/v1/accounts?' + selected_query, headers=auth_header)[0] == 200
                selection['resources'][0]['id'] = '22222222-2222-4222-8222-222222222222'
                refused_query = catalog_query + '&' + urllib.parse.urlencode({'scope': json.dumps(selection)})
                assert request('GET', '/api/v1/accounts?' + refused_query, headers=auth_header)[0] == 403
                selection['resources'][0] = catalog
                malformed_query = catalog_query + '&' + urllib.parse.urlencode({'scope': json.dumps(selection)})
                assert request('GET', '/api/v1/accounts?' + malformed_query, headers=auth_header)[0] == 400
                assert request('GET', '/api/v1/accounts', headers=auth_header)[0] == 400
                assert request('GET', '/api/v1/accounts?' + catalog_query + '&page_token=bad', headers=auth_header)[0] == 400
                assert request('GET', '/api/v1/accounts?' + catalog_query + '&page_token=down', headers=auth_header)[0] == 503
                empty_page = request('GET', '/api/v1/accounts?' + catalog_query,
                                     headers={'Authorization': 'Bearer fixture-empty'})
                assert empty_page[0] == 200 and empty_page[1]['accounts'] == [], empty_page
                oversized = request('GET', '/api/v1/accounts?' + catalog_query,
                                    headers={'Authorization': 'Bearer fixture-many'})
                assert oversized[0] == 429 and oversized[1]['code'] == 'RATE_LIMITED', oversized
                for suffix in ['revoked', 'disabled', 'expired']:
                    refused = request('GET', '/api/v1/accounts?' + catalog_query,
                                      headers={'Authorization': 'Bearer fixture-' + suffix})
                    assert refused[0] == 503, refused
                created_account_body = {'catalog': catalog, 'platform': 'qq', 'platform_account_id': 'fixture'}
                account_headers = {**auth_header, 'Content-Type': 'application/json',
                                   'Idempotency-Key': 'matrix-account-create-0001'}
                account_created = request('POST', '/api/v1/accounts', created_account_body, account_headers)
                assert account_created[0] == 200, account_created
                account_replay = request('POST', '/api/v1/accounts', created_account_body,
                                         {**account_headers, 'X-Request-ID': 'new-request-for-same-key'})
                assert account_replay[:2] == account_created[:2], account_replay
                assert request('POST', '/api/v1/accounts', created_account_body, auth_header)[0] == 400
                instance_body = {'installation_id': 'installation-fixture',
                                 'host_id': 'host-fixture', 'externally_managed': False}
                status, body, _ = request('POST', '/api/v1/installations/installation-fixture/instances',
                                          instance_body,
                                          {**auth_header, 'Content-Type': 'application/json',
                                           'Idempotency-Key': 'matrix-instance-1'})
                assert status == 200 and body['instance']['instance_id'] == 'instance-fixture', (status, body)
                instance_base = '/api/v1/installations/installation-fixture/instances/instance-fixture'
                status, body, _ = request(
                    'PUT', instance_base + '/plugin-state',
                    {'installation_id': 'installation-fixture', 'instance_id': 'instance-fixture',
                     'desired': 'DESIRED_RUNNING', 'expected_version': '2'},
                    {**auth_header, 'Content-Type': 'application/json',
                     'Idempotency-Key': 'matrix-state-1'})
                assert status == 200 and body['plugin']['apply_state'] == 'CONFIG_APPLY_PENDING', (status, body)
                status, body, _ = request('GET', instance_base + '/configuration', headers=auth_header)
                assert status == 200 and body['runtime']['apply_state'] == 'CONFIG_APPLY_PENDING', (status, body)
                status, body, _ = request(
                    'PUT', instance_base + '/configuration',
                    {'configuration': {'timeout': 5}, 'expected_version': '2'},
                    {**auth_header, 'Content-Type': 'application/json',
                     'Idempotency-Key': 'matrix-config-1'})
                assert status == 200 and body['operation']['operation_id'] == 'config-operation', (status, body)
                status, body, _ = request('GET', '/api/v1/accounts/account-fixture/action-policy',
                                          headers=auth_header)
                assert status == 200 and body['policy']['account_id'] == 'account-fixture', (status, body)
                status, body, _ = request(
                    'PUT', '/api/v1/accounts/account-fixture/action-policy',
                    {'policy': {'account_id': 'account-fixture', 'mute_enabled': True,
                                'kick_enabled': False, 'allowed_group_ids': ['group-fixture'],
                                'mute_daily_budget': 2, 'kick_daily_budget': 1, 'version': '4'},
                     'expected_version': '4'},
                    {**auth_header, 'Content-Type': 'application/json',
                     'Idempotency-Key': 'matrix-policy-1'})
                assert status == 200 and body['policy']['version'] == '5', (status, body)
                status, body, _ = request('GET', '/api/v1/tasks?scope=' + account_scope,
                                          headers=auth_header)
                assert status == 200 and body['items'][0]['id'] == 'task-fixture', (status, body)
                status, body, _ = request('GET', '/api/v1/tasks/task-fixture?scope=' + account_scope,
                                          headers=auth_header)
                assert status == 200 and body['task']['task_id'] == 'task-fixture', (status, body)
                status, body, _ = request(
                    'POST', '/api/v1/tasks/task-fixture/cancel?scope=' + account_scope, {},
                    {**auth_header, 'Content-Type': 'application/json',
                     'Idempotency-Key': 'matrix-cancel-1'})
                assert status == 200 and body['task']['state'] == 'SCHEDULED_TASK_CANCELLED', (status, body)
                status, body, _ = request('GET', '/api/v1/occurrences/occurrence-fixture?scope=' +
                                          account_scope, headers=auth_header)
                assert status == 200 and body['occurrence']['occurrence_id'] == \
                    'occurrence-fixture', (status, body)
                assert body['occurrence']['task_id'] == 'task-fixture' and \
                    body['occurrence']['state'] == 'OPERATION_SUCCEEDED', body
                status, body, _ = request('GET', '/api/v1/occurrences/occurrence-fixture?scope=' +
                                          account_scope, headers=auth_header)
                assert status == 200, (status, body)
                status, body, _ = request('GET', '/api/v1/operations/runtime/op-fixture?scope=' +
                                          account_scope, headers=auth_header)
                assert status == 200 and body['operation']['state'] == 'OPERATION_BLOCKED' and \
                    body['operation']['participants'], (status, body)
                status, body, _ = request('GET', '/api/v1/operations/scheduler/op-fixture?scope=' +
                                          account_scope, headers=auth_header)
                assert status == 200 and body['operation']['state'] == 'OPERATION_SUCCEEDED', (status, body)
                lifecycle_resource = 'fixture-instance'
                for lifecycle_action, expected_operation in (('delete', 'delete-operation'),
                                                             ('restore', 'restore-operation')):
                    status, body, _ = request(
                        'POST', f'/api/v1/resources/RESOURCE_INSTANCE/{lifecycle_resource}/'
                                f'{lifecycle_action}',
                        {'resource': {'kind': 'RESOURCE_INSTANCE', 'id': lifecycle_resource},
                         'expected_version': '1'},
                        {**auth_header, 'Content-Type': 'application/json',
                         'Idempotency-Key': 'matrix-' + lifecycle_action + '-idempotency-0001'})
                    assert status == 202 and body['operation']['operation_id'] == expected_operation, \
                        (status, body)
                status, body, _ = request(
                    'PUT', '/api/v1/adapter-bindings/conflict-account',
                    {'binding': {'account_id': 'conflict-account',
                                 'adapter_instance_id': 'adapter-fixture'},
                     'expected_version': '1'},
                    {**auth_header, 'Content-Type': 'application/json',
                     'Idempotency-Key': 'matrix-conflict-1'})
                assert status == 409 and body['code'] == 'CONFLICT', (status, body)
                status, body, _ = request(
                    'PUT', '/api/v1/adapter-bindings/unavailable-account',
                    {'binding': {'account_id': 'unavailable-account',
                                 'adapter_instance_id': 'adapter-fixture'},
                     'expected_version': '1'},
                    {**auth_header, 'Content-Type': 'application/json',
                     'Idempotency-Key': 'matrix-unavailable-1'})
                assert status == 503 and body['code'] == 'UPSTREAM_UNAVAILABLE', (status, body)
                # Legacy /plugins commands are deliberately not reused for instances.
                status, _, _ = request('POST', '/api/v1/plugins/fixture/enable', {},
                                       {**auth_header, 'Content-Type': 'application/json',
                                        'Idempotency-Key': 'matrix-legacy-1'})
                assert status in (404, 405), status
                status, body, _ = request('GET', '/api/v1/operations/host/op-fixture?scope=' +
                                          account_scope, headers=auth_header)
                assert status == 400 and body['code'] == 'INVALID_ARGUMENT', (status, body)
            scope = urllib.parse.quote(json.dumps({'resources': [
                {'kind': 'RESOURCE_ACCOUNT', 'id': SESSION}]}))
            status, body, _ = request(
                'GET', '/api/v1/actions/' + SESSION + '?scope=' + scope,
                headers={'Authorization': 'Bearer ' + ACCESS})
            # With Runtime configured the typed action result is returned; without it the owner
            # path is either denied (403) or unavailable (503), never fabricated.
            assert status in (200, 403, 503), (status, body)
            if status == 200:
                assert body['action_id'] and body['status'], body
            else:
                assert body['code'] in ('PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE',
                                        'CAPABILITY_NOT_SUPPORTED'), body
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
                # The cap test needs at least six of the 70 excess sockets closed. Under a loaded
                # CI host the accept/close path can take longer than one 0.5s select window, so
                # observe for a bounded two seconds instead of sampling once.
                closed = 0
                seen = set()
                deadline = time.monotonic() + 2
                while closed < 6 and time.monotonic() < deadline:
                    ready = select.select(stalled_connections, [], [], 0.25)[0]
                    for connection in ready:
                        if connection in seen:
                            continue
                        seen.add(connection)
                        try:
                            data = connection.recv(1)
                        except OSError:
                            data = b''
                        if data == b'':
                            closed += 1
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
            if model is not None and model.poll() is None:
                stop(model)
            if runtime is not None and runtime.poll() is None:
                stop(runtime)
            if owner_process is not None and owner_process.poll() is None:
                stop(owner_process)
            if identity.poll() is None:
                stop(identity)


if __name__ == '__main__':
    main(*sys.argv[1:])
