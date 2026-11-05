#!/usr/bin/env python3
"""HOST-04 all-real single chain: real Identity + real Runtime gate + real Host + real Control.

Owned-fixture driver supplies PG16.4, initializer and binaries. Offline bootstrap enrolls the
Identity administrator, Control platform peer, Runtime platform peer and the v1.1 Host peers
(host-1 has the exact host.execute grant, host-2 only host.read). Then real processes:
  real Control HTTPS -> real Identity delegation -> real plugin-host HostManagementService
  -> HostBridge mTLS -> real Runtime SetHostAdmissionGate -> Identity.CheckHostGateAuthorization
  -> Runtime PG host_admission_gates -> Host SQLite maintenance/operation/admission.

Asserts Begin 202 -> local admission closed -> real gate ACK -> known stop set -> SUCCEEDED,
Resume, owner GetOperation, cancel window, Runtime gate outage BLOCKED without fake success
(and recovery after Runtime restart), Host restart persistence, Identity negative decision for a
host without host.execute, and cross-checks Runtime PG / Host SQLite / Control projections.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import signal
import ssl
import subprocess
import sys
import time
import traceback
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
sys.path.insert(0, str(REPO / 'tests/architecture'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402
import p14_native_single_host as p14  # noqa: E402

STATE: dict = {}
SECOND_HOST = str(uuid.uuid4())


class Failure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise Failure(label)


def stage(name):
    print('P12H stage: ' + name, file=sys.stderr, flush=True)


def write_json(path: Path, value):
    path.write_text(json.dumps(value))
    path.chmod(0o600)


def prepare(root: Path, enrolled: dict):
    """Offline enrollment: admin host grants, second host scope, Control-delegation bootstrap."""
    host = bootstrap_fixture.HOST_STATE['host-1']
    enrolled['administrator_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['host.read', 'host.execute'],
         'scope': [{'kind': 'RESOURCE_HOST', 'id': host['host_id']}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['host.read', 'host.execute'],
         'scope': [{'kind': 'RESOURCE_HOST', 'id': SECOND_HOST}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_HOST', 'id': host['host_id']}]},
    ])
    # A Host owner calls Identity.CheckAuthorization as itself for every owner RPC, so the Host
    # platform peer needs the same consumer-wide identity.authorization.check grant a Runtime/
    # Scheduler/Model peer carries; the gate path separately requires its exact host.execute.
    for peer in enrolled['platform_peers']:
        if peer['service'] != 'host':
            continue
        peer['assignments'].append({
            'assignment_id': str(uuid.uuid4()),
            'permissions': ['identity.authorization.check'],
            'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': peer['principal_id']}]})
    STATE['host_principal'] = host['principal_id']


def pem_fingerprint(path: Path) -> bytes:
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(path.read_text())).digest()


def stop(child):
    if child is None:
        return
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
    try:
        child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait(timeout=5)


def start_process(binary, config, root, name, timeout=30.0):
    path = root / f'{name}.json'
    write_json(path, config)
    child = subprocess.Popen([str(binary), '--config', str(path)],
                             stdout=subprocess.PIPE, stderr=open(root / f'{name}.err', 'w'))
    first = p14.line(child, timeout)
    fields = dict(item.split('=', 1) for item in first.split() if '=' in item)
    return child, fields


def wait_until(predicate, label, timeout=30.0):
    deadline = time.monotonic() + timeout
    value = None
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise Failure('timeout: ' + label + ' last=' + str(value)[:300])


def sqlite_rows(root, query):
    import sqlite3
    with sqlite3.connect(str(root / 'host.sqlite')) as database:
        return database.execute(query).fetchall()


def runtime_sql(root, pg_bin, port, query):
    environment, pg = rt.postgres(root, pg_bin, port, 'p03_runtime')
    return rt.run_sql((environment, pg), query)


OPERATION_STATES = {0: 'OPERATION_ACCEPTED', 1: 'OPERATION_RUNNING', 2: 'OPERATION_BLOCKED',
                    3: 'OPERATION_SUCCEEDED', 4: 'OPERATION_FAILED', 5: 'OPERATION_CANCELLED',
                    6: 'OPERATION_EXPIRED'}


def host_state(root, sqlite_path=None):
    path = Path(sqlite_path) if sqlite_path else root / 'host.sqlite'
    import sqlite3
    with sqlite3.connect(str(path)) as database:
        row = database.execute(
            'SELECT admission,gate,version FROM host_maintenance_state').fetchone()
        require(row is not None, 'host maintenance state row')
        operations = {}
        for (payload,) in database.execute(
                'SELECT payload FROM host_maintenance_operations'):
            value = json.loads(payload)
            operation = value['operation']
            operations[operation['id']] = {
                'id': operation['id'], 'host': operation['host'],
                'state': OPERATION_STATES.get(operation['state'], 'UNKNOWN')}
    return {'admission': row[0], 'gate': row[1], 'version': row[2], 'operations': operations}


def runtime_gate_row(root, pg_bin, port, host_id):
    rows = runtime_sql(root, pg_bin, port,
                       "SELECT host_id,maintenance_version,gate_version,admission,operation_id "
                       "FROM kiarabot.host_admission_gates WHERE host_id='" + host_id + "'")
    if not rows:
        return None
    fields = rows.split('|')
    return {'host_id': fields[0], 'maintenance_version': int(fields[1]),
            'gate_version': int(fields[2]), 'admission': int(fields[3]),
            'operation_id': fields[4]}


class ControlRest:
    def __init__(self, port, ca_path):
        self.port = port
        self.context = ssl.create_default_context(cafile=str(ca_path))
        self.token = None

    def request(self, method, target, body=None, key=True, token=True):
        import http.client
        connection = http.client.HTTPSConnection('localhost', self.port, context=self.context,
                                                 timeout=15)
        headers = {}
        if token and self.token:
            headers['Authorization'] = 'Bearer ' + self.token
        if body is not None:
            body = json.dumps(body)
            headers['Content-Type'] = 'application/json'
            if key:
                headers['Idempotency-Key'] = 'real-chain-' + str(uuid.uuid4())
        try:
            connection.request(method, target, body, headers)
            response = connection.getresponse()
            content = response.read()
            return response.status, json.loads(content) if content else None
        finally:
            connection.close()

    def login(self, root, deadline=30.0):
        import http.client
        password = (root / 'admin-password').read_text().strip()
        end = time.monotonic() + deadline
        while True:
            connection = http.client.HTTPSConnection('localhost', self.port,
                                                     context=self.context, timeout=15)
            try:
                connection.request('POST', '/api/v1/auth/login',
                                   json.dumps({'username': 'administrator',
                                               'password': password}),
                                   {'Content-Type': 'application/json',
                                    'Idempotency-Key': 'real-chain-login-0001'})
                response = connection.getresponse()
                body = json.loads(response.read() or b'{}')
            finally:
                connection.close()
            if response.status == 200 and body.get('access_token'):
                self.token = body['access_token']
                return
            require(response.status in (401, 503) and time.monotonic() < end,
                    'control login: ' + str((response.status, body))[:300])
            time.sleep(0.2)


def debug_direct_body(root, ca, host_port, host_id, token=''):
    """Test-only diagnostic: reproduce the exact Control -> Host delegation call and print the
    typed owner status instead of Control's redacted stable code."""
    import grpc
    from kiarabot.v1 import identity_pb2, identity_pb2_grpc, host_pb2, host_pb2_grpc
    from kiarabot.v1 import platform_pb2, policy_pb2
    from google.protobuf.timestamp_pb2 import Timestamp
    ca_data = Path(ca).read_bytes()
    control_chain = (Path(root) / 'bootstrap-tls' / 'control.pem').read_bytes()
    control_key = (Path(root) / 'bootstrap-tls' / 'control.key').read_bytes()
    identity_port = STATE['identity_port']
    password = (Path(root) / 'admin-password').read_text().strip()
    credentials = grpc.ssl_channel_credentials(
        root_certificates=ca_data, private_key=control_key, certificate_chain=control_chain)
    identity = identity_pb2_grpc.IdentityServiceStub(
        grpc.secure_channel(f'127.0.0.1:{identity_port}', credentials))
    deadline = Timestamp()
    deadline.FromNanoseconds(int((time.time() + 20) * 1_000_000_000))

    def context():
        return platform_pb2.RequestContext(request_id=str(uuid.uuid4()),
                                           trace_id=str(uuid.uuid4()), idempotency_key=str(uuid.uuid4()),
                                           deadline=deadline)

    if not token:
        login = identity.Login(identity_pb2.IdentityLoginRequest(
            context=context(), username='administrator', password=password), timeout=10)
        token = login.tokens.access_token
    scope = platform_pb2.ResourceScope(resources=[
        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_HOST, id=host_id)])
    issued = identity.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
        context=context(), subject_proof=token, audience='host', scope=scope,
        permissions=[policy_pb2.PERMISSION_HOST_READ],
        client_certificate_sha256=pem_fingerprint(Path(root) / 'bootstrap-tls' / 'control.pem')),
        timeout=10)
    print('P12H debug: issued host delegation len=' + str(len(issued.delegation.proof)),
          file=sys.stderr, flush=True)
    try:
        check = identity.CheckAuthorization(identity_pb2.IdentityCheckAuthorizationRequest(
            context=context(), subject_proof=issued.delegation.proof,
            full_method='kiarabot.v1.HostManagementService/GetMaintenance', scope=scope),
            timeout=10)
        print('P12H debug: identity control-caller check allowed=' + str(check.decision.allowed),
              file=sys.stderr, flush=True)
    except grpc.RpcError as error:
        print('P12H debug: identity control-caller error=' + str(error.code()) + ' ' +
              str(error.details())[:300], file=sys.stderr, flush=True)
    import base64
    segments = issued.delegation.proof.split('.')
    claims = json.loads(base64.urlsafe_b64decode(segments[1] + '=' * (-len(segments[1]) % 4)))
    print('P12H debug: claims authorization_version=' +
          str(claims.get('delegation', {}).get('authorization_version')),
          file=sys.stderr, flush=True)
    host_chain = (Path(root) / 'bootstrap-tls' / 'host-1.pem').read_bytes()
    host_key = (Path(root) / 'bootstrap-tls' / 'host-1.key').read_bytes()
    host_credentials = grpc.ssl_channel_credentials(
        root_certificates=ca_data, private_key=host_key, certificate_chain=host_chain)
    host_identity = identity_pb2_grpc.IdentityServiceStub(
        grpc.secure_channel(f'127.0.0.1:{identity_port}', host_credentials))
    try:
        check = host_identity.CheckAuthorization(
            identity_pb2.IdentityCheckAuthorizationRequest(
                context=context(), subject_proof=issued.delegation.proof,
                full_method='kiarabot.v1.HostManagementService/GetMaintenance', scope=scope),
            timeout=10)
        print('P12H debug: identity host-caller check allowed=' + str(check.decision.allowed),
              file=sys.stderr, flush=True)
    except grpc.RpcError as error:
        print('P12H debug: identity host-caller error=' + str(error.code()) + ' ' +
              str(error.details())[:300], file=sys.stderr, flush=True)
    try:
        check = host_identity.CheckAuthorization(
            identity_pb2.IdentityCheckAuthorizationRequest(
                context=context(), subject_proof=issued.delegation.proof,
                full_method='kiarabot.v1.HostManagementService/GetMaintenance', scope=scope,
                known_authorization_version=int(
                    claims.get('delegation', {}).get('authorization_version', '0'))),
            timeout=10)
        print('P12H debug: identity host-caller+known check allowed=' +
              str(check.decision.allowed), file=sys.stderr, flush=True)
    except grpc.RpcError as error:
        print('P12H debug: identity host-caller+known error=' + str(error.code()) + ' ' +
              str(error.details())[:300], file=sys.stderr, flush=True)
    execute_scope = platform_pb2.ResourceScope(resources=[
        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_HOST, id=host_id)])
    try:
        execute_issued = identity.IssueDelegation(
            identity_pb2.IdentityIssueDelegationRequest(
                context=context(), subject_proof=token, audience='host',
                scope=execute_scope, permissions=[policy_pb2.PERMISSION_HOST_EXECUTE],
                client_certificate_sha256=pem_fingerprint(
                    Path(root) / 'bootstrap-tls' / 'control.pem')), timeout=10)
        print('P12H debug: issued host.execute delegation len=' +
              str(len(execute_issued.delegation.proof)), file=sys.stderr, flush=True)
        execute_check = host_identity.CheckAuthorization(
            identity_pb2.IdentityCheckAuthorizationRequest(
                context=context(), subject_proof=execute_issued.delegation.proof,
                full_method='kiarabot.v1.HostManagementService/BeginMaintenance',
                scope=execute_scope), timeout=10)
        print('P12H debug: identity host.execute check allowed=' +
              str(execute_check.decision.allowed), file=sys.stderr, flush=True)
    except grpc.RpcError as error:
        print('P12H debug: host.execute hop error=' + str(error.code()) + ' ' +
              str(error.details())[:200], file=sys.stderr, flush=True)
    host = host_pb2_grpc.HostManagementServiceStub(
        grpc.secure_channel(f'127.0.0.1:{host_port}', credentials))
    request = host_pb2.HostGetMaintenanceRequest(
        context=platform_pb2.RequestContext(
            request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()), deadline=deadline,
            delegation=issued.delegation.proof), host_id=host_id)
    try:
        reply = host.GetMaintenance(request, timeout=10)
        print('P12H debug: direct owner ok version=' + str(reply.maintenance.version),
              file=sys.stderr, flush=True)
        try:
            begun = host.BeginMaintenance(
                host_pb2.HostBeginMaintenanceRequest(
                    context=platform_pb2.RequestContext(
                        request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()),
                        idempotency_key=str(uuid.uuid4()), deadline=deadline,
                        delegation=execute_issued.delegation.proof),
                    host_id=host_id, expected_version=reply.maintenance.version,
                    grace_ms=500), timeout=10)
            print('P12H debug: direct begin ok state=' +
                  str(begun.operation.state), file=sys.stderr, flush=True)
        except grpc.RpcError as error:
            print('P12H debug: direct begin error=' + str(error.code()) + ' ' +
                  str(error.details())[:300], file=sys.stderr, flush=True)
    except grpc.RpcError as error:
        print('P12H debug: direct owner error code=' + str(error.code()) +
              ' details=' + str(error.details())[:600], file=sys.stderr, flush=True)


def debug_direct_owner(root, ca, host_port, host_id, token=''):
    """Spawn the sdk venv python for the diagnostic because the owned-fixture driver runs this
    script with the system interpreter (stdlib-only chain, grpc lives in the venv)."""
    environment = dict(os.environ)
    environment['PYTHONPATH'] = str(REPO / 'sdk/python/build/generated')
    reply = subprocess.run(
        [str(REPO / 'sdk/python/.venv/bin/python'), str(Path(__file__).resolve()),
         '--debug-direct', str(root), str(ca), str(host_port), host_id, token],
        env=environment, capture_output=True, text=True, timeout=60)
    print(reply.stdout[-2000:] + reply.stderr[-2000:], file=sys.stderr, flush=True)


def wait_ready(probe_endpoint, timeout=30.0):
    require(rt.wait_ready(int(probe_endpoint.rsplit(':', 1)[1]), True, timeout),
            'probe readiness ' + probe_endpoint)


def wait_control_ready(control):
    end = time.monotonic() + 30
    while True:
        status, body = control.request('GET', '/api/v1/health')
        if status == 200:
            return
        require(time.monotonic() < end, 'control health: ' + str((status, body))[:300])
        time.sleep(0.2)


def verify_owned_root():
    """Reject a mismatched/disposable backend before any bootstrap DDL (owned-fixture driver)."""
    root = Path(sys.argv[1]).resolve()
    marker = root / 'p03-owned-marker'
    token = marker.read_text().strip() if marker.is_file() else ''
    valid = (root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid() and
             not root.is_symlink() and marker.is_file() and not marker.is_symlink() and
             len(token) == 64 and all(character in '0123456789abcdef' for character in token) and
             (root / 'data').is_dir() and not (root / 'data').is_symlink() and
             (root / 'data').resolve() == (root / 'data'))
    if not valid:
        print('fixture backend directory mismatch', file=sys.stderr, flush=True)
        raise SystemExit(1)
    if not (root / 'password').is_file():
        print('fixture backend directory mismatch', file=sys.stderr, flush=True)
        raise SystemExit(1)


def main():
    verify_owned_root()
    bootstrap_fixture.main(control_server_certificate=True, prepare=prepare,
                           after_bootstrap=after_bootstrap, host_enrollment=True)


def after_bootstrap(root: Path):
    stage('after bootstrap')
    port = int(sys.argv[2])
    initializer = str(Path(sys.argv[3]).resolve())
    bootstrap = Path(sys.argv[5]).resolve()
    pg_bin = Path(sys.argv[6]).resolve()
    identity_binary = Path(sys.argv[7]).resolve()
    runtime_binary = Path(sys.argv[8]).resolve()
    control_binary = Path(sys.argv[9]).resolve()
    host_binary = Path(sys.argv[10]).resolve()
    _ = bootstrap
    stage('create runtime database')
    runtime_database = p14.create_database(root, pg_bin, port, initializer, 'runtime')
    rt.identity_security_material(root)
    tls = root / 'bootstrap-tls'
    ca = tls / 'ca.pem'
    host = bootstrap_fixture.HOST_STATE['host-1']
    host_id = host['host_id']

    identity_config = json.loads((root / 'identity.json').read_text())
    identity_config['probe_listen'] = p14.free_endpoint()
    identity_config['grpc_listen'] = p14.free_endpoint()
    write_json(root / 'identity.json', identity_config)
    identity_endpoint = identity_config['grpc_listen']

    runtime_probe, runtime_grpc = p14.free_endpoint(), p14.free_endpoint()
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': runtime_probe, 'grpc_listen': runtime_grpc,
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': str(ca)},
        'database': runtime_database, 'identity_endpoint': identity_endpoint,
        'hosts': [{'host_id': host_id,
                   'certificate_sha256': pem_fingerprint(tls / 'host-1.pem').hex()},
                  {'host_id': bootstrap_fixture.HOST_STATE['host-2']['host_id'],
                   'certificate_sha256': pem_fingerprint(tls / 'host-2.pem').hex()}],
    }
    write_json(root / 'runtime.json', runtime_config)

    control_config = {
        'schema_version': 1, 'service': 'control', 'probe_listen': p14.free_endpoint(),
        'tls': {'certificate_file': str(tls / 'control.pem'),
                'private_key_file': str(tls / 'control.key'), 'ca_file': str(ca)},
        'control_api': {'listen': '127.0.0.1:0',
                        'identity_endpoint': identity_endpoint},
    }
    write_json(root / 'control-template.json', control_config)

    host_config = {
        'schema_version': 1, 'service': 'plugin-host',
        'probe_listen': '127.0.0.1:0', 'grpc_listen': '127.0.0.1:0',
        'tls': {'certificate_file': host['certificate_file'],
                'private_key_file': host['key_file'], 'ca_file': str(ca)},
        'identity_endpoint': identity_endpoint,
        'host': {
            'host_id': host_id,
            'sqlite_path': str(root / 'host.sqlite'),
            'runtime_endpoint': runtime_grpc,
            'credential_directory': str(root),
            'packages': [{'plugin_type_id': 'fixture', 'root': str(root),
                          'sha256': 'aa' * 32, 'launch_profile': 'fixture',
                          'allowed_argument_names': []}],
            'profiles': [{'name': 'fixture', 'executable': '/bin/sh',
                          'fixed_arguments': ['sh', '-c', 'true'],
                          'allowed_environment_names': [], 'credential_environment': None}],
            'environment': {},
        },
    }
    write_json(root / 'host.json', host_config)

    children = {}
    try:
        stage('identity start')
        identity = subprocess.Popen([str(identity_binary), '--config',
                                     str(root / 'identity.json')],
                                    stdout=subprocess.PIPE,
                                    stderr=open(root / 'identity.err', 'w'))
        children['identity'] = identity
        identity_state = {}
        for _ in range(2):
            text = p14.line(identity)
            identity_state.update(dict(item.split('=', 1) for item in text.split()
                                       if '=' in item))
            if 'ready' in identity_state:
                break
        require(identity_state.get('ready') == 'true', 'identity readiness: ' + str(identity_state))
        wait_ready(identity_config['probe_listen'])

        stage('runtime start')
        runtime, _ = p14.daemon(runtime_binary, root / 'runtime.json')
        children['runtime'] = runtime
        wait_ready(runtime_probe)

        stage('host start')
        host_process, host_fields = start_process(host_binary, host_config, root, 'host')
        children['host'] = host_process
        require(host_fields.get('grpc_port') and host_fields.get('ready') == 'false',
                'host startup: ' + str(host_fields))
        host_port = int(host_fields['grpc_port'])

        stage('control start')
        control_config['control_api']['host_endpoint'] = f'127.0.0.1:{host_port}'
        write_json(root / 'control.json', control_config)
        control_process, control_fields = start_process(control_binary,
                                                        control_config, root, 'control-cfg')
        children['control'] = control_process
        require(control_fields.get('control_https_port'), 'control startup: ' + str(control_fields))
        control_port = int(control_fields['control_https_port'])
        control = ControlRest(control_port, ca)
        control.login(root)
        # Control health projection needs an admin health.read grant; the host owner routes under
        # test use host.read/host.execute, so a health denial is not a chain failure here.
        status, body = control.request('GET', '/api/v1/health')
        require(status in (200, 403), 'control health projection: ' + str((status, body))[:300])

        host_scope = '?scope=' + __import__('urllib.parse').parse.quote(
            json.dumps({'resources': [{'kind': 'RESOURCE_HOST', 'id': host_id}]}))

        def maintenance():
            status, body = control.request('GET', '/api/v1/hosts/' + host_id + '/maintenance')
            if status != 200 and not STATE.get('debug_direct'):
                STATE['debug_direct'] = True
                try:
                    debug_direct_owner(root, ca, host_port, host_id)
                except Exception as error:  # noqa: BLE001
                    print('P12H debug: direct call failed ' + repr(error), file=sys.stderr,
                          flush=True)
            require(status == 200, 'maintenance GET: ' + str((status, body))[:300])
            return body['maintenance']

        def get_operation(operation_id):
            status, body = control.request(
                'GET', '/api/v1/operations/host/' + operation_id + host_scope)
            require(status == 200, 'operation GET: ' + str((status, body))[:300])
            return body['operation']

        def wait_operation(operation_id, predicate, label, timeout=60.0):
            def read():
                value = get_operation(operation_id)
                return value if predicate(value) else None
            return wait_until(read, label, timeout)

        def begin(grace_ms=1000, expect=202):
            status, body = control.request(
                'POST', '/api/v1/hosts/' + host_id + '/maintenance',
                {'expected_version': int(maintenance()['version']), 'grace_ms': grace_ms})
            if status != expect and not STATE.get('debug_direct'):
                STATE['debug_direct'] = True
                try:
                    debug_direct_owner(root, ca, host_port, host_id, control.token)
                except Exception as error:  # noqa: BLE001
                    print('P12H debug: direct call failed ' + repr(error), file=sys.stderr,
                          flush=True)
            require(status == expect, 'begin: ' + str((status, body))[:400])
            return body

        # 1) All-real Begin: Host closes local admission, real HostBridge calls real Runtime gate,
        # real Identity authorizes the Host leaf/binding/host.execute, PG row persists, empty
        # execution set is confirmed and only then SUCCEEDED.
        stage('real begin maintenance')
        started = begin()
        operation_id = started['operation']['ref']['operation_id']
        require(started['operation']['state'] == 'OPERATION_ACCEPTED', 'begin accepted')
        wait_until(lambda: maintenance()['admission'] == 'HOST_ADMISSION_CLOSED',
                   'local admission closed')
        finished = wait_operation(operation_id,
                                  lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                                  'real begin SUCCEEDED')
        require(finished['state'] == 'OPERATION_SUCCEEDED' and
                finished['execution_outcome'] in ('EXECUTION_NOT_STARTED',
                                                  'EXECUTION_KNOWN_SUCCEEDED'),
                'begin known-stop evidence: ' + str(finished)[:300])
        gate_row = runtime_gate_row(root, pg_bin, port, host_id)
        require(gate_row is not None, 'Runtime PG gate row persisted')
        require(gate_row['admission'] == 2 and gate_row['gate_version'] >= 1,
                'Runtime PG gate CLOSED version: ' + str(gate_row))
        require(gate_row['operation_id'] == operation_id, 'PG gate operation identity')
        local = host_state(root)
        require(local['admission'] == 0, 'Host SQLite admission closed')
        require(local['gate'] == gate_row['gate_version'], 'Host gate version matches Runtime PG')
        require(local['operations'][operation_id]['state'] == 'OPERATION_SUCCEEDED',
                'Host SQLite operation SUCCEEDED')
        projected = maintenance()
        require(projected['admission'] == 'HOST_ADMISSION_CLOSED' and
                int(projected['version']) == local['version'],
                'Control projection matches Host SQLite maintenance')
        print('P12H stage: real gate row ' + json.dumps(gate_row), file=sys.stderr, flush=True)

        # 2) Resume reopens admission through the same real gate, then owner GetOperation.
        stage('resume admission')
        status, resume = control.request(
            'POST', '/api/v1/hosts/' + host_id + '/resume',
            {'expected_version': int(projected['version'])})
        require(status in (200, 202), 'resume: ' + str((status, resume))[:300])
        resume_id = resume['operation']['ref']['operation_id']
        wait_operation(resume_id, lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'resume SUCCEEDED')
        opened = wait_until(lambda: (lambda value: value if value['admission'] ==
                                     'HOST_ADMISSION_OPEN' else None)(maintenance()),
                            'admission reopened')
        opened_gate = runtime_gate_row(root, pg_bin, port, host_id)
        require(opened_gate['admission'] == 1 and
                opened_gate['gate_version'] > gate_row['gate_version'],
                'Runtime PG gate OPEN advanced monotonically')
        require(host_state(root)['gate'] == opened_gate['gate_version'],
                'Host gate matches PG after resume')
        owner_operation = get_operation(resume_id)
        require(owner_operation['ref']['owner_service'] == 'host' and
                owner_operation['ref']['resource']['id'] == host_id,
                'owner GetOperation projection')
        require(int(opened['version']) == host_state(root)['version'],
                'Control maintenance version matches Host SQLite')

        # 3) Cancel window: cancellation is only allowed before admission/gate effects.
        stage('cancel window')
        cancelled = False
        for _ in range(6):
            status, begin_body = control.request(
                'POST', '/api/v1/hosts/' + host_id + '/maintenance',
                {'expected_version': int(maintenance()['version']), 'grace_ms': 500})
            require(status == 202, 'cancel begin: ' + str((status, begin_body))[:300])
            candidate = begin_body['operation']['ref']['operation_id']
            status, cancel_body = control.request(
                'POST', '/api/v1/operations/host/' + candidate + '/cancel',
                {'expected_version': '1',
                 'scope': {'resources': [{'kind': 'RESOURCE_HOST', 'id': host_id}]}})
            if status == 200 and cancel_body['operation']['state'] == 'OPERATION_CANCELLED':
                cancelled = True
                break
            require(status == 409, 'late cancel CONFLICT: ' +
                    str((status, cancel_body))[:300])
            wait_operation(candidate, lambda value: value['state'] in (
                'OPERATION_SUCCEEDED', 'OPERATION_BLOCKED'), 'cancel candidate terminal')
            status, resume = control.request(
                'POST', '/api/v1/hosts/' + host_id + '/resume',
                {'expected_version': int(maintenance()['version'])})
            require(status in (200, 202), 'reopen after cancel conflict')
            wait_operation(resume['operation']['ref']['operation_id'],
                           lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                           'reopen after cancel conflict SUCCEEDED')
            wait_until(lambda: maintenance()['admission'] == 'HOST_ADMISSION_OPEN',
                       'admission open for retry')
        require(cancelled, 'an accepted pre-effect operation was cancelled')
        wait_until(lambda: maintenance()['admission'] == 'HOST_ADMISSION_OPEN',
                   'cancel leaves admission open')

        # 4) Host restart persists SQLite maintenance/operation/gate facts and re-registers with the
        # real Runtime; a later real gate round advances the durable version.
        stage('host restart persistence')
        before_restart = maintenance()
        stop(children.pop('host'))
        time.sleep(0.5)
        restarted, restarted_fields = start_process(host_binary, host_config, root, 'host-restart')
        children['host'] = restarted
        require(int(restarted_fields['grpc_port']) == host_port,
                'restarted Host keeps the configured owner endpoint')
        after_restart = wait_until(maintenance, 'maintenance after Host restart')
        require(int(after_restart['version']) == int(before_restart['version']) and
                after_restart['admission'] == before_restart['admission'],
                'Host restart keeps maintenance/admission version')
        restarted_begin = begin()
        wait_operation(restarted_begin['operation']['ref']['operation_id'],
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'post-restart real gate SUCCEEDED')
        post_restart_gate = runtime_gate_row(root, pg_bin, port, host_id)
        require(post_restart_gate['gate_version'] > opened_gate['gate_version'],
                'real gate version monotonic across Host restart')
        status, resume = control.request(
            'POST', '/api/v1/hosts/' + host_id + '/resume',
            {'expected_version': int(maintenance()['version'])})
        require(status in (200, 202), 'post-restart resume')
        wait_operation(resume['operation']['ref']['operation_id'],
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'post-restart resume SUCCEEDED')

        # 5) Runtime gate outage: Host must stay BLOCKED with local admission closed and no fake
        # success; after the real Runtime/PG gate returns the same operation converges.
        stage('runtime gate outage')
        stop(children.pop('runtime'))
        time.sleep(0.5)
        status, outage_begin = control.request(
            'POST', '/api/v1/hosts/' + host_id + '/maintenance',
            {'expected_version': int(maintenance()['version']), 'grace_ms': 500})
        require(status == 202, 'outage begin accepted')
        outage_id = outage_begin['operation']['ref']['operation_id']
        blocked = wait_operation(outage_id, lambda value: value['state'] == 'OPERATION_BLOCKED',
                                 'real gate outage BLOCKED', timeout=60)
        require(blocked['execution_outcome'] != 'EXECUTION_KNOWN_SUCCEEDED',
                'gate outage never claims known success')
        outage_local = host_state(root)
        require(outage_local['admission'] == 0,
                'Host keeps local admission closed while Runtime gate is down')
        require(outage_local['operations'][outage_id]['state'] == 'OPERATION_BLOCKED',
                'Host SQLite retains BLOCKED operation')
        stage('runtime restart')
        runtime, _ = p14.daemon(runtime_binary, root / 'runtime.json')
        children['runtime'] = runtime
        wait_ready(runtime_probe)
        converged = wait_operation(outage_id,
                                   lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                                   'same operation converges after real Runtime recovery',
                                   timeout=90)
        require(converged['state'] == 'OPERATION_SUCCEEDED',
                'outage operation converges through the real PG gate')
        recovered_gate = runtime_gate_row(root, pg_bin, port, host_id)
        require(recovered_gate['gate_version'] > post_restart_gate['gate_version'],
                'PG gate version advanced after runtime recovery')
        require(host_state(root)['gate'] == recovered_gate['gate_version'],
                'Host gate matches PG after recovery')

        # 6) Identity negative decision: host-2 is enrolled with host.read only. A real Host
        # process for host-2 must receive a permission-denied gate decision, stay BLOCKED, and
        # the Runtime PG gate row for host-2 must not claim closed admission success.
        stage('identity negative decision (host-2)')
        second = bootstrap_fixture.HOST_STATE['host-2']
        second_root = root / 'host2'
        second_root.mkdir(mode=0o700)
        second_config = json.loads(json.dumps(host_config))
        second_config['tls'] = {'certificate_file': second['certificate_file'],
                                'private_key_file': second['key_file'], 'ca_file': str(ca)}
        second_config['host']['host_id'] = second['host_id']
        second_config['host']['sqlite_path'] = str(second_root / 'host.sqlite')
        second_config_path = root / 'host-2.json'
        write_json(second_config_path, second_config)
        second_process = subprocess.Popen(
            [str(host_binary), '--config', str(second_config_path)],
            stdout=subprocess.PIPE, stderr=open(root / 'host-2.err', 'w'))
        children['host-2'] = second_process
        second_fields = dict(item.split('=', 1) for item in
                             p14.line(second_process, 30).split() if '=' in item)
        require(second_fields.get('grpc_port'), 'host-2 startup')
        second_port = int(second_fields['grpc_port'])
        second_control_config = json.loads(json.dumps(control_config))
        second_control_config['control_api']['host_endpoint'] = f'127.0.0.1:{second_port}'
        second_control, second_control_fields = start_process(
            control_binary, second_control_config, root, 'control-host2')
        children['control-host2'] = second_control
        second_rest = ControlRest(int(second_control_fields['control_https_port']), ca)
        second_rest.login(root)
        second_scope = '?scope=' + __import__('urllib.parse').parse.quote(
            json.dumps({'resources': [{'kind': 'RESOURCE_HOST', 'id': second['host_id']}]}))

        def second_maintenance():
            status, body = second_rest.request(
                'GET', '/api/v1/hosts/' + second['host_id'] + '/maintenance')
            require(status == 200, 'host-2 maintenance GET: ' + str((status, body))[:300])
            return body['maintenance']

        status, second_begin = second_rest.request(
            'POST', '/api/v1/hosts/' + second['host_id'] + '/maintenance',
            {'expected_version': int(second_maintenance()['version']), 'grace_ms': 500})
        require(status == 202, 'host-2 begin accepted: ' + str((status, second_begin))[:300])
        second_operation = second_begin['operation']['ref']['operation_id']

        def second_read():
            status, body = second_rest.request(
                'GET', '/api/v1/operations/host/' + second_operation + second_scope)
            if status != 200:
                return None
            return body['operation'] if body['operation']['state'] == 'OPERATION_BLOCKED' else None

        denied = wait_until(second_read, 'host-2 gate denial BLOCKED', timeout=60)
        require(denied['state'] == 'OPERATION_BLOCKED' and
                denied['execution_outcome'] != 'EXECUTION_KNOWN_SUCCEEDED',
                'Identity denial blocks Host maintenance without fake success')
        if denied.get('reason'):
            require(denied['reason']['code'] == 'PERMISSION_DENIED',
                    'Identity denial reason: ' + str(denied['reason']))
        second_local = host_state(root, second_root / 'host.sqlite')
        require(second_local['admission'] == 0 and
                second_local['operations'][second_operation]['state'] == 'OPERATION_BLOCKED',
                'host-2 SQLite keeps closed admission and BLOCKED operation')
        second_gate = runtime_gate_row(root, pg_bin, port, second['host_id'])
        require(second_gate is None or second_gate['admission'] != 1,
                'denied gate never persists an open admission: ' + str(second_gate))
        print('P12H stage: identity denial ' + json.dumps(denied)[:400], file=sys.stderr,
              flush=True)
        print('P12H real host chain passed: ' + operation_id, flush=True)
    finally:
        for name, child in reversed(list(children.items())):
            try:
                stop(child)
            except Exception:  # noqa: BLE001
                pass
        if sys.exc_info()[0] is not None:
            import shutil
            destination = REPO / 'build/logs/capability-expansion'
            destination.mkdir(parents=True, exist_ok=True)
            for name in ('host', 'host-restart', 'host-2', 'control-cfg', 'control-host2',
                         'runtime', 'identity'):
                source = root / f'{name}.err'
                if source.exists():
                    shutil.copyfile(source, destination / f'p12-real-chain-{name}.err')
        for name in ('host', 'host-restart', 'host-2', 'control-cfg', 'control-host2',
                     'runtime', 'identity'):
            path = root / f'{name}.err'
            if path.exists() and sys.exc_info()[0] is not None:
                limit = 8000 if name == 'identity' else 600
                sys.stderr.write(name + '.err tail: ' + path.read_text()[-limit:] + '\n')


if __name__ == '__main__':
    if len(sys.argv) in (6, 7) and sys.argv[1] == '--debug-direct':
        STATE['identity_port'] = int(
            json.loads((Path(sys.argv[2]) / 'identity.json').read_text())['grpc_listen']
            .rsplit(':', 1)[1])
        debug_direct_body(Path(sys.argv[2]), sys.argv[3], int(sys.argv[4]), sys.argv[5],
                          sys.argv[6] if len(sys.argv) == 7 else '')
        raise SystemExit(0)
    try:
        main()
    except Exception as error:  # noqa: BLE001
        print('P12H stage: failure ' + type(error).__name__ + ': ' + str(error)[:600],
              file=sys.stderr, flush=True)
        traceback.print_exc()
        raise SystemExit(1)
