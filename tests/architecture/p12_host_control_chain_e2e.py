#!/usr/bin/env python3
"""HOST-04 single-chain acceptance: real Control -> real Host maintenance owner RPC.

Chain under test (one process chain, no fake HostManagement):
  real Control HTTPS/mTLS -> Identity fixture (Login + IssueDelegation + CheckAuthorization)
  -> Control owner gateway host routes -> real plugin-host HostManagementService (mTLS)
  -> Host SQLite maintenance operation/admission/gate, with a real-TLS fake Runtime providing
  the HostBridge registration/fence/apply path and the Runtime SetHostAdmissionGate ACK.

It proves BeginMaintenance 202 -> local admission closed -> gate ACK -> known stop ->
SUCCEEDED, explicit Resume 200, owner GetOperation projection, cancel window, and stable error
mapping (missing endpoint 503 / unknown owner 400 / cross-host 404 / bad scope 400). Control
never fabricates progress: every claimed state is compared with Host SQLite and the fake gate
ledger. Identity is a real-TLS fixture (structural delegation wire + online CheckAuthorization),
not a real PG Identity; that remains an explicit gap in the acceptance report.
"""
from __future__ import annotations

import json
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import uuid
from http import client
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tests/architecture'))
sys.path.insert(0, str(REPO / 'core/tests/platform'))
from concurrent.futures import ThreadPoolExecutor  # noqa: E402

import grpc  # noqa: E402

import host_management_e2e as hm  # noqa: E402
from service_config_test import certificates  # noqa: E402
from host_management_e2e import HOST_ID  # noqa: E402
from kiarabot.v1 import host_pb2_grpc, runtime_pb2_grpc  # noqa: E402

ACCESS = 'secret-access-credential-login'
KEY = 'client-retry-key-0001'
CROSS_HOST = '99999999-9999-4999-8999-999999999999'
OPERATION = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'


def line(child):
    deadline = time.monotonic() + 15
    value = b''
    while not value.endswith(b'\n'):
        import os
        import select
        remaining = deadline - time.monotonic()
        assert remaining > 0 and select.select([child.stdout], [], [], remaining)[0], \
            'process startup timeout: ' + child.stderr.read()[-400:]
        byte = os.read(child.stdout.fileno(), 1)
        assert byte, child.stderr.read()[-400:]
        value += byte
    return value.decode().strip()


def require(condition, message):
    if not condition:
        raise RuntimeError('P12 host chain assertion: ' + message)


def start_identity(fixture, root):
    process = subprocess.Popen(
        [fixture, str(root / 'ca.pem'), str(root / 'server.pem'), str(root / 'server.key')],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return process, int(line(process))


def start_fake_runtime(root):
    credentials = grpc.ssl_server_credentials(
        [((root / 'server.key').read_bytes(), (root / 'server.pem').read_bytes())],
        root_certificates=(root / 'ca.pem').read_bytes(), require_client_auth=True)
    fake = hm.FakeRuntime()
    server = grpc.server(ThreadPoolExecutor(max_workers=8))
    runtime_pb2_grpc.add_RuntimeServiceServicer_to_server(fake, server)
    host_pb2_grpc.add_HostBridgeServicer_to_server(fake, server)
    import socket
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    server.add_secure_port(f'127.0.0.1:{port}', credentials)
    server.start()
    return server, port, fake


def tls_files(root):
    return {'certificate_file': str(root / 'server.pem'),
            'private_key_file': str(root / 'server.key'),
            'ca_file': str(root / 'ca.pem')}


def host_config(root, identity_port, runtime_port):
    return {
        'schema_version': 1, 'service': 'plugin-host',
        'probe_listen': '127.0.0.1:0', 'grpc_listen': '127.0.0.1:0',
        'tls': tls_files(root),
        'identity_endpoint': f'127.0.0.1:{identity_port}',
        'host': {
            'host_id': HOST_ID,
            'sqlite_path': str(root / 'host.sqlite'),
            'runtime_endpoint': f'127.0.0.1:{runtime_port}',
            'credential_directory': str(root),
            'packages': [{'plugin_type_id': 'fixture', 'root': str(root),
                          'sha256': 'aa' * 32, 'launch_profile': 'fixture',
                          'allowed_argument_names': []}],
            'profiles': [{'name': 'fixture', 'executable': '/bin/sh',
                          'fixed_arguments': ['sh', '-c',
                                              "trap '' TERM; while true; do sleep 1; done"],
                          'allowed_environment_names': [], 'credential_environment': None}],
            'environment': {},
        },
    }


def control_config(root, identity_port, host_port):
    return {
        'schema_version': 1, 'service': 'control',
        'probe_listen': '127.0.0.1:0',
        'tls': tls_files(root),
        'control_api': {
            'listen': '127.0.0.1:0',
            'identity_endpoint': f'127.0.0.1:{identity_port}',
            'host_endpoint': f'127.0.0.1:{host_port}',
        },
    }


def start_process(binary, config_path, root, log_name):
    child = subprocess.Popen([binary, '--config', str(config_path)], stdout=subprocess.PIPE,
                             stderr=open(root / log_name, 'w'), text=True)
    first = line(child)
    return child, first


def start_control(binary, config, root, name):
    path = root / f'{name}.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    child, first = start_process(binary, path, root, f'{name}.err')
    require(first.startswith('control_https_port='), f'{name} startup: ' + first)
    port = int(first.split('=')[1])
    line(child)  # ready/degraded state; requests retry below
    return child, port


class Rest:
    def __init__(self, port, ca):
        self.port = port
        self.context = ssl.create_default_context(cafile=str(ca))
        self.token = None

    def request(self, method, target, body=None, key=True, token=True, idem=None):
        connection = client.HTTPSConnection('localhost', self.port, context=self.context,
                                            timeout=10)
        headers = {}
        if token and self.token:
            headers['Authorization'] = 'Bearer ' + self.token
        if body is not None:
            body = json.dumps(body)
            headers['Content-Type'] = 'application/json'
            if key:
                headers['Idempotency-Key'] = idem or ('host-chain-' + str(uuid.uuid4()))
        try:
            connection.request(method, target, body, headers)
            response = connection.getresponse()
            content = response.read()
            parsed = json.loads(content) if content else None
            return response.status, parsed
        finally:
            connection.close()

    def login(self):
        deadline = time.monotonic() + 15
        while True:
            status, tokens = self.request('POST', '/api/v1/auth/login',
                                          {'username': 'alice',
                                           'password': 'never-print-password'},
                                          token=False, idem=KEY)
            if status == 200 and tokens and tokens.get('access_token') == ACCESS:
                self.token = tokens['access_token']
                return
            require(status in (503, 401) and time.monotonic() < deadline,
                    'control login: ' + str((status, tokens))[:200])
            time.sleep(0.1)

    def wait_ready(self):
        deadline = time.monotonic() + 10
        while True:
            status, body = self.request('GET', '/api/v1/health')
            if status == 200:
                return
            require(time.monotonic() < deadline, 'control health: ' + str((status, body))[:200])
            time.sleep(0.05)


def sqlite_rows(path, statement):
    import sqlite3
    connection = sqlite3.connect(str(path))
    try:
        return connection.execute(statement).fetchall()
    finally:
        connection.close()


def maintenance_state(root):
    rows = sqlite_rows(root / 'host.sqlite',
                       'SELECT admission,gate,version FROM host_maintenance_state')
    require(len(rows) == 1, 'host maintenance state row')
    return {'admission': rows[0][0], 'gate': rows[0][1], 'version': rows[0][2]}


# Host SQLite owns the operation payload in its own compact JSON shape (domain enum numbers),
# while Control projects the frozen wire shape; the e2e compares both representations.
OPERATION_STATES = {0: 'OPERATION_ACCEPTED', 1: 'OPERATION_RUNNING', 2: 'OPERATION_BLOCKED',
                    3: 'OPERATION_SUCCEEDED', 4: 'OPERATION_FAILED',
                    5: 'OPERATION_CANCELLED', 6: 'OPERATION_EXPIRED'}


def operations(root):
    rows = sqlite_rows(root / 'host.sqlite',
                       'SELECT payload FROM host_maintenance_operations')
    result = {}
    for (payload,) in rows:
        value = json.loads(payload)
        operation = value['operation']
        result[operation['id']] = {'id': operation['id'], 'host': operation['host'],
                                   'state': OPERATION_STATES.get(operation['state'], 'UNKNOWN'),
                                   'intent': value.get('intent', {}),
                                   'phase': value.get('phase'), 'gate': value.get('gate_intent')}
    return result


def wait_maintenance(stub, root, deadline, predicate, label, timeout=20.0):
    limit = time.monotonic() + timeout
    state = None
    while time.monotonic() < limit:
        status, body = stub.request('GET', '/api/v1/hosts/' + HOST_ID + '/maintenance')
        require(status == 200, label + ' maintenance GET: ' + str((status, body))[:200])
        state = body['maintenance']
        if predicate(state):
            return state
        time.sleep(0.1)
    raise RuntimeError('P12 host chain assertion: timeout ' + label + ' ' + str(state))


def wait_operation(stub, operation_id, predicate, label, timeout=25.0):
    limit = time.monotonic() + timeout
    scope = urllib.parse.quote(json.dumps(
        {'resources': [{'kind': 'RESOURCE_HOST', 'id': HOST_ID}]}))
    target = '/api/v1/operations/host/' + operation_id + '?scope=' + scope
    current = None
    while time.monotonic() < limit:
        status, body = stub.request('GET', target)
        require(status == 200, label + ' operation GET: ' + str((status, body))[:200])
        current = body['operation']
        if predicate(current):
            return current
        time.sleep(0.1)
    raise RuntimeError('P12 host chain assertion: timeout ' + label + ' ' + str(current)[:300])


def main(control, identity_fixture, plugin_host):
    with tempfile.TemporaryDirectory(prefix='p12-host-chain-') as folder:
        root = Path(folder)
        certificates(root, 'ec')
        children = {}
        fake_server = None
        try:
            identity, identity_port = start_identity(identity_fixture, root)
            children['identity'] = identity
            fake_server, runtime_port, fake = start_fake_runtime(root)

            host_path = root / 'host.json'
            host_path.write_text(json.dumps(host_config(root, identity_port, runtime_port)))
            host_path.chmod(0o600)
            host, host_first = start_process(plugin_host, host_path, root, 'host.err')
            children['host'] = host
            require('service=plugin-host' in host_first, 'host startup: ' + host_first)
            host_fields = dict(item.partition('=')[::2] for item in host_first.split())
            host_port = int(host_fields['grpc_port'])

            control_process, control_port = start_control(
                control, control_config(root, identity_port, host_port), root, 'control')
            children['control'] = control_process
            stub = Rest(control_port, root / 'ca.pem')
            stub.login()
            stub.wait_ready()

            # Real Host registration + apply over the fake Runtime bridge must be live before
            # maintenance can prove a known stop of the finite execution set.
            require(fake.connected.wait(10.0), 'fake Runtime HostBridge registration')
            require(fake.apply_sent.wait(10.0), 'fake Runtime apply command')
            deadline = time.monotonic() + 10
            while not fake.receipts and time.monotonic() < deadline:
                time.sleep(0.05)
            require(any(receipt.persisted for receipt in fake.receipts),
                    'managed execution receipt persisted')
            wait_maintenance(stub, root, deadline,
                             lambda value: value['admission'] == 'HOST_ADMISSION_OPEN',
                             'initial admission open')

            # 1) BeginMaintenance through Control: owner persists local closed admission, asks the
            # Runtime gate, stops the managed execution and only then reports SUCCEEDED.
            status, body = stub.request('POST', '/api/v1/hosts/' + HOST_ID + '/maintenance',
                                        {'expected_version': int(
                                            wait_maintenance(
                                                stub, root, None,
                                                lambda value: True,
                                                'read version')['version']),
                                         'grace_ms': 1000})
            require(status == 202, 'begin maintenance 202: ' + str((status, body))[:300])
            begin_operation = body['operation']
            operation_id = begin_operation['ref']['operation_id']
            require(body['maintenance']['host_id'] == HOST_ID,
                    'begin projects the owner maintenance record')
            require(begin_operation['state'] == 'OPERATION_ACCEPTED',
                    'begin is accepted, never completion')
            # The owner reports Accepted first; the local admission close happens on its own
            # advancement path and must be observable before the gate result is claimed.
            wait_maintenance(stub, root, None,
                             lambda value: value['admission'] == 'HOST_ADMISSION_CLOSED',
                             'begin closes admission')
            finished = wait_operation(stub, operation_id,
                                      lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                                      'begin maintenance SUCCEEDED')
            require(finished['execution_outcome'] in ('EXECUTION_KNOWN_SUCCEEDED',
                                                      'EXECUTION_NOT_STARTED'),
                    'begin execution outcome known: ' + str(finished['execution_outcome']))
            gate_after_begin = maintenance_state(root)
            require(gate_after_begin['admission'] == 0, 'host SQLite admission closed')
            require(gate_after_begin['gate'] == 1, 'host SQLite gate version recorded')
            require(operation_id in operations(root), 'host SQLite operation persisted')
            require(operations(root)[operation_id]['state'] == 'OPERATION_SUCCEEDED',
                    'host SQLite operation SUCCEEDED')
            require(len(fake.gate_calls) >= 1, 'Runtime gate called from real Host')
            gate_request = fake.gate_calls[-1]
            require(gate_request.host_id == HOST_ID and
                    gate_request.fence.instance_id == HOST_ID and
                    gate_request.fence.connection_generation == 3, 'gate carries live fence')
            require(gate_request.maintenance_version > 0, 'gate carries durable intent version')
            require(gate_request.admission == 2, 'begin gate closes admission')  # 2 == CLOSED
            projected = wait_maintenance(stub, root, None,
                                         lambda value: value['admission'] ==
                                         'HOST_ADMISSION_CLOSED', 'control admission closed')
            require(int(projected['version']) == gate_after_begin['version'],
                    'Control maintenance version matches Host SQLite: control=' +
                    str(projected['version']) + ' sqlite=' + str(gate_after_begin['version']))

            # 2) Explicit Resume is a new operation that reopens admission.
            resume_status, resume_body = stub.request(
                'POST', '/api/v1/hosts/' + HOST_ID + '/resume',
                {'expected_version': int(projected['version'])})
            require(resume_status in (200, 202), 'resume accepted: ' +
                    str((resume_status, resume_body))[:300])
            resume_id = resume_body['operation']['ref']['operation_id']
            resumed = wait_operation(stub, resume_id,
                                     lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                                     'resume SUCCEEDED')
            require(resumed['state'] == 'OPERATION_SUCCEEDED', 'resume owner snapshot')
            reopened = wait_maintenance(stub, root, None,
                                        lambda value: value['admission'] == 'HOST_ADMISSION_OPEN',
                                        'admission reopened')
            require(reopened['admission'] == 'HOST_ADMISSION_OPEN', 'Control sees admission open')
            require(maintenance_state(root)['admission'] == 1, 'Host SQLite admission open')
            # GetOperation is served by the real Host owner (not the Runtime projection).
            scope = urllib.parse.quote(json.dumps(
                {'resources': [{'kind': 'RESOURCE_HOST', 'id': HOST_ID}]}))
            status, operation_body = stub.request(
                'GET', '/api/v1/operations/host/' + resume_id + '?scope=' + scope)
            require(status == 200 and operation_body['operation']['state'] ==
                    'OPERATION_SUCCEEDED', 'host owner GetOperation projection')
            require(operation_body['operation']['ref']['resource']['id'] == HOST_ID,
                    'owner operation carries the stored host resource')

            # 3) Cancel window: only an operation still in Accepted may be cancelled; later
            # cancellation is CONFLICT and never rolls back stopped executions.
            cancelled_id = None
            for _ in range(8):
                version = int(wait_maintenance(stub, root, None,
                                               lambda value: True, 'cancel version')['version'])
                status, body = stub.request('POST', '/api/v1/hosts/' + HOST_ID + '/maintenance',
                                            {'expected_version': version, 'grace_ms': 500})
                if status == 409:
                    time.sleep(0.3)
                    continue
                require(status == 202, 'cancel-window begin 202: ' + str((status, body))[:200])
                candidate = body['operation']['ref']['operation_id']
                cancel_status, cancel_body = stub.request(
                    'POST', '/api/v1/operations/host/' + candidate + '/cancel',
                    {'expected_version': '1',
                     'scope': {'resources': [{'kind': 'RESOURCE_HOST', 'id': HOST_ID}]}})
                if cancel_status == 200 and cancel_body['operation']['state'] == \
                        'OPERATION_CANCELLED':
                    cancelled_id = candidate
                    break
                require(cancel_status == 409, 'late cancel is CONFLICT: ' +
                        str((cancel_status, cancel_body))[:200])
                target = '/api/v1/operations/host/' + candidate + '?scope=' + scope
                limit = time.monotonic() + 20
                while time.monotonic() < limit:
                    read_status, read_body = stub.request('GET', target)
                    if read_status == 200 and read_body['operation']['state'] in (
                            'OPERATION_SUCCEEDED', 'OPERATION_BLOCKED'):
                        break
                    time.sleep(0.1)
                if read_body['operation']['state'] == 'OPERATION_SUCCEEDED':
                    stub.request('POST', '/api/v1/hosts/' + HOST_ID + '/resume',
                                 {'expected_version': int(wait_maintenance(
                                     stub, root, None, lambda value: True,
                                     'resume after conflict')['version'])})
                    wait_maintenance(stub, root, None,
                                     lambda value: value['admission'] == 'HOST_ADMISSION_OPEN',
                                     'reopen after cancel conflict')
            require(cancelled_id is not None, 'a just-accepted maintenance operation was cancelled')
            operations_after_cancel = operations(root)
            require(operations_after_cancel[cancelled_id]['state'] == 'OPERATION_CANCELLED',
                    'Host SQLite records cancellation')
            reopened = wait_maintenance(stub, root, None,
                                        lambda value: value['admission'] == 'HOST_ADMISSION_OPEN',
                                        'cancel leaves admission open')
            require(reopened['admission'] == 'HOST_ADMISSION_OPEN',
                    'cancel pre-effect leaves admission open')

            # 4) Stable error mapping through the real Control -> real Host chain.
            wrong_scope = stub.request(
                'GET', '/api/v1/operations/host/' + resume_id +
                '?scope=' + urllib.parse.quote(json.dumps(
                    {'resources': [{'kind': 'RESOURCE_ACCOUNT',
                                    'id': '11111111-1111-4111-8111-111111111111'}]})))
            require(wrong_scope[0] == 400 and wrong_scope[1]['code'] == 'INVALID_ARGUMENT',
                    'bad host scope 400')
            unknown_owner = stub.request('GET', '/api/v1/operations/unknown/' + resume_id)
            require(unknown_owner[0] == 400 and unknown_owner[1]['code'] == 'INVALID_ARGUMENT',
                    'unknown owner 400')
            cross_host = stub.request(
                'GET', '/api/v1/operations/host/' + resume_id +
                '?scope=' + urllib.parse.quote(json.dumps(
                    {'resources': [{'kind': 'RESOURCE_HOST', 'id': CROSS_HOST}]})))
            require(cross_host[0] == 404 and cross_host[1]['code'] == 'NOT_FOUND',
                    'cross-host visibility 404: ' + str(cross_host)[:200])
            bad_host = stub.request('GET', '/api/v1/hosts/not-a-uuid/maintenance')
            require(bad_host[0] == 400 and bad_host[1]['code'] == 'INVALID_ARGUMENT',
                    'malformed host id 400')

            # 5) Runtime gate outage: the owner must stay BLOCKED, never fake success, and a
            # later explicit resume must converge after the gate recovers.
            fake.gate_mode = 'unavailable'
            blocked_version = int(wait_maintenance(stub, root, None,
                                                   lambda value: True, 'blocked version')['version'])
            blocked_status, blocked_body = stub.request(
                'POST', '/api/v1/hosts/' + HOST_ID + '/maintenance',
                {'expected_version': blocked_version, 'grace_ms': 500})
            require(blocked_status == 202, 'gate-outage begin 202')
            blocked_id = blocked_body['operation']['ref']['operation_id']
            blocked = wait_operation(stub, blocked_id,
                                     lambda value: value['state'] == 'OPERATION_BLOCKED',
                                     'gate outage BLOCKED')
            require(blocked['state'] == 'OPERATION_BLOCKED' and
                    blocked['execution_outcome'] != 'EXECUTION_KNOWN_SUCCEEDED',
                    'gate outage never becomes a false success')
            require(maintenance_state(root)['admission'] == 0,
                    'Host SQLite keeps local admission closed while gate is unresolved')
            # The owner retries the same operation after the gate recovers; a blocked operation is
            # not terminal and is never rewritten as success by Control.
            fake.gate_mode = 'ok'
            recovered_operation = wait_operation(stub, blocked_id,
                                                 lambda value: value['state'] ==
                                                 'OPERATION_SUCCEEDED',
                                                 'blocked operation converges after gate recovery',
                                                 timeout=30)
            require(recovered_operation['state'] == 'OPERATION_SUCCEEDED',
                    'gate outage operation converges only after real gate recovery')
            require(operations(root)[blocked_id]['state'] == 'OPERATION_SUCCEEDED',
                    'Host SQLite converges the same operation identity')
            reopened_after_outage = wait_maintenance(
                stub, root, None, lambda value: True, 'read post-outage version')
            reopen = stub.request(
                'POST', '/api/v1/hosts/' + HOST_ID + '/resume',
                {'expected_version': int(reopened_after_outage['version'])})
            require(reopen[0] in (200, 202), 'post-outage resume accepted: ' +
                    str(reopen)[:200])
            wait_operation(stub, reopen[1]['operation']['ref']['operation_id'],
                           lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                           'post-outage resume SUCCEEDED')
            wait_maintenance(stub, root, None,
                             lambda value: value['admission'] == 'HOST_ADMISSION_OPEN',
                             'post-outage admission open')

            # 6) No registered host endpoint: Control must answer 503 and never an empty success.
            no_host_process, no_host_port = start_control(
                control, {'schema_version': 1, 'service': 'control',
                          'probe_listen': '127.0.0.1:0', 'tls': tls_files(root),
                          'control_api': {'listen': '127.0.0.1:0',
                                          'identity_endpoint': f'127.0.0.1:{identity_port}'}},
                root, 'control-no-host')
            children['control-no-host'] = no_host_process
            unavailability = Rest(no_host_port, root / 'ca.pem')
            unavailability.login()
            unavailability.wait_ready()
            missing = unavailability.request('GET',
                                             '/api/v1/hosts/' + HOST_ID + '/maintenance')
            require(missing[0] == 503 and missing[1]['code'] == 'UPSTREAM_UNAVAILABLE',
                    'missing host endpoint 503: ' + str(missing)[:200])
            print('P12 host control chain e2e passed: ' + operation_id, flush=True)
        finally:
            logs = []
            for name, child in reversed(list(children.items())):
                if child is not None:
                    try:
                        child.send_signal(2)
                    except Exception:
                        pass
                    try:
                        child.wait(timeout=15)
                    except Exception:
                        child.kill()
                    for path in (root / f'{name}.err', root / 'host.err', root / 'control.err'):
                        if path.exists():
                            logs.append(path.name + ':\n' + path.read_text()[-800:])
            if fake_server is not None:
                fake_server.stop(0)
            if sys.exc_info()[0] is not None:
                for entry in logs[:3]:
                    sys.stderr.write(entry + '\n')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(
            'usage: p12_host_control_chain_e2e.py <control> <identity-fixture> <plugin-host>')
    main(sys.argv[1], sys.argv[2], sys.argv[3])
