#!/usr/bin/env python3
"""Real two-process Runtime<->Identity mTLS/PostgreSQL authorization end-to-end test.

Owned PG16.4 cluster, offline bootstrap enrollment of the Runtime platform peer, real Identity
and Runtime daemons with mTLS, a real Control-signed delegation and a real gRPC resource query.
Nothing here is a fake Identity server: every allow/deny decision crosses the production
Identity.CheckAuthorization path and every command crosses the production Runtime RPC surface.

Invoked only by infrastructure/postgres/tests/run_database_test.py with its owned cluster and the
compiled initializer/bootstrap/daemon binaries. No external DSN is accepted.
"""
import hashlib
import http.client
import json
import os
from pathlib import Path
import secrets
import select
import signal
import ssl
import subprocess
import sys
import time
import uuid

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402

# Filled by the pre-bootstrap hook: administrator grant ids reused by the post-bootstrap scenario.
STATE = {}


def require(condition, stage):
    if not condition:
        raise RuntimeError('runtime identity e2e assertion: ' + stage)


def line(child, timeout=15):
    end = time.monotonic() + timeout
    value = b''
    while not value.endswith(b'\n'):
        remaining = max(0, end - time.monotonic())
        require(select.select([child.stdout], [], [], remaining)[0] != [],
                'daemon startup timeout')
        byte = os.read(child.stdout.fileno(), 1)
        require(bool(byte), 'daemon stopped before startup')
        value += byte
    return value.decode().strip()


def probe_ready(port):
    try:
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=2)
        connection.request('GET', '/health/ready')
        response = connection.getresponse()
        body = response.read()
        connection.close()
        return response.status == 200 and b'"state":"HEALTHY"' in body
    except (OSError, http.client.HTTPException):
        return False


def wait_ready(port, expected, timeout=15.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if probe_ready(port) == expected:
            return True
        time.sleep(0.1)
    return False


def prepare(root, enrolled):
    """Add the business grant the delegation will carry and Control's management grants."""
    account_id, admin = str(uuid.uuid4()), enrolled['administrator_id']
    enrolled['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()),
        'permissions': ['account.read', 'account.manage', 'resource.manage', 'resource.read'],
        'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account_id}]})
    enrolled['control_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account_id}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.principal.manage'],
         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': admin}]}])
    STATE.update(account_id=account_id,
                 account_read_assignment=enrolled['administrator_assignments'][-1]['assignment_id'])


def identity_security_material(root):
    material = {}
    for name in ['token', 'request', 'receipt']:
        path = root / f'{name}.hex'
        path.write_text(secrets.token_hex(32) + '\n')
        path.chmod(0o600)
        material[name + '_key'] = {'id': name + '-v1', 'file': str(path)}
    private, public = root / 'delegation.key', root / 'delegation.pem'
    for arguments in [('genpkey', '-algorithm', 'ED25519', '-out', str(private)),
                      ('pkey', '-in', str(private), '-pubout', '-out', str(public))]:
        reply = subprocess.run(['openssl', *arguments], capture_output=True, timeout=5)
        require(reply.returncode == 0, 'delegation key generation failed')
    private.chmod(0o600)
    public.chmod(0o600)
    material.update(
        delegation_key={'id': 'delegation-v1', 'public_key_file': str(public),
                        'private_key_file': str(private)},
        issuer='identity-owned-e2e',
        clock={'trusted': True, 'maximum_known_skew_ms': 2000},
        verification={'catalog_id': str(uuid.uuid4()), 'version': 1, 'key_version': 1,
                      'valid_until_epoch_seconds': int(time.time()) + 3600})
    config = json.loads((root / 'identity.json').read_text())
    config['identity_security'] = material
    (root / 'identity.json').write_text(json.dumps(config))


def postgres(root, pg_bin, port, database='postgres'):
    environment = {
        'PGHOST': 'localhost', 'PGPORT': str(port), 'PGUSER': 'p03_admin',
        'PGDATABASE': database, 'PGPASSWORD': (root / 'password').read_text().strip(),
        'PGSSLMODE': 'verify-full', 'PGSSLROOTCERT': str(root / 'server.pem'),
        'PGCHANNELBINDING': 'require', 'PGCONNECT_TIMEOUT': '2'}
    return environment, pg_bin


def run_sql(environment, statement):
    reply = subprocess.run([str(environment[1] / 'psql'), '-X', '-qAt', '-v',
                            'ON_ERROR_STOP=1'], input=statement, text=True,
                           env=environment[0], capture_output=True, timeout=15)
    require(reply.returncode == 0, 'owned fixture SQL failed; diagnostics redacted')
    return reply.stdout.strip()


def runtime_database(root, pg_bin, port, initializer):
    migration, runtime = secrets.token_hex(32), secrets.token_hex(32)
    migration_file = root / 'password-p03_runtime_migration'
    runtime_file = root / 'password-p03_runtime_runtime'
    for path, value in [(migration_file, migration), (runtime_file, runtime)]:
        path.write_text(value + '\n')
        path.chmod(0o600)
    environment = postgres(root, pg_bin, port)
    constraints = ('LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION '
                   'NOBYPASSRLS')
    run_sql(environment,
            f"CREATE ROLE p03_runtime_migration {constraints} PASSWORD '{migration}';\n"
            f"CREATE ROLE p03_runtime_runtime {constraints} PASSWORD '{runtime}';\n"
            'CREATE DATABASE p03_runtime OWNER p03_runtime_migration;\n'
            'REVOKE ALL ON DATABASE p03_runtime FROM PUBLIC;\n'
            'GRANT CONNECT ON DATABASE p03_runtime TO p03_runtime_runtime;\n')
    initialized = subprocess.run(
        [initializer, '--initialize-empty', 'runtime', 'localhost', str(port), 'p03_runtime',
         'p03_runtime_migration', 'p03_runtime_runtime', str(root / 'server.pem'),
         str(migration_file)],
        capture_output=True, text=True, env={key: value for key, value in os.environ.items()
                                             if not key.startswith('PG')},
        timeout=20)
    require(initialized.returncode == 0, 'owned runtime initialization failed')
    runtime_db = {'host': 'localhost', 'port': int(port), 'name': 'p03_runtime',
                  'migration_role': 'p03_runtime_migration',
                  'runtime_role': 'p03_runtime_runtime',
                  'password_file': str(runtime_file),
                  'ca_file': str(root / 'server.pem')}
    return runtime_db, environment


def daemon(binary, config):
    child = subprocess.Popen([binary, '--config', str(config)], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
    state = dict((item.split('=', 1) for item in line(child).split()))
    return child, state


def timestamp(seconds=30):
    from google.protobuf.timestamp_pb2 import Timestamp
    value = Timestamp()
    value.FromNanoseconds(int((time.time() + seconds) * 1e9))
    return value


def context(seconds=30, delegation=''):
    from kiarabot.v1 import platform_pb2
    value = platform_pb2.RequestContext(
        request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()), deadline=timestamp(seconds),
        idempotency_key=str(uuid.uuid4()))
    if delegation:
        value.delegation = delegation
    return value


def channel(endpoint, tls, certificate='control.pem', key='control.key'):
    import grpc
    credentials = grpc.ssl_channel_credentials(
        root_certificates=(tls / 'ca.pem').read_bytes(),
        private_key=(tls / key).read_bytes(),
        certificate_chain=(tls / certificate).read_bytes())
    return grpc.secure_channel(endpoint, credentials)


def leaf_fingerprint(tls):
    der = ssl.PEM_cert_to_DER_cert((tls / 'control.pem').read_text())
    return hashlib.sha256(der).digest()


def scenario(root, port, pg_bin, binaries):
    identity_binary, runtime_binary = binaries
    tls = root / 'bootstrap-tls'
    identity_security_material(root)
    runtime_db, environment = runtime_database(root, pg_bin, port, sys.argv[3])
    account_id = STATE['account_id']
    run_sql(({**environment[0], 'PGDATABASE': 'p03_runtime'}, environment[1]),
            f"INSERT INTO kiarabot.accounts "
            f"(id,platform,platform_account_id,display_name,version,state) VALUES "
            f"('{account_id}','qq','10001','Alice',1,1);")
    identity, identity_state = daemon(identity_binary, root / 'identity.json')
    runtime = None
    fallback = None
    children = [identity]
    try:
        require(identity_state.get('ready') == 'true' and identity_state.get('service') == 'identity',
                'identity readiness/startup')
        identity_endpoint = '127.0.0.1:' + identity_state['grpc_port']
        config = {'schema_version': 1, 'service': 'runtime', 'probe_listen': '127.0.0.1:0',
                  'grpc_listen': '127.0.0.1:0',
                  'tls': {'certificate_file': str(tls / 'runtime.pem'),
                          'private_key_file': str(tls / 'runtime.key'),
                          'ca_file': str(tls / 'ca.pem')},
                  'database': runtime_db, 'identity_endpoint': identity_endpoint}
        (tls / 'runtime.key').chmod(0o600)
        runtime_config = root / 'runtime.json'
        runtime_config.write_text(json.dumps(config))
        runtime_config.chmod(0o600)
        runtime, runtime_state = daemon(runtime_binary, runtime_config)
        children.append(runtime)
        require(runtime_state.get('service') == 'runtime', 'runtime startup identity')
        probe_port = int(runtime_state['probe_port'])
        require(wait_ready(probe_port, True), 'runtime readiness never became true')

        import grpc
        from kiarabot.v1 import (identity_pb2, identity_pb2_grpc, platform_pb2, policy_pb2,
                                 runtime_pb2, runtime_pb2_grpc)
        identity_channel = channel(identity_endpoint, tls)
        runtime_endpoint = '127.0.0.1:' + runtime_state['grpc_port']
        runtime_channel = channel(runtime_endpoint, tls)
        try:
            identity_stub = identity_pb2_grpc.IdentityServiceStub(identity_channel)
            runtime_stub = runtime_pb2_grpc.RuntimeServiceStub(runtime_channel)
            password = (root / 'admin-password').read_text().strip()
            tokens = identity_stub.Login(identity_pb2.IdentityLoginRequest(
                context=context(), username='administrator', password=password), timeout=10)
            access = tokens.tokens.access_token
            require(bool(access), 'login access token')

            def issue_delegation(permissions=None):
                if permissions is None:
                    permissions = [policy_pb2.PERMISSION_ACCOUNT_READ]
                return identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                    context=context(),
                    subject_proof=access,
                    audience='runtime',
                    scope=platform_pb2.ResourceScope(resources=[
                        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ACCOUNT,
                                                 id=account_id)]),
                    permissions=permissions,
                    client_certificate_sha256=leaf_fingerprint(tls)), timeout=10)

            proof = issue_delegation().delegation.proof
            require(bool(proof), 'delegation proof')

            def get_account():
                return runtime_stub.GetAccount(runtime_pb2.RuntimeGetAccountRequest(
                    context=context(delegation=proof),
                    resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ACCOUNT,
                                                      id=account_id)), timeout=10)

            response = get_account()
            require(response.account.account_id == account_id and
                    response.account.display_name == 'Alice',
                    'real mTLS runtime query allowed by current Identity authority')

            # P13a lifecycle RPCs: the same real Identity delegation now carries resource.manage.
            manage_proof = issue_delegation([
                policy_pb2.PERMISSION_RESOURCE_READ,
                policy_pb2.PERMISSION_RESOURCE_MANAGE]).delegation.proof
            require(bool(manage_proof), 'resource.manage delegation proof')
            deleted = runtime_stub.BeginDelete(runtime_pb2.RuntimeBeginDeleteRequest(
                context=context(delegation=manage_proof),
                resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ACCOUNT,
                                                  id=account_id),
                expected_version=1), timeout=10)
            operation_ref = deleted.operation
            require(bool(operation_ref.operation_id), 'BeginDelete accepted operation')
            operation = runtime_stub.GetOperation(runtime_pb2.RuntimeGetOperationRequest(
                context=context(delegation=manage_proof), operation=operation_ref), timeout=10)
            require(operation.operation.state == platform_pb2.OPERATION_SUCCEEDED,
                    'runtime-only deletion completed operation')
            require(len(operation.operation.participants) == 1 and
                    operation.operation.participants[0].participant == 'runtime',
                    'deletion participant progress projected')
            restored = runtime_stub.RestoreResource(runtime_pb2.RuntimeRestoreResourceRequest(
                context=context(delegation=manage_proof),
                resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ACCOUNT,
                                                  id=account_id),
                expected_version=2), timeout=10)
            restored_operation = runtime_stub.GetOperation(
                runtime_pb2.RuntimeGetOperationRequest(
                    context=context(delegation=manage_proof),
                    operation=restored.operation), timeout=10)
            require(restored_operation.operation.state == platform_pb2.OPERATION_SUCCEEDED,
                    'restore completed operation')
            require(get_account().account.account_id == account_id,
                    'restored account remains readable')

            identity_stub.RevokeAssignment(identity_pb2.IdentityRevokeAssignmentRequest(
                context=context(), assignment_id=STATE['account_read_assignment'],
                expected_version=1), timeout=10)
            denied = None
            try:
                get_account()
            except grpc.RpcError as error:
                denied = error.code()
            require(denied in (grpc.StatusCode.PERMISSION_DENIED,
                               grpc.StatusCode.UNAUTHENTICATED),
                    'revoked assignment must fail closed')
            issuance_denied = None
            try:
                issue_delegation()
            except grpc.RpcError as error:
                issuance_denied = error.code()
            require(issuance_denied in (grpc.StatusCode.PERMISSION_DENIED,
                                        grpc.StatusCode.UNAUTHENTICATED),
                    'revoked assignment cannot mint new delegation')

            admin_credential = json.loads((root / 'enrollment.json').read_text())[
                'password_credential_id']
            identity_stub.RevokeCredential(identity_pb2.IdentityRevokeCredentialRequest(
                context=context(), credential_id=admin_credential,
                expected_version=1), timeout=10)
            login_denied = None
            try:
                identity_stub.Login(identity_pb2.IdentityLoginRequest(
                    context=context(), username='administrator', password=password), timeout=10)
            except grpc.RpcError as error:
                login_denied = error.code()
            require(login_denied == grpc.StatusCode.UNAUTHENTICATED,
                    'revoked administrator credential rejected')

            identity.send_signal(signal.SIGTERM)
            require(identity.wait(timeout=10) == 0, 'identity graceful stop')
            require(wait_ready(probe_port, False, 20.0),
                    'runtime readiness must fail closed when Identity is unavailable')
            require(runtime.poll() is None, 'runtime process stays alive without Identity')
            # A Runtime configured without identity_endpoint never accepts business commands and is
            # not ready: the missing authorization dependency is explicit fail-closed configuration.
            # The database writer lease is single-process, so stop the authorized runtime first.
            runtime.send_signal(signal.SIGTERM)
            require(runtime.wait(timeout=10) == 0, 'runtime graceful stop')
            fallback_config = dict(config)
            del fallback_config['identity_endpoint']
            fallback_path = root / 'runtime-no-identity.json'
            fallback_path.write_text(json.dumps(fallback_config))
            fallback_path.chmod(0o600)
            fallback, fallback_state = daemon(runtime_binary, fallback_path)
            children.append(fallback)
            require(fallback_state.get('ready') == 'false', 'missing Identity config is not ready')
            require(not wait_ready(int(fallback_state['probe_port']), True, 2.0),
                    'missing Identity config never becomes ready')
            fallback_denied = None
            fallback_channel = channel('127.0.0.1:' + fallback_state['grpc_port'], tls)
            try:
                runtime_pb2_grpc.RuntimeServiceStub(fallback_channel).GetAccount(
                    runtime_pb2.RuntimeGetAccountRequest(
                        context=context(delegation=proof),
                        resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ACCOUNT,
                                                          id=account_id)), timeout=10)
            except grpc.RpcError as error:
                fallback_denied = error.code()
            finally:
                fallback_channel.close()
            require(fallback_denied in (grpc.StatusCode.UNAUTHENTICATED,
                                        grpc.StatusCode.UNAVAILABLE,
                                        grpc.StatusCode.PERMISSION_DENIED),
                    'fail-closed authorizer rejects without fake success')
        finally:
            identity_channel.close()
            runtime_channel.close()
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.send_signal(signal.SIGTERM)
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
    print('Real two-process Runtime<->Identity mTLS authorization e2e passed')


def main():
    bootstrap_fixture.main(prepare=prepare, after_bootstrap=lambda root: scenario(
        root, int(sys.argv[2]), Path(sys.argv[6]),
        [str(Path(value).resolve()) for value in sys.argv[7:9]]))


if __name__ == '__main__':
    main()
