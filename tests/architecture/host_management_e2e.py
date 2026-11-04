#!/ usr / bin / env python3
"""HOST-04 real Host process maintenance e2e with controlled fake Runtime/Identity TLS services.

The Host binary under test is the production `kiarabot-plugin-host` composition: it owns its
SQLite maintenance store/journal, the real POSIX supervisor, the real HostManagementService owner
mTLS listener, and the real Host->Runtime admission gate client. Runtime and Identity are bounded
test doubles on a real TLS socket: the HostBridge registration/fence frame and the unary
Runtime.SetHostAdmissionGate / Identity.CheckAuthorization calls cross real mTLS but never a
production service. No external DSN, QQ, paid provider or user database is involved.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
import time
import uuid

import grpc
from google.protobuf import timestamp_pb2
from kiarabot.v1 import (common_pb2, host_pb2, host_pb2_grpc, identity_pb2,
                         identity_pb2_grpc, platform_pb2, runtime_pb2,
                         runtime_pb2_grpc)

HOST_ID = '11111111-1111-4111-8111-111111111111'
INSTANCE_ID = '22222222-2222-4222-8222-222222222222'
INSTALLATION_ID = '33333333-3333-4333-8333-333333333333'
RUNTIME_EPOCH = 'fixture-runtime-epoch'
STATE = {}


def require(condition, stage):
    if not condition:
        raise RuntimeError('host management e2e assertion: ' + stage)


def openssl(root, *arguments):
    result = subprocess.run(['openssl', *arguments], cwd=root, capture_output=True, timeout=10)
    require(result.returncode == 0, 'fixture certificate generation')


def certificates(root):
    (root / 'ca.ext').write_text(
        'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n'
        'subjectKeyIdentifier=hash\n')
    (root / 'server.ext').write_text(
        'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature,keyEncipherment\n'
        'extendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n')
    (root / 'host.ext').write_text(
        'subjectAltName=DNS:localhost,IP:127.0.0.1,URI:spiffe://kiarabot/host-test\n'
        'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n'
        'extendedKeyUsage=serverAuth,clientAuth\nsubjectKeyIdentifier=hash\n'
        'authorityKeyIdentifier=keyid:always\n')
    (root / 'control.ext').write_text(
        'subjectAltName=URI:spiffe://kiarabot/control-test\nbasicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n'
        'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n')
    openssl(root, 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
            '-nodes', '-days', '1', '-subj', '/CN=HOST04-Test-CA', '-keyout', 'ca.key',
            '-out', 'ca.pem', '-addext', 'basicConstraints=critical,CA:TRUE',
            '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
            '-addext', 'subjectKeyIdentifier=hash')
    for name, key, extension, subject in (
            ('server', 'server.key', 'server.ext', '/CN=localhost'),
            ('host', 'host.key', 'host.ext', '/CN=localhost'),
            ('control', 'control.key', 'control.ext', '/CN=control-test')):
        openssl(root, 'req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
                '-subj', subject, '-keyout', key, '-out', f'{name}.csr')
        openssl(root, 'x509', '-req', '-in', f'{name}.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
                '-CAcreateserial', '-days', '1', '-extfile', extension, '-out', f'{name}.pem')
    for key in ('server.key', 'host.key', 'control.key'):
        (root / key).chmod(0o600)
    return {
        'ca': (root / 'ca.pem').read_text(),
        'server_chain': (root / 'server.pem').read_text(),
        'server_key': (root / 'server.key').read_text(),
        'host_chain': (root / 'host.pem').read_text(),
        'host_key': (root / 'host.key').read_text(),
        'control_chain': (root / 'control.pem').read_text(),
        'control_key': (root / 'control.key').read_text(),
        'control_fingerprint': hashlib.sha256(ssl.PEM_cert_to_DER_cert(
            (root / 'control.pem').read_text())).hexdigest(),
    }


def timestamp(seconds):
    return timestamp_pb2.Timestamp(seconds=int(time.time()) + seconds)


def request_context(idempotent=False):
    value = platform_pb2.RequestContext(
        request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()),
        deadline=timestamp(60))
    if idempotent:
        value.idempotency_key = str(uuid.uuid4())
    return value


def authorized_context(idempotent=False):
    value = request_context(idempotent)
    if 'artifacts' in STATE:
        value.delegation = delegation_proof(STATE['artifacts'])
    return value


def base64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def delegation_proof(artifacts):
    now = int(time.time())
    document = {
        'sub': str(uuid.uuid4()), 'aud': 'host', 'jti': str(uuid.uuid4()),
        'iss': str(uuid.uuid4()), 'iat': now - 1, 'nbf': now - 1, 'exp': now + 30,
        'delegation': {
            'direct_actor': str(uuid.uuid4()),
            'actor_chain': [str(uuid.uuid4())],
            'permissions': [['9', '5', HOST_ID]],
            'client_certificate': artifacts['control_fingerprint'],
            'authorization_version': '1',
            'business_hops': '0',
            'source_credential': [str(uuid.uuid4()), '1'],
            'scope': [],
        },
    }
    payload = base64url(json.dumps(document, separators=(',', ':')).encode())
    return base64url(b'{}') + '.' + payload + '.' + base64url(b'fixture-signature')


class FakeIdentity(identity_pb2_grpc.IdentityServiceServicer):
    def __init__(self):
        self.check_calls = 0
        self.revoked = False

    def CheckAuthorization(self, request, context):
        self.check_calls += 1
        decision = identity_pb2.AuthorizationDecision(
            allowed=not self.revoked, authorization_version=1,
            checked_at=timestamp_pb2.Timestamp(seconds=int(time.time())))
        if self.revoked:
            decision.denial.CopyFrom(common_pb2.Error(code=common_pb2.PERMISSION_DENIED))
        return identity_pb2.IdentityCheckAuthorizationResponse(decision=decision)


class FakeRuntime(runtime_pb2_grpc.RuntimeServiceServicer, host_pb2_grpc.HostBridgeServicer):
    def __init__(self):
        self.gate_calls = []
        self.gate_version = 0
        self.gate_mode = 'ok'
        self.receipts = []
        self.apply_sent = threading.Event()
        self.connected = threading.Event()

    def SetHostAdmissionGate(self, request, context):
        self.gate_calls.append(request)
        if request.host_id != HOST_ID or request.fence.instance_id != HOST_ID:
            context.abort(grpc.StatusCode.UNAUTHENTICATED, 'host/fence mismatch')
        if self.gate_mode == 'unavailable':
            context.abort(grpc.StatusCode.UNAVAILABLE, 'fixture gate unavailable')
        if self.gate_mode == 'lost_ack_once':
#Durably applied by the fake owner, but the first response is lost; the Host must
#retry under the same operation identity and converge on the same gate.
            self.gate_mode = 'ok'
            self.gate_version += 1
            self._gate_receipt = (request, self.gate_version)
            context.abort(grpc.StatusCode.UNAVAILABLE, 'fixture lost gate ack')
        self.gate_version += 1
        return runtime_pb2.RuntimeSetHostAdmissionGateResponse(
            host_id=HOST_ID, gate_version=self.gate_version,
            maintenance_version=request.maintenance_version, admission=request.admission,
            operation=request.operation)

    def Connect(self, request_iterator, context):
        yield from self._connect(request_iterator, context)

    def _connect(self, request_iterator, context):
        registration = next(request_iterator)
        require(registration.WhichOneof('body') == 'registration',
                'host registers over real mTLS')
        require(registration.registration.host.host_id == HOST_ID, 'registration host id')
        self.connected.set()
        fence = platform_pb2.Fence(instance_id=HOST_ID, connection_generation=3,
                                   runtime_epoch=RUNTIME_EPOCH)
        registered = host_pb2.RuntimeToHost()
        registered.registered.host.CopyFrom(registration.registration.host)
        registered.registered.host.version = 1
        registered.registered.fence.CopyFrom(fence)
        yield registered
        apply = host_pb2.RuntimeToHost()
        apply.fence.CopyFrom(fence)
        command = apply.apply
        command.command_id = str(uuid.uuid4())
        command.context.CopyFrom(request_context())
        command.handoff.payload_sha256 = b'\xaa' * 32
        command.process.instance_id = INSTANCE_ID
        command.process.installation_id = INSTALLATION_ID
        command.process.execution_generation = 1
        command.process.desired_version = 1
        command.process.desired_state = platform_pb2.DESIRED_RUNNING
        command.process.package_sha256 = b'\xaa' * 32
        command.process.launch_profile = 'fixture'
        yield apply
        self.apply_sent.set()
        for frame in request_iterator:
            if frame.WhichOneof('body') == 'receipt':
                self.receipts.append(frame.receipt)
            elif frame.WhichOneof('body') == 'heartbeat':
                ack = host_pb2.RuntimeToHost()
                ack.heartbeat_ack.fence.CopyFrom(fence)
                ack.heartbeat_ack.received_at.FromDatetime(
                    __import__('datetime').datetime.now(__import__('datetime').timezone.utc))
                yield ack


def start_fake_services(artifacts):
    credentials = grpc.ssl_server_credentials(
        [(artifacts['server_key'].encode(), artifacts['server_chain'].encode())],
        root_certificates=artifacts['ca'].encode(),
        require_client_auth=True)
    fake_runtime = FakeRuntime()
    fake_identity = FakeIdentity()
    server = grpc.server(ThreadPoolExecutor(max_workers=8))
    runtime_pb2_grpc.add_RuntimeServiceServicer_to_server(fake_runtime, server)
    host_pb2_grpc.add_HostBridgeServicer_to_server(fake_runtime, server)
    identity_pb2_grpc.add_IdentityServiceServicer_to_server(fake_identity, server)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    server.add_secure_port(f'127.0.0.1:{port}', credentials)
    server.start()
    return server, port, fake_runtime, fake_identity


def control_channel(host_port, artifacts):
    credentials = grpc.ssl_channel_credentials(
        root_certificates=artifacts['ca'].encode(),
        private_key=artifacts['control_key'].encode(),
        certificate_chain=artifacts['control_chain'].encode())
    return grpc.secure_channel(f'127.0.0.1:{host_port}', credentials)


def host_config(root, artifacts, services_port, profile_executable, profile_arguments):
    return {
        'schema_version': 1, 'service': 'plugin-host',
        'probe_listen': '127.0.0.1:0', 'grpc_listen': '127.0.0.1:0',
        'tls': {'certificate_file': str(root / 'host.pem'),
                'private_key_file': str(root / 'host.key'),
                'ca_file': str(root / 'ca.pem')},
        'identity_endpoint': f'127.0.0.1:{services_port}',
        'host': {
            'host_id': HOST_ID,
            'sqlite_path': str(root / 'host.sqlite'),
            'runtime_endpoint': f'127.0.0.1:{services_port}',
            'credential_directory': str(root),
            'packages': [{'plugin_type_id': 'fixture', 'root': str(root),
                          'sha256': 'aa' * 32, 'launch_profile': 'fixture',
                          'allowed_argument_names': []}],
            'profiles': [{'name': 'fixture', 'executable': profile_executable,
                          'fixed_arguments': profile_arguments,
                          'allowed_environment_names': [], 'credential_environment': None}],
            'environment': {},
        },
    }


def start_host(binary, config_path):
    child = subprocess.Popen([binary, '--config', str(config_path)], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
    line = child.stdout.readline().strip()
    if 'service=plugin-host' not in line:
        detail = child.stderr.read().strip() if child.poll() is not None else ''
        child.terminate()
        child.wait(timeout=10)
        require(False, 'host startup rejected: ' + detail[:160])
    fields = {}
    for item in line.split():
        key, _, value = item.partition('=')
        fields[key] = value
    require(fields.get('ready') == 'false', 'host readiness is fail-closed before Runtime')
    return child, int(fields['grpc_port'])


def stop_host(child):
    child.send_signal(signal.SIGTERM)
    return child.wait(timeout=20)


def wait_until(predicate, stage, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise RuntimeError('host management e2e assertion: timeout ' + stage)


def get_maintenance(stub, artifacts):
    return stub.GetMaintenance(
        host_pb2.HostGetMaintenanceRequest(context=authorized_context(), host_id=HOST_ID),
        timeout=10).maintenance


def begin(stub, artifacts, grace_ms=1000):
    return stub.BeginMaintenance(
        host_pb2.HostBeginMaintenanceRequest(
            context=authorized_context(idempotent=True), host_id=HOST_ID,
            expected_version=get_maintenance(stub, artifacts).version,
            grace_ms=grace_ms), timeout=10)


def get_operation(stub, operation):
    return stub.GetOperation(
        host_pb2.HostGetOperationRequest(context=authorized_context(), operation=operation),
        timeout=10).operation


def scenario(binary, root, artifacts, services_port, fake_runtime, fake_identity):
    config = host_config(root, artifacts, services_port, '/bin/sh',
                         ['sh', '-c', "trap '' TERM; while true; do sleep 1; done"])
    config_path = root / 'host.json'
    config_path.write_text(json.dumps(config))
    config_path.chmod(0o600)
    host, host_port = start_host(binary, config_path)
    children = [host]
    try:
        if not fake_runtime.connected.wait(10.0):
            host.terminate()
            host.wait(timeout=10)
            require(False, 'HostBridge registered with fake Runtime')
        require(fake_runtime.apply_sent.wait(10.0), 'fake Runtime dispatched the apply command')
        wait_until(lambda: any(receipt.persisted for receipt in fake_runtime.receipts),
                   'managed plugin spawned and receipted')
        channel = control_channel(host_port, artifacts)
        try:
            stub = host_pb2_grpc.HostManagementServiceStub(channel)

            maintenance = get_maintenance(stub, artifacts)
            require(maintenance.admission == host_pb2.HOST_ADMISSION_OPEN and
                    maintenance.version == 1, 'initial Host maintenance is open')
            started = begin(stub, artifacts, grace_ms=1000)
            require(started.operation.state == platform_pb2.OPERATION_ACCEPTED,
                    'BeginMaintenance durably accepted')
            accepted_ref = started.operation.ref
            finished = wait_until(lambda: (lambda value: value if value.state in (
                platform_pb2.OPERATION_SUCCEEDED, platform_pb2.OPERATION_BLOCKED) else None)(
                    get_operation(stub, accepted_ref)), 'maintenance converges')
            detail = ' state=' + platform_pb2.OperationState.Name(finished.state)
            if finished.HasField('reason'):
                detail += ' reason=' + common_pb2.ErrorCode.Name(finished.reason.code)
            detail += ' participants=' + '|'.join(
                item.participant + ':' + platform_pb2.OperationState.Name(item.state)
                for item in finished.participants)
            require(finished.state == platform_pb2.OPERATION_SUCCEEDED,
                    'maintenance operation succeeded' + detail)
            maintenance = get_maintenance(stub, artifacts)
            require(maintenance.admission == host_pb2.HOST_ADMISSION_CLOSED,
                    'admission closed after maintenance')
#Resume is an explicit new operation and reopens admission.
            resumed = stub.ResumeAdmission(
                host_pb2.HostResumeAdmissionRequest(
                    context=authorized_context(idempotent=True), host_id=HOST_ID,
                    expected_version=maintenance.version), timeout=10)
            require(resumed.operation.state == platform_pb2.OPERATION_ACCEPTED,
                    'ResumeAdmission durably accepted')
            resumed_ref = resumed.operation.ref
            reopened = wait_until(lambda: (lambda value: value if value.state in (
                platform_pb2.OPERATION_SUCCEEDED, platform_pb2.OPERATION_BLOCKED) else None)(
                    get_operation(stub, resumed_ref)), 'resume converges')
            require(reopened.state == platform_pb2.OPERATION_SUCCEEDED,
                    'resume operation succeeded')
            require(get_maintenance(stub, artifacts).admission == host_pb2.HOST_ADMISSION_OPEN,
                    'admission reopened by explicit resume')

#Lost gate ACK : the Runtime applied the gate but the response was lost.The Host must
#keep the operation BLOCKED / Unknown and only converge on a later retry.
            fake_runtime.gate_mode = 'lost_ack_once'
            lost = begin(stub, artifacts, grace_ms=500)
            lost_ref = lost.operation.ref
            blocked = wait_until(lambda: (lambda value: value if value.state ==
                                          platform_pb2.OPERATION_BLOCKED else None)(
                get_operation(stub, lost_ref)), 'lost ACK keeps operation blocked')
            require(blocked.HasField('reason') and blocked.execution_outcome !=
                    platform_pb2.EXECUTION_KNOWN_SUCCEEDED,
                    'lost ACK is not a false success')
            converged = wait_until(lambda: (lambda value: value if value.state ==
                                            platform_pb2.OPERATION_SUCCEEDED else None)(
                get_operation(stub, lost_ref)), 'lost ACK retry converges')
            require(converged.state == platform_pb2.OPERATION_SUCCEEDED,
                    'same operation identity converges after gate ACK loss')
            reopened = stub.ResumeAdmission(
                host_pb2.HostResumeAdmissionRequest(
                    context=authorized_context(idempotent=True), host_id=HOST_ID,
                    expected_version=get_maintenance(stub, artifacts).version), timeout=10)
            wait_until(lambda: (lambda value: value if value.state in (
                platform_pb2.OPERATION_SUCCEEDED, platform_pb2.OPERATION_BLOCKED) else None)(
                    get_operation(stub, reopened.operation.ref)), 'reopen after lost ACK converges')
            require(get_maintenance(stub, artifacts).admission == host_pb2.HOST_ADMISSION_OPEN,
                    'admission reopened before cancel-window check')

#Cancel window : a just - accepted operation may be cancelled before any gate effect.
            pending = begin(stub, artifacts, grace_ms=500)
            cancelled = stub.CancelOperation(
                host_pb2.HostCancelOperationRequest(
                    context=authorized_context(idempotent=True),
                    operation=pending.operation.ref, expected_version=1), timeout=10)
            require(cancelled.operation.state == platform_pb2.OPERATION_CANCELLED,
                    'accepted operation cancelled before gate effect')
            require(get_maintenance(stub, artifacts).admission == host_pb2.HOST_ADMISSION_OPEN,
                    'cancelled pre-effect operation leaves admission open')
        finally:
            channel.close()
#Restart persistence : an UNKNOWN / BLOCKED maintenance intent stays closed and queryable.
        fake_runtime.gate_mode = 'unavailable'
        host, host_port = start_host(binary, config_path)
        children.append(host)
        wait_until(lambda: any(receipt.persisted for receipt in fake_runtime.receipts),
                   'restarted Host re-registers and spawns nothing')
        channel = control_channel(host_port, artifacts)
        try:
            stub = host_pb2_grpc.HostManagementServiceStub(channel)
            maintenance = get_maintenance(stub, artifacts)
            require(maintenance.admission == host_pb2.HOST_ADMISSION_OPEN,
                    'restart keeps the last durable admission state')
            failing = begin(stub, artifacts, grace_ms=500)
            blocked_ref = failing.operation.ref
            blocked = wait_until(lambda: (lambda value: value if value.state ==
                                          platform_pb2.OPERATION_BLOCKED else None)(
                get_operation(stub, blocked_ref)), 'unavailable Runtime gate blocks bounded')
            require(blocked.state == platform_pb2.OPERATION_BLOCKED and
                    blocked.execution_outcome != platform_pb2.EXECUTION_KNOWN_SUCCEEDED and
                    get_maintenance(stub, artifacts).admission == host_pb2.HOST_ADMISSION_CLOSED,
                    'Runtime gate outage never becomes a false success')
        finally:
            channel.close()
    finally:
        for child in children:
            if child.poll() is None:
                stop_host(child)
        if host.poll() is None:
            child.send_signal(signal.SIGTERM)
    return True


def main():
    binary = str(Path(sys.argv[1]).resolve())
    if not os.path.isfile(binary):
        raise RuntimeError('Host binary required')
    with tempfile.TemporaryDirectory(prefix='kiarabot-host04-e2e-') as directory:
        root = Path(directory)
        artifacts = certificates(root)
        STATE['artifacts'] = artifacts
        server, port, fake_runtime, fake_identity = start_fake_services(artifacts)
        try:
            scenario(binary, root, artifacts, port, fake_runtime, fake_identity)
        finally:
            server.stop(0)
    print('Host management e2e passed (real Host process, real mTLS, fake Runtime gate/Identity)')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:  # noqa: BLE001
        message = str(error) if 'assertion' in str(error) else type(error).__name__
        if isinstance(error, grpc.RpcError):
            message += ' code=' + str(error.code()) + ' details=' + str(error.details())
        print('P08-HOST04 e2e failure: ' + message, file=sys.stderr)
        sys.exit(1)
