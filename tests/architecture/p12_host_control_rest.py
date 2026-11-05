#!/usr/bin/env python3
"""HOST-04 Control REST acceptance for the Host owner management surface.

Runs the shipped Control binary against the real-TLS Identity fixture and the generated
owner fixture that also serves HostManagementService. It proves the Control transport /
delegation / error mapping contract only: Host SQLite, Runtime gate and execution evidence
stay Host-owned; the fixture returns canned typed snapshots and never claims completion.
"""
from __future__ import annotations

import json
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.parse
from http import client
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'control/api/tests/architecture'))
sys.path.insert(0, str(REPO / 'core/tests/platform'))
from auth_https_test import ACCESS, KEY, line, stop  # noqa: E402
from service_config_test import certificates  # noqa: E402

HOST_ID = '33333333-3333-4333-8333-333333333333'
CANCEL_OPERATION = '44444444-4444-4444-8444-444444444444'
SCOPE = json.dumps({'resources': [{'kind': 'RESOURCE_HOST', 'id': HOST_ID}]})
QUERY = '?scope=' + urllib.parse.quote(SCOPE)


def main(control, identity_fixture, owner_fixture):
    with tempfile.TemporaryDirectory(prefix='p12-host-rest-') as folder:
        root = Path(folder)
        config = certificates(root, 'ec')
        tls = config['tls']
        identity = subprocess.Popen(
            [identity_fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        owner = None
        child = None
        logs = ''
        try:
            identity_port = int(line(identity))
            owner = subprocess.Popen(
                [owner_fixture, tls['ca_file'], tls['certificate_file'], tls['private_key_file']],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            owner_port = int(line(owner))
            config['service'] = 'control'
            config.pop('grpc_listen', None)
            config['control_api'] = {
                'listen': '127.0.0.1:0',
                'identity_endpoint': f'localhost:{identity_port}',
                'host_endpoint': f'localhost:{owner_port}',
            }
            path = root / 'control.json'
            path.write_text(json.dumps(config))
            child = subprocess.Popen([control, '--config', str(path)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            first = line(child)
            assert first.startswith('control_https_port='), first
            port = int(first.split('=')[1])
            line(child)  # ready/degraded state; requests retry below

            context = ssl.create_default_context(cafile=tls['ca_file'])

            def request(method, target, body=None, token=None, key=True):
                connection = client.HTTPSConnection('localhost', port, context=context, timeout=5)
                headers = {}
                if token:
                    headers['Authorization'] = 'Bearer ' + token
                if body is not None:
                    body = json.dumps(body)
                    headers['Content-Type'] = 'application/json'
                    if key:
                        headers['Idempotency-Key'] = KEY
                try:
                    connection.request(method, target, body, headers)
                    response = connection.getresponse()
                    content = response.read()
                    parsed = json.loads(content) if content else None
                    return response.status, parsed
                finally:
                    connection.close()

            status, tokens = request('POST', '/api/v1/auth/login',
                                     {'username': 'alice', 'password': 'never-print-password'})
            assert status == 200 and tokens['access_token'] == ACCESS, (status, tokens)
            token = tokens['access_token']
            deadline = time.monotonic() + 5
            while True:
                status, body = request('GET', '/api/v1/health', token=token)
                if status == 200:
                    break
                assert time.monotonic() < deadline, (status, body)
                time.sleep(0.05)

            # 1) Anonymous and authenticated maintenance reads: owner snapshot only.
            maintenance_path = '/api/v1/hosts/' + HOST_ID + '/maintenance'
            anonymous = request('GET', maintenance_path)
            assert anonymous[0] == 401 and anonymous[1]['code'] == 'UNAUTHENTICATED', anonymous
            status, body = request('GET', maintenance_path, token=token)
            assert status == 200, (status, body)
            maintenance = body['maintenance']
            assert maintenance['host_id'] == HOST_ID, maintenance
            assert maintenance['version'] == '4', maintenance
            assert maintenance['admission'] == 'HOST_ADMISSION_OPEN', maintenance
            assert maintenance['observed_at'].endswith('Z'), maintenance

            # 2) Accepted begin is 202 with the owner operation, never claimed completion.
            def write_begin(expected_version):
                return request('POST', maintenance_path,
                               {'expected_version': str(expected_version), 'grace_ms': 500},
                               token=token)

            status, body = write_begin(4)
            assert status == 202, (status, body)
            assert body['maintenance']['admission'] == 'HOST_ADMISSION_CLOSED', body
            assert body['operation']['ref']['owner_service'] == 'host', body
            assert body['operation']['state'] == 'OPERATION_ACCEPTED', body
            assert body['operation']['execution_outcome'] == 'EXECUTION_UNKNOWN', body

            # 3) Owner CONFLICT / UNAVAILABLE and bounded validation errors stay stable.
            conflict = write_begin(9999)
            assert conflict[0] == 409 and conflict[1]['code'] == 'CONFLICT', conflict
            unavailable = write_begin(8888)
            assert unavailable[0] == 503 and unavailable[1]['code'] == 'UPSTREAM_UNAVAILABLE', (
                unavailable)
            missing_key = request('POST', maintenance_path,
                                  {'expected_version': '5'}, token=token, key=False)
            assert missing_key[0] == 400 and missing_key[1]['code'] == 'INVALID_ARGUMENT', (
                missing_key)
            malformed = request('POST', maintenance_path, {'grace_ms': 500}, token=token)
            assert malformed[0] == 400 and malformed[1]['code'] == 'INVALID_ARGUMENT', malformed
            bad_host = request('GET', '/api/v1/hosts/not-a-uuid/maintenance', token=token)
            assert bad_host[0] == 400 and bad_host[1]['code'] == 'INVALID_ARGUMENT', bad_host

            # 4) Explicit resume is a separate operation; completed snapshot maps to 200.
            status, body = request('POST', '/api/v1/hosts/' + HOST_ID + '/resume',
                                   {'expected_version': '5'}, token=token)
            assert status == 200, (status, body)
            assert body['maintenance']['admission'] == 'HOST_ADMISSION_OPEN', body
            assert body['operation']['state'] == 'OPERATION_SUCCEEDED', body
            assert body['operation']['execution_outcome'] == 'EXECUTION_KNOWN_SUCCEEDED', body

            # 5) Host operation query routes only under exactly one RESOURCE_HOST scope.
            status, body = request(
                'GET', '/api/v1/operations/host/' + CANCEL_OPERATION + QUERY, token=token)
            assert status == 200, (status, body)
            assert body['operation']['ref']['owner_service'] == 'host', body
            assert body['operation']['ref']['resource']['id'] == HOST_ID, body
            assert body['operation']['state'] == 'OPERATION_BLOCKED', body
            wrong_scope = request(
                'GET', '/api/v1/operations/host/' + CANCEL_OPERATION +
                '?scope=' + urllib.parse.quote(json.dumps(
                    {'resources': [{'kind': 'RESOURCE_ACCOUNT',
                                    'id': '11111111-1111-4111-8111-111111111111'}]})),
                token=token)
            assert wrong_scope[0] == 400 and wrong_scope[1]['code'] == 'INVALID_ARGUMENT', (
                wrong_scope)
            unknown_owner = request(
                'GET', '/api/v1/operations/unknown/' + CANCEL_OPERATION + QUERY, token=token)
            assert unknown_owner[0] == 400 and unknown_owner[1]['code'] == 'INVALID_ARGUMENT', (
                unknown_owner)

            # 6) Cancellation requires the explicit expected version and host scope body.
            cancel_path = '/api/v1/operations/host/' + CANCEL_OPERATION + '/cancel'
            status, body = request('POST', cancel_path,
                                   {'expected_version': '2',
                                    'scope': {'resources': [{'kind': 'RESOURCE_HOST',
                                                             'id': HOST_ID}]}},
                                   token=token)
            assert status == 200, (status, body)
            assert body['operation']['state'] == 'OPERATION_CANCELLED', body
            bad_cancel_scope = request('POST', cancel_path,
                                       {'expected_version': '2',
                                        'scope': {'resources': [{'kind': 'RESOURCE_ACCOUNT',
                                                                 'id': '11111111-1111-4111-8111-111111111111'}]}},
                                       token=token)
            assert bad_cancel_scope[0] == 400 and \
                bad_cancel_scope[1]['code'] == 'INVALID_ARGUMENT', bad_cancel_scope
            missing_cancel_scope = request('POST', cancel_path, {'expected_version': '2'},
                                           token=token)
            assert missing_cancel_scope[0] == 400 and \
                missing_cancel_scope[1]['code'] == 'INVALID_ARGUMENT', missing_cancel_scope
            # Multi-host fixed offline registry: Control routes each request by the host id in
            # the path/operation ref. The registered owner endpoint is reached; an endpoint that
            # is down and a host that is not registered both fail closed (503) and never leak a
            # different host's snapshot.
            import socket
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                dead_port = reservation.getsockname()[1]
            second_host = '55555555-5555-4555-8555-555555555555'
            multi = json.loads(json.dumps(config))
            multi['control_api'].pop('host_endpoint', None)
            multi['control_api']['host_endpoints'] = {
                HOST_ID: f'localhost:{owner_port}',
                second_host: f'localhost:{dead_port}',
            }
            multi['probe_listen'] = '127.0.0.1:0'
            multi_path = root / 'control-multi.json'
            multi_path.write_text(json.dumps(multi))
            multi_child = subprocess.Popen([control, '--config', str(multi_path)],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           text=True)
            try:
                first_line = line(multi_child)
                assert first_line.startswith('control_https_port='), first_line
                multi_port = int(first_line.split('=')[1])
                line(multi_child)

                def multi_request(method, target):
                    connection = client.HTTPSConnection('localhost', multi_port,
                                                       context=context, timeout=10)
                    headers = {'Authorization': 'Bearer ' + token}
                    try:
                        connection.request(method, target, None, headers)
                        response = connection.getresponse()
                        content = response.read()
                        return response.status, json.loads(content) if content else None
                    finally:
                        connection.close()

                status, multi_tokens = None, None
                connection = client.HTTPSConnection('localhost', multi_port, context=context,
                                                    timeout=10)
                try:
                    connection.request('POST', '/api/v1/auth/login',
                                       json.dumps({'username': 'alice',
                                                   'password': 'never-print-password'}),
                                       {'Content-Type': 'application/json',
                                        'Idempotency-Key': KEY})
                    response = connection.getresponse()
                    multi_tokens = json.loads(response.read() or b'{}')
                    status = response.status
                finally:
                    connection.close()
                if status != 200:
                    raise RuntimeError('multi-host control login: ' + str((status, multi_tokens)))
                token = multi_tokens['access_token']
                routed = multi_request('GET',
                                       '/api/v1/hosts/' + HOST_ID + '/maintenance')
                assert routed[0] == 200 and routed[1]['maintenance']['host_id'] == HOST_ID, routed
                down = multi_request('GET',
                                     '/api/v1/hosts/' + second_host + '/maintenance')
                assert down[0] == 503 and down[1]['code'] == 'UPSTREAM_UNAVAILABLE', down
                unregistered = multi_request(
                    'GET', '/api/v1/hosts/66666666-6666-4666-8666-666666666666/maintenance')
                assert unregistered[0] == 503 and \
                    unregistered[1]['code'] == 'UPSTREAM_UNAVAILABLE', unregistered
                operation_down = multi_request(
                    'GET', '/api/v1/operations/host/' + CANCEL_OPERATION +
                    '?scope=' + urllib.parse.quote(json.dumps(
                        {'resources': [{'kind': 'RESOURCE_HOST', 'id': second_host}]})))
                assert operation_down[0] == 503 and \
                    operation_down[1]['code'] == 'UPSTREAM_UNAVAILABLE', operation_down
            finally:
                stop(multi_child)

        finally:
            logs = ''
            for process in (child, owner, identity):
                if process is not None:
                    logs += (stop(process)[1] or '')[-400:]
            if sys.exc_info()[0] is not None and logs:
                sys.stderr.write(logs + '\n')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('usage: p12_host_control_rest.py <control> <identity-fixture> <owner-fixture>')
    main(sys.argv[1], sys.argv[2], sys.argv[3])
