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
    account_read_assignment = str(uuid.uuid4())
    enrolled['administrator_assignments'].append({
        'assignment_id': account_read_assignment,
        'permissions': ['account.read', 'account.manage', 'resource.manage', 'resource.read'],
        'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account_id}]})
    instance_id, installation_id = str(uuid.uuid4()), str(uuid.uuid4())
    manage_assignment = str(uuid.uuid4())
    instance_task_assignment = str(uuid.uuid4())
    installation_task_assignment = str(uuid.uuid4())
    enrolled['administrator_assignments'].extend([
        {'assignment_id': manage_assignment, 'permissions': ['resource.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance_id}]},
        {'assignment_id': instance_task_assignment,
         'permissions': ['task.schedule', 'task.read'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance_id}]},
        {'assignment_id': installation_task_assignment,
         'permissions': ['task.schedule', 'task.read'],
         'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': installation_id}]}])
    enrolled['control_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.principal.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance_id}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance_id}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': installation_id}]}])
    # Exchange requires the actual Runtime delegator's own explicit issuance capability. Keep the
    # single bootstrap assignment (fixture asserts exactly one) and add the permission tuple.
    runtime_assignment = enrolled['platform_peers'][0]['assignments'][0]
    runtime_assignment['permissions'].append('identity.delegation.issue')
    enrolled['control_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account_id}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.principal.manage'],
         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': admin}]}])
    # P14m: the delegated subject must hold the exact catalog-scope sensitive reads so a real
    # Control-issued runtime delegation for ListAudit/ListLogs is authorized by Identity.
    enrolled['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()),
        'permissions': ['log.read', 'audit.read'],
        'scope': [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]})
    STATE.update(account_id=account_id, instance_id=instance_id,
                 installation_id=installation_id,
                 manage_assignment=manage_assignment,
                 instance_task_assignment=instance_task_assignment,
                 installation_task_assignment=installation_task_assignment,
                 account_read_assignment=account_read_assignment)



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


def leaf_fingerprint_file(tls, name):
    der = ssl.PEM_cert_to_DER_cert((tls / name).read_text())
    return hashlib.sha256(der).digest()


def scenario(root, port, pg_bin, binaries):
    identity_binary, runtime_binary = binaries
    tls = root / 'bootstrap-tls'
    enrollment = json.loads((root / 'enrollment.json').read_text())
    enrolled_admin_principal = enrollment['administrator_id']
    enrolled_control_principal = enrollment['control_id']
    enrolled_runtime_principal = enrollment['platform_peers'][0]['principal_id']
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
        from google.protobuf import timestamp_pb2
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

            def issue_delegation(permissions=None, scope=None):
                if permissions is None:
                    permissions = [policy_pb2.PERMISSION_ACCOUNT_READ]
                if scope is None:
                    scope = [platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ACCOUNT,
                                                      id=account_id)]
                return identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                    context=context(),
                    subject_proof=access,
                    audience='runtime',
                    scope=platform_pb2.ResourceScope(resources=scope),
                    permissions=permissions,
                    client_certificate_sha256=leaf_fingerprint(tls)), timeout=10)

            proof = issue_delegation().delegation.proof
            require(bool(proof), 'delegation proof')

            # ---- P05b-2/B1: Control administrator issues an instance-subject parent delegation.
            import base64
            def claims_of(token):
                _, payload, _ = token.split('.')
                return json.loads(base64.urlsafe_b64decode(payload + '=' * (-len(payload) % 4)))

            instance_id = STATE['instance_id']
            installation_id = STATE['installation_id']
            registered = identity_stub.RegisterPrincipal(
                identity_pb2.IdentityRegisterPrincipalRequest(
                    context=context(), principal_kind='plugin',
                    binding=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                     id=instance_id),
                    allowed_services=['runtime']), timeout=10)
            instance_principal = registered.principal.principal_id
            require(bool(instance_principal), 'instance principal registration')
            identity_sql = postgres(root, pg_bin, port, 'p03_identity')
            run_sql(identity_sql, "UPDATE kiarabot.principals SET state=0 WHERE id='" +
                    instance_principal + "'; "
                    "INSERT INTO kiarabot.authorizations(subject,version,invalidation_dirty) "
                    "VALUES('" + instance_principal + "',1,false) ON CONFLICT (subject) DO NOTHING;")

            scope = [platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE, id=instance_id),
                     platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTALLATION,
                                              id=installation_id)]
            permissions = [policy_pb2.PERMISSION_TASK_SCHEDULE, policy_pb2.PERMISSION_TASK_READ]
            parent = identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                context=context(), subject_proof=access, audience='runtime',
                scope=platform_pb2.ResourceScope(resources=scope), permissions=permissions,
                client_certificate_sha256=leaf_fingerprint(tls),
                instance_id=instance_id), timeout=10).delegation.proof
            require(bool(parent), 'instance parent delegation proof')
            parent_claims = claims_of(parent)
            parent_body = parent_claims['delegation']
            require(parent_claims['sub'] == instance_principal and
                    parent_claims['aud'] == 'runtime' and
                    parent_body['direct_actor'] == enrolled_control_principal and
                    parent_body['actor_chain'] == [enrolled_control_principal] and
                    parent_body['business_hops'] == '1' and
                    parent_claims['exp'] - parent_claims['iat'] <= 56,
                    'instance parent claims subject/audience/actor/TTL')
            audit_rows = run_sql(identity_sql,
                                 "SELECT count(*) FROM kiarabot.audit_entries WHERE "
                                 "method='IdentityIssueDelegation/InstanceParent'")
            require(int(audit_rows or '0') >= 1, 'instance parent issuance audited')

            runtime_channel = channel(identity_endpoint, tls, 'runtime.pem', 'runtime.key')
            try:
                runtime_identity = identity_pb2_grpc.IdentityServiceStub(runtime_channel)
                child = runtime_identity.ExchangeDelegation(
                    identity_pb2.IdentityExchangeDelegationRequest(
                        context=context(), parent_proof=parent, audience='scheduler',
                        scope=platform_pb2.ResourceScope(resources=scope),
                        permissions=permissions,
                        client_certificate_sha256=leaf_fingerprint_file(tls, 'runtime.pem')),
                    timeout=10).delegation.proof
                child_claims = claims_of(child)
                child_body = child_claims['delegation']
                require(child_claims['sub'] == instance_principal and
                        child_claims['aud'] == 'scheduler' and
                        child_body['business_hops'] == '2' and
                        child_body['actor_chain'] == [enrolled_control_principal,
                                                      enrolled_runtime_principal],
                        'instance parent exchanges to hops2 instance-subject child')
            finally:
                runtime_channel.close()

            # ---- P05b-6: admin trigger -> sealed file -> strict reader ----
            sealed_dir = root / 'delegations'
            sealed_dir.mkdir(mode=0o700)
            token_file = root / 'admin-token'
            token_file.write_text(access + '\n')
            token_file.chmod(0o600)
            sealed_config = {
                'identity_endpoint': identity_endpoint,
                'ca_file': str(tls / 'ca.pem'),
                'control_certificate_file': str(tls / 'control.pem'),
                'control_private_key_file': str(tls / 'control.key'),
                'instance_id': instance_id,
                'installation_id': installation_id,
                'output_dir': str(sealed_dir),
                'token_file': str(token_file),
            }
            sealed_config_file = root / 'sealed-refresh.json'
            sealed_config_file.write_text(json.dumps(sealed_config))
            sealed_config_file.chmod(0o600)
            refresh_tool = REPO / 'scripts/deploy/instance-delegation-refresh'
            refresh = subprocess.run(
                [sys.executable, str(refresh_tool), '--config', str(sealed_config_file),
                 '--once'], env=os.environ.copy(), capture_output=True, text=True, timeout=30)
            require(refresh.returncode == 0,
                    f'sealed refresh failed: {refresh.stderr.strip()[-200:]}')
            sealed_path = sealed_dir / ('instance-delegation-' + instance_id + '.json')
            sys.path.insert(0, str(REPO / 'scripts/deploy'))
            import sealed_delegation
            first_sealed = sealed_delegation.read_sealed_delegation(
                str(sealed_path), expected_instance=instance_id,
                expected_installation=installation_id)
            require(first_sealed['expires_at'] - first_sealed['issued_at'] <= 56,
                    'sealed file TTL')
            require(access not in refresh.stdout and access not in refresh.stderr,
                    'sealed tool leaked admin token')
            # Idempotent refresh: same file path, valid content, no temporary residue.
            second = subprocess.run(
                [sys.executable, str(refresh_tool), '--config', str(sealed_config_file),
                 '--once'], env=os.environ.copy(), capture_output=True, text=True, timeout=30)
            require(second.returncode == 0, 'sealed refresh not idempotent')
            require(sealed_delegation.read_sealed_delegation(
                str(sealed_path), expected_instance=instance_id)['instance_id'] == instance_id,
                'sealed refresh replaced with invalid content')
            require(sorted(entry.name for entry in sealed_dir.iterdir()) ==
                    ['instance-delegation-' + instance_id + '.json'],
                    'sealed refresh left temporary files')
            # Wrong instance must be rejected by Identity and must not create a file.
            wrong_config = dict(sealed_config)
            wrong_instance = str(uuid.uuid4())
            wrong_config['instance_id'] = wrong_instance
            wrong_path = root / 'sealed-wrong.json'
            wrong_path.write_text(json.dumps(wrong_config))
            wrong_path.chmod(0o600)
            wrong_run = subprocess.run(
                [sys.executable, str(refresh_tool), '--config', str(wrong_path), '--once'],
                env=os.environ.copy(), capture_output=True, text=True, timeout=30)
            require(wrong_run.returncode != 0, 'wrong instance sealed refresh must fail')
            require(not (sealed_dir / ('instance-delegation-' + wrong_instance + '.json')).exists(),
                    'wrong instance created a sealed file')
            # Unauthorized token must not replace the valid file.
            bad_token = root / 'bad-token'
            bad_token.write_text('not-an-admin-token\n')
            bad_token.chmod(0o600)
            bad_config = dict(sealed_config)
            bad_config['token_file'] = str(bad_token)
            bad_path = root / 'sealed-bad-token.json'
            bad_path.write_text(json.dumps(bad_config))
            bad_path.chmod(0o600)
            before_bad = sealed_path.read_bytes()
            bad_run = subprocess.run(
                [sys.executable, str(refresh_tool), '--config', str(bad_path), '--once'],
                env=os.environ.copy(), capture_output=True, text=True, timeout=30)
            require(bad_run.returncode != 0, 'unauthorized sealed refresh must fail')
            require(sealed_path.read_bytes() == before_bad,
                    'unauthorized sealed refresh replaced the valid file')
            # Existing symlink target must be rejected without following/replacing it.
            link_dir = root / 'delegations-link'
            link_dir.mkdir(mode=0o700)
            victim = root / 'victim'
            victim.write_text('{}')
            victim.chmod(0o600)
            link_path = link_dir / ('instance-delegation-' + instance_id + '.json')
            link_path.symlink_to(victim)
            link_config = dict(sealed_config)
            link_config['output_dir'] = str(link_dir)
            link_config_path = root / 'sealed-link.json'
            link_config_path.write_text(json.dumps(link_config))
            link_config_path.chmod(0o600)
            link_run = subprocess.run(
                [sys.executable, str(refresh_tool), '--config', str(link_config_path), '--once'],
                env=os.environ.copy(), capture_output=True, text=True, timeout=30)
            require(link_run.returncode != 0, 'symlink sealed target must be rejected')
            require(victim.read_text() == '{}' and link_path.is_symlink(),
                    'symlink target was followed or replaced')

            # Negative: whitelist rejects model.chat bound to an instance resource.
            wrong_permission = None
            try:
                identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                    context=context(), subject_proof=access, audience='runtime',
                    scope=platform_pb2.ResourceScope(resources=[
                        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                 id=instance_id)]),
                    permissions=[policy_pb2.PERMISSION_MODEL_CHAT],
                    client_certificate_sha256=leaf_fingerprint(tls),
                    instance_id=instance_id), timeout=10)
            except grpc.RpcError as error:
                wrong_permission = error.code()
            require(wrong_permission in (grpc.StatusCode.INVALID_ARGUMENT,
                                         grpc.StatusCode.PERMISSION_DENIED),
                    'instance parent non-whitelisted permission rejected')

            # Negative: revoking the administrator's exact grant invalidates a later Exchange.
            identity_stub.RevokeAssignment(identity_pb2.IdentityRevokeAssignmentRequest(
                context=context(), assignment_id=STATE['instance_task_assignment'],
                expected_version=1), timeout=10)
            revoked_exchange = None
            try:
                runtime_channel = channel(identity_endpoint, tls, 'runtime.pem', 'runtime.key')
                try:
                    identity_pb2_grpc.IdentityServiceStub(runtime_channel).ExchangeDelegation(
                        identity_pb2.IdentityExchangeDelegationRequest(
                            context=context(), parent_proof=parent, audience='scheduler',
                            scope=platform_pb2.ResourceScope(resources=scope),
                            permissions=permissions,
                            client_certificate_sha256=leaf_fingerprint_file(tls,
                                                                           'runtime.pem')),
                        timeout=10)
                finally:
                    runtime_channel.close()
            except grpc.RpcError as error:
                revoked_exchange = error.code()
            require(revoked_exchange in (grpc.StatusCode.PERMISSION_DENIED,
                                         grpc.StatusCode.UNAUTHENTICATED),
                    'revoked administrator grant blocks Exchange')

            # Negative: a non-authorized administrator scope cannot mint another parent.
            identity_stub.RevokeAssignment(identity_pb2.IdentityRevokeAssignmentRequest(
                context=context(), assignment_id=STATE['manage_assignment'],
                expected_version=1), timeout=10)
            unauthorized_issue = None
            try:
                identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                    context=context(), subject_proof=access, audience='runtime',
                    scope=platform_pb2.ResourceScope(resources=scope), permissions=permissions,
                    client_certificate_sha256=leaf_fingerprint(tls),
                    instance_id=instance_id), timeout=10)
            except grpc.RpcError as error:
                unauthorized_issue = error.code()
            require(unauthorized_issue in (grpc.StatusCode.PERMISSION_DENIED,
                                           grpc.StatusCode.UNAUTHENTICATED),
                    'non-authorized administrator cannot mint instance parent')

            # Negative: wrong certificate binding on issuance is rejected before signing.
            wrong_cert = None
            try:
                identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                    context=context(), subject_proof=access, audience='runtime',
                    scope=platform_pb2.ResourceScope(resources=scope), permissions=permissions,
                    client_certificate_sha256=leaf_fingerprint_file(tls, 'runtime.pem'),
                    instance_id=instance_id), timeout=10)
            except grpc.RpcError as error:
                wrong_cert = error.code()
            require(wrong_cert == grpc.StatusCode.INVALID_ARGUMENT,
                    'instance parent wrong certificate rejected')

            # Negative: nonexistent instance binding (no arbitrary subject parameter exists).
            missing = None
            try:
                identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                    context=context(), subject_proof=access, audience='runtime',
                    scope=platform_pb2.ResourceScope(resources=[
                        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                 id=str(uuid.uuid4()))]),
                    permissions=[policy_pb2.PERMISSION_TASK_READ],
                    client_certificate_sha256=leaf_fingerprint(tls),
                    instance_id=str(uuid.uuid4())), timeout=10)
            except grpc.RpcError as error:
                missing = error.code()
            require(missing in (grpc.StatusCode.NOT_FOUND, grpc.StatusCode.PERMISSION_DENIED),
                    'instance parent missing binding rejected')

            # Negative: cross-instance / non-whitelisted scope.
            cross = None
            try:
                identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                    context=context(), subject_proof=access, audience='runtime',
                    scope=platform_pb2.ResourceScope(resources=[
                        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                 id=instance_id),
                        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                 id=str(uuid.uuid4()))]),
                    permissions=[policy_pb2.PERMISSION_TASK_READ],
                    client_certificate_sha256=leaf_fingerprint(tls),
                    instance_id=instance_id), timeout=10)
            except grpc.RpcError as error:
                cross = error.code()
            require(cross in (grpc.StatusCode.INVALID_ARGUMENT, grpc.StatusCode.PERMISSION_DENIED),
                    'instance parent cross-instance scope rejected')

            # B1 negatives changed the administrator's authorization version; re-mint the legacy
            # account delegation for the remaining P13/P14 assertions.
            proof = issue_delegation().delegation.proof
            require(bool(proof), 'post-B1 account delegation proof')

            # ---- end P05b-2/B1 positive/negative issuance ----

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

            # P14m: Control-issued runtime delegation for the catalog-scope sensitive reads.
            catalog = [platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                                id='control')]
            audit_proof = issue_delegation([policy_pb2.PERMISSION_AUDIT_READ],
                                           catalog).delegation.proof
            require(bool(audit_proof), 'audit delegation proof')

            def audit_filter(page_size=20, occurred_from=None):
                value = runtime_pb2.RuntimeAuditFilter(
                    scope=platform_pb2.ResourceScope(resources=catalog),
                    page=platform_pb2.SnapshotPage(page_size=page_size))
                if occurred_from is not None:
                    value.occurred_from.CopyFrom(occurred_from)
                return value

            # Success with real data: earlier account reads/deletes/restores wrote runtime audit.
            data_page = runtime_stub.ListAudit(runtime_pb2.RuntimeListAuditRequest(
                context=context(delegation=audit_proof), filter=audit_filter()), timeout=10)
            require(len(data_page.entries) >= 1 and data_page.HasField('checked_at') and
                    all(entry.entry_id and entry.method for entry in data_page.entries),
                    'runtime audit data page through real delegation')
            future = timestamp_pb2.Timestamp(seconds=int(time.time()) + 3600)
            empty_page = runtime_stub.ListAudit(runtime_pb2.RuntimeListAuditRequest(
                context=context(delegation=audit_proof),
                filter=audit_filter(occurred_from=future)), timeout=10)
            require(len(empty_page.entries) == 0 and empty_page.HasField('checked_at') and
                    empty_page.page.page_size == 0,
                    'authorized empty runtime audit page stays a success, not 401')
            audit_proof_for_logs = issue_delegation([policy_pb2.PERMISSION_LOG_READ],
                                                    catalog).delegation.proof
            logs_page = runtime_stub.ListLogs(runtime_pb2.RuntimeListLogsRequest(
                context=context(delegation=audit_proof_for_logs),
                filter=runtime_pb2.RuntimeAuditFilter(
                    scope=platform_pb2.ResourceScope(resources=catalog),
                    page=platform_pb2.SnapshotPage(page_size=20))), timeout=10)
            require(logs_page.HasField('checked_at'), 'runtime log page through real delegation')
            # Permission mismatch: a log.read delegation cannot read audit.
            audit_denied = None
            try:
                runtime_stub.ListAudit(runtime_pb2.RuntimeListAuditRequest(
                    context=context(delegation=audit_proof_for_logs),
                    filter=audit_filter()), timeout=10)
            except grpc.RpcError as error:
                audit_denied = error.code()
            require(audit_denied == grpc.StatusCode.PERMISSION_DENIED,
                    'runtime audit requires audit.read, not log.read')
            # Structural expiry: tamper the unverified claims expiry; Runtime must reject before
            # any query (real cryptographic verification remains Identity CheckAuthorization).
            import base64
            header, payload, signature = audit_proof.split('.')
            claims = json.loads(base64.urlsafe_b64decode(
                payload + '=' * (-len(payload) % 4)))
            claims['iat'] = claims['nbf'] = claims['exp'] = 1
            expired_payload = base64.urlsafe_b64encode(
                json.dumps(claims, separators=(',', ':')).encode()).rstrip(b'=').decode()
            expired_proof = header + '.' + expired_payload + '.' + signature
            expired_denied = None
            try:
                runtime_stub.ListAudit(runtime_pb2.RuntimeListAuditRequest(
                    context=context(delegation=expired_proof), filter=audit_filter()),
                    timeout=10)
            except grpc.RpcError as error:
                expired_denied = error.code()
            require(expired_denied == grpc.StatusCode.UNAUTHENTICATED,
                    'expired/tampered delegation rejected before query')
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
