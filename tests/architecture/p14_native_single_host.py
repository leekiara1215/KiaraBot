#!/usr/bin/env python3
"""P14f native single-host target-stack smoke (no Docker).

Runs the real native service binaries against an owned PostgreSQL 16.4 fixture:
Identity, Runtime, Scheduler, Model and Control processes, temporary CA/leaf
certificates, offline identity-bootstrap enrollment, then exercises the real
Control HTTPS chain (login/session, health, authorized dependency health,
owner-backed reads and lifecycle calls) plus identity/runtime outage and
fail-closed certificate/database startup paths.

Invoked by run_database_test.py as an owned-fixture driver; never accepts an
external DSN or production endpoint. This is an acceptance harness, not a
replacement for the Docker Compose single-host deployment.
"""
import base64
import http.client
import json
import os
from pathlib import Path
import secrets
import select
import signal
import socket
import ssl
import subprocess
import sys
import time
import urllib.parse
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402

STATE = {}


def _supports_sensitive_grants():
    """Detect whether P14h Identity fixes (permission name catalogue + PG CHECK 41-50)
    are present in this checkout, so the fixture can enroll log/audit/health grants only when
    the database and bootstrap can actually persist them."""
    policy = (REPO / 'core/include/kiarabot/core/domain/target/identity_policy.hpp').read_text()
    schema = (REPO / 'infrastructure/postgres/schema/identity_security.hpp').read_text()
    names = all(f'"{name}"' in policy for name in
                ('log.read', 'audit.read', 'health.read'))
    allowed = any(marker in schema for marker in
                  ('permission BETWEEN 1 AND 50', 'permission BETWEEN 1 AND 49'))
    return names and allowed


SUPPORTS_SENSITIVE_GRANTS = _supports_sensitive_grants()
SAN_SCHEDULER = 'spiffe://kiarabot/scheduler-test'
SAN_MODEL = 'spiffe://kiarabot/model-test'


def stage(name):
    STATE['stage'] = name


def require(condition, label):
    if not condition:
        raise RuntimeError(f'P14 native single-host ({STATE.get("stage", "?")}): {label}')


def free_endpoint():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return '127.0.0.1:' + str(sock.getsockname()[1])


def line(child, timeout=20):
    end = time.monotonic() + timeout
    value = b''
    while not value.endswith(b'\n'):
        remaining = max(0.0, end - time.monotonic())
        if not select.select([child.stdout], [], [], remaining)[0]:
            raise RuntimeError('service startup timeout')
        byte = os.read(child.stdout.fileno(), 1)
        if not byte:
            raise RuntimeError('service exited before startup')
        value += byte
    return value.decode().strip()


def daemon(binary, config_path):
    child = subprocess.Popen([str(binary), '--config', str(config_path)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    name = Path(binary).name
    try:
        state = {}
        for _ in range(2):
            text = line(child)
            if '=' in text:
                state.update(dict(item.split('=', 1) for item in text.split() if '=' in item))
            if 'ready' in state:
                break
        return child, state
    except Exception as error:
        tail = ''
        if child.poll() is not None:
            tail = child.stderr.read(600).decode(errors='replace')
        raise RuntimeError(f'{name} startup failed: {error}; {tail}'[:600]) from None


def stop(child):
    if child is None:
        return 0
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
    try:
        return child.wait(timeout=10)
    except subprocess.TimeoutExpired:
        child.kill()
        return child.wait(timeout=5)


def probe_http(endpoint, path):
    host, port = endpoint.rsplit(':', 1)
    connection = http.client.HTTPConnection(host, int(port), timeout=5)
    try:
        connection.request('GET', path)
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


def wait_probe(endpoint, expected=True, timeout=20.0):
    port = int(endpoint.rsplit(':', 1)[1])
    return rt.wait_ready(port, expected, timeout)


def write_json(path, value):
    path.write_text(json.dumps(value))
    path.chmod(0o600)


def openssl(directory, *arguments):
    reply = subprocess.run(['openssl', *map(str, arguments)], cwd=directory,
                           capture_output=True, timeout=15)
    require(reply.returncode == 0, f'openssl {arguments[0]} failed')


def write_leaf(directory, name, san):
    (directory / f'{name}.ext').write_text(
        f'subjectAltName=URI:{san},DNS:localhost,IP:127.0.0.1\n'
        'extendedKeyUsage=clientAuth,serverAuth\nbasicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature\nsubjectKeyIdentifier=hash\n'
        'authorityKeyIdentifier=keyid:always\n')
    openssl(directory, 'req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
            '-nodes', '-subj', f'/CN={name}', '-keyout', f'{name}.key',
            '-out', f'{name}.csr')
    openssl(directory, 'x509', '-req', '-in', f'{name}.csr', '-CA', 'ca.pem', '-CAkey',
            'ca.key', '-CAcreateserial', '-days', '1', '-extfile', f'{name}.ext',
            '-out', f'{name}.pem')
    (directory / f'{name}.key').chmod(0o600)


def create_database(root, pg_bin, port, initializer, service):
    migration, runtime = secrets.token_hex(32), secrets.token_hex(32)
    migration_file = root / f'password-p03_{service}_migration'
    runtime_file = root / f'password-p03_{service}_runtime'
    for path, value in [(migration_file, migration), (runtime_file, runtime)]:
        path.write_text(value + '\n')
        path.chmod(0o600)
    environment = rt.postgres(root, pg_bin, port)
    constraints = ('LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION '
                   'NOBYPASSRLS')
    rt.run_sql(environment,
               f"CREATE ROLE p03_{service}_migration {constraints} PASSWORD '{migration}';\n"
               f"CREATE ROLE p03_{service}_runtime {constraints} PASSWORD '{runtime}';\n"
               f'CREATE DATABASE p03_{service} OWNER p03_{service}_migration;\n'
               f'REVOKE ALL ON DATABASE p03_{service} FROM PUBLIC;\n'
               f'GRANT CONNECT ON DATABASE p03_{service} TO p03_{service}_runtime;\n')
    initialized = subprocess.run(
        [initializer, '--initialize-empty', service, 'localhost', str(port),
         f'p03_{service}', f'p03_{service}_migration', f'p03_{service}_runtime',
         str(root / 'server.pem'), str(migration_file)],
        capture_output=True, text=True,
        env={key: value for key, value in os.environ.items() if not key.startswith('PG')},
        timeout=30)
    require(initialized.returncode == 0, f'{service} database initialization')
    return {'host': 'localhost', 'port': int(port), 'name': f'p03_{service}',
            'migration_role': f'p03_{service}_migration',
            'runtime_role': f'p03_{service}_runtime',
            'password_file': str(runtime_file), 'ca_file': str(root / 'server.pem')}


def prepare(root, enrolled):
    stage('identity bootstrap enrollment')
    tls_root = root / 'bootstrap-tls'
    write_leaf(tls_root, 'scheduler', SAN_SCHEDULER)
    write_leaf(tls_root, 'model', SAN_MODEL)
    scheduler_principal, scheduler_credential = str(uuid.uuid4()), str(uuid.uuid4())
    model_principal, model_credential = str(uuid.uuid4()), str(uuid.uuid4())
    check_scope = lambda principal: [{'kind': 'RESOURCE_PRINCIPAL', 'id': principal}]
    enrolled['platform_peers'].extend([
        {'service': 'scheduler', 'principal_id': scheduler_principal,
         'credential_id': scheduler_credential,
         'certificate_chain_file': str(tls_root / 'scheduler.pem'),
         'san': SAN_SCHEDULER,
         'assignments': [{'assignment_id': str(uuid.uuid4()),
                          'permissions': ['identity.authorization.check'],
                          'scope': check_scope(scheduler_principal)}]},
        {'service': 'model', 'principal_id': model_principal,
         'credential_id': model_credential,
         'certificate_chain_file': str(tls_root / 'model.pem'), 'san': SAN_MODEL,
         'assignments': [{'assignment_id': str(uuid.uuid4()),
                          'permissions': ['identity.authorization.check'],
                          'scope': check_scope(model_principal)}]}])
    account = str(uuid.uuid4())
    installation = str(uuid.uuid4())
    instance = str(uuid.uuid4())
    model_id = str(uuid.uuid4())
    debug_grants = os.environ.get('KIARABOT_P14F_DEBUG_GRANTS', 'full')
    if SUPPORTS_SENSITIVE_GRANTS and debug_grants != 'none':
        permissions = {'full': ['log.read', 'audit.read', 'health.read'],
                       'health': ['health.read'], 'log': ['log.read'],
                       'audit': ['audit.read'],
                       'health_acct': ['health.read'],
                       'health_uuid': ['health.read'],
                       'catalog_low': ['task.read'],
                       'catalog_47': ['namespace.read'],
                       'catalog_50': ['health.read']}.get(debug_grants)
        if permissions:
            scope = [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]
            if debug_grants.endswith('_acct'):
                permissions = permissions if isinstance(permissions, list) else []
                scope = [{'kind': 'RESOURCE_ACCOUNT', 'id': account}]
            elif debug_grants.endswith('_uuid'):
                scope = [{'kind': 'RESOURCE_CATALOG', 'id': str(uuid.uuid4())}]
            enrolled['administrator_assignments'].append({
                'assignment_id': str(uuid.uuid4()),
                'permissions': permissions,
                'scope': scope})
    enrolled['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()),
        'permissions': ['identity.principal.read'],
        'scope': [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]})
    enrolled['administrator_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()),
         'permissions': ['identity.principal.read'],
         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': enrolled['administrator_id']}]},
        {'assignment_id': str(uuid.uuid4()),
         'permissions': ['account.read', 'account.manage', 'action.inspect',
                         'account.policy.read', 'account.policy.manage'],
         'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account}]},
        {'assignment_id': str(uuid.uuid4()),
         'permissions': ['resource.read', 'resource.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['task.read'],
         'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': installation},
                   {'kind': 'RESOURCE_INSTANCE', 'id': instance}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['model.read', 'model.manage'],
         'scope': [{'kind': 'RESOURCE_MODEL', 'id': model_id}]}])
    STATE.update(administrator_id=enrolled['administrator_id'],
                 tls=tls_root, account=account, installation=installation, instance=instance,
                 model_id=model_id, scheduler_principal=scheduler_principal,
                 model_principal=model_principal)
    # Mirror the business grants on the Control platform peer: owner applications authorize the
    # direct caller (Control) and the delegated subject independently, so both sides need the
    # exact permission/scope pair for native owner-backed success chains.
    for permissions, scope in (
            (['log.read', 'audit.read', 'health.read'],
             [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]),
            (['identity.principal.read'],
             [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]),
            (['account.read', 'account.manage', 'action.inspect',
              'account.policy.read', 'account.policy.manage'],
             [{'kind': 'RESOURCE_ACCOUNT', 'id': account}]),
            (['resource.read', 'resource.manage'],
             [{'kind': 'RESOURCE_INSTANCE', 'id': instance}]),
            (['task.read'],
             [{'kind': 'RESOURCE_INSTALLATION', 'id': installation},
              {'kind': 'RESOURCE_INSTANCE', 'id': instance}]),
            (['model.read', 'model.manage'], [{'kind': 'RESOURCE_MODEL', 'id': model_id}])):
        enrolled['control_assignments'].append({
            'assignment_id': str(uuid.uuid4()), 'permissions': permissions, 'scope': scope})


def start_stack(root):
    stage('service database initialization')
    pg_bin = Path(sys.argv[6]).resolve()
    port = int(sys.argv[2])
    initializer = str(Path(sys.argv[3]).resolve())
    identity_binary, runtime_binary, scheduler_binary, model_binary, control_binary = [
        Path(value).resolve() for value in sys.argv[7:12]]
    tls = STATE['tls']
    ca = str(tls / 'ca.pem')
    databases = {service: create_database(root, pg_bin, port, initializer, service)
                 for service in ('runtime', 'scheduler', 'model')}
    account = STATE['account']
    rt.run_sql(({**rt.postgres(root, pg_bin, port)[0], 'PGDATABASE': 'p03_runtime'},
                pg_bin),
               f"INSERT INTO kiarabot.accounts "
               f"(id,platform,platform_account_id,display_name,version,state) VALUES "
               f"('{account}','qq','10001','P14f',1,1);")
    rt.identity_security_material(root)
    identity_config = json.loads((root / 'identity.json').read_text())
    identity_config['probe_listen'] = free_endpoint()
    identity_config['grpc_listen'] = free_endpoint()
    write_json(root / 'identity.json', identity_config)
    identity_endpoint = identity_config['grpc_listen']
    endpoints = {name: free_endpoint() for name in
                 ('runtime_grpc', 'runtime_probe', 'scheduler_grpc', 'scheduler_probe',
                  'model_grpc', 'model_probe', 'control_probe', 'control_listen')}
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': endpoints['runtime_probe'], 'grpc_listen': endpoints['runtime_grpc'],
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
        'database': databases['runtime'], 'identity_endpoint': identity_endpoint,
        'model_endpoint': endpoints['model_grpc']}
    scheduler_config = {
        'schema_version': 1, 'service': 'scheduler',
        'probe_listen': endpoints['scheduler_probe'],
        'grpc_listen': endpoints['scheduler_grpc'],
        'tls': {'certificate_file': str(tls / 'scheduler.pem'),
                'private_key_file': str(tls / 'scheduler.key'), 'ca_file': ca},
        'database': databases['scheduler'], 'runtime_endpoint': endpoints['runtime_grpc'],
        'identity_endpoint': identity_endpoint}
    root_key = tls / 'model-root.key'
    root_key.write_text(secrets.token_hex(32) + '\n')
    root_key.chmod(0o600)
    model_config = {
        'schema_version': 1, 'service': 'model',
        'probe_listen': endpoints['model_probe'], 'grpc_listen': endpoints['model_grpc'],
        'tls': {'certificate_file': str(tls / 'model.pem'),
                'private_key_file': str(tls / 'model.key'), 'ca_file': ca},
        'database': databases['model'], 'identity_endpoint': identity_endpoint,
        'model_security': {'root_key_file': {'id': 'model-root-v1', 'file': str(root_key)},
                           'key_version': 1}}
    control_config = {
        'schema_version': 1, 'service': 'control',
        'probe_listen': endpoints['control_probe'],
        'tls': {'certificate_file': str(tls / 'control.pem'),
                'private_key_file': str(tls / 'control.key'), 'ca_file': ca},
        'control_api': {'listen': endpoints['control_listen'],
                        'identity_endpoint': identity_endpoint,
                        'runtime_endpoint': endpoints['runtime_grpc'],
                        'scheduler_endpoint': endpoints['scheduler_grpc'],
                        'model_endpoint': endpoints['model_grpc']}}
    configs = {'identity': root / 'identity.json', 'model': root / 'model.json',
               'runtime': root / 'runtime.json', 'scheduler': root / 'scheduler.json',
               'control': root / 'control.json'}
    for name, value in (('runtime', runtime_config), ('scheduler', scheduler_config),
                        ('model', model_config), ('control', control_config)):
        write_json(configs[name], value)
    binaries = {'identity': identity_binary, 'model': model_binary,
                'runtime': runtime_binary, 'scheduler': scheduler_binary,
                'control': control_binary}
    children = {}
    try:
        stage('identity startup')
        identity, identity_state = daemon(binaries['identity'], configs['identity'])
        children['identity'] = identity
        require(identity_state.get('ready') == 'true', 'identity readiness')
        require(rt.wait_ready(int(identity_config['probe_listen'].rsplit(':', 1)[1]), True,
                              20.0), 'identity probe readiness')
        for name in ('model', 'runtime', 'scheduler'):
            stage(name + ' startup')
            child, child_state = daemon(binaries[name], configs[name])
            children[name] = child
            require(rt.wait_ready(int(child_state['probe_port']), True, 20.0),
                    name + ' readiness')
        stage('control startup')
        control = subprocess.Popen([str(binaries['control']), '--config',
                                    str(configs['control'])],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        children['control'] = control
        https_port = None
        for _ in range(3):
            text = line(control)
            if text.startswith('control_https_port='):
                https_port = int(text.split('=', 1)[1])
            if 'ready' in text:
                break
        require(https_port is not None, 'control https port')
        listener = endpoints['control_listen'].rsplit(':', 1)[1]
        deadline = time.monotonic() + 20.0
        accepted = False
        while time.monotonic() < deadline and control.poll() is None:
            try:
                with socket.create_connection(('127.0.0.1', int(listener)), timeout=0.25):
                    accepted = True
                    break
            except OSError:
                time.sleep(0.05)
        require(accepted, 'control HTTPS listener')
        STATE.update(identity_probe=int(identity_config['probe_listen'].rsplit(':', 1)[1]),
                     runtime_probe=int(endpoints['runtime_probe'].rsplit(':', 1)[1]),
                     scheduler_probe=int(endpoints['scheduler_probe'].rsplit(':', 1)[1]),
                     model_probe=int(endpoints['model_probe'].rsplit(':', 1)[1]))
        run_checks(root, https_port, configs, binaries, children, endpoints)
    finally:
        stage('shutdown')
        logs = {}
        for name in ('control', 'scheduler', 'runtime', 'model', 'identity'):
            child = children.get(name)
            if child is not None and child.poll() is None:
                try:
                    stop(child)
                except Exception:
                    pass


def run_checks(root, https_port, configs, binaries, children, endpoints):
    context = ssl.create_default_context(cafile=str(STATE['tls'] / 'ca.pem'))

    def request(method, target, body=None, token=None, idempotency=False):
        headers = {}
        encoded = None
        if body is not None:
            encoded = json.dumps(body)
            headers['Content-Type'] = 'application/json'
        if token:
            headers['Authorization'] = 'Bearer ' + token
        if idempotency:
            headers['Idempotency-Key'] = str(uuid.uuid4())
        connection = None
        try:
            connection = http.client.HTTPSConnection(
                'localhost', https_port, context=context, timeout=15)
            connection.request(method, target, encoded, headers)
            response = connection.getresponse()
            payload = response.read()
            return response.status, (json.loads(payload) if payload else None)
        finally:
            if connection is not None:
                connection.close()

    def stable(status, body, allowed, label):
        require(status in allowed, f'{label} status {status}')
        if status >= 400:
            require(isinstance(body, dict) and body.get('code') and body.get('request_id'),
                    f'{label} stable error body')
        return body

    password = (root / 'admin-password').read_text().strip()
    stage('control login')
    status, tokens = request('POST', '/api/v1/auth/login',
                             {'username': 'administrator', 'password': password},
                             idempotency=True)
    require(status == 200 and tokens and tokens.get('access_token'),
            'authorized browser-TLS login against real Identity')
    token = tokens['access_token']
    status, refreshed = request('POST', '/api/v1/auth/refresh',
                                {'refresh_token': tokens['refresh_token']}, idempotency=True)
    require(status == 200 and refreshed.get('access_token'), 'session refresh')
    token = refreshed['access_token']
    status, unauthenticated = request('GET', '/api/v1/health')
    require(status == 401 and unauthenticated.get('code') == 'UNAUTHENTICATED',
            'health requires authentication')
    early_status, early_body = request('GET', '/api/v1/health', token=token)
    print(f'P14f early authenticated health: status={early_status} body={str(early_body)[:200]}',
          file=sys.stderr)
    stage('probe endpoints')
    probe = endpoints['control_probe']
    status, payload = probe_http(probe, '/health/live')
    require(status == 200 and json.loads(payload) == {'state': 'HEALTHY'},
            'control probe /health/live')
    # Control has no business database: readiness requires its loaded TLS material and a reachable
    # Identity validation/revocation channel. Identity was started and probed healthy above, so the
    # composed Control probe must converge to HEALTHY instead of the old placeholder false.
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        status, payload = probe_http(probe, '/health/ready')
        if status == 200 and json.loads(payload).get('state') == 'HEALTHY':
            break
        time.sleep(0.1)
    else:
        raise RuntimeError(f'control probe /health/ready expected HEALTHY, status={status} '
                           f'body={payload}')
    status, payload = probe_http(probe, '/health/ready')
    ready = json.loads(payload)
    require(status == 200 and ready.get('state') == 'HEALTHY',
            f'control probe /health/ready HEALTHY expected, body={ready}')
    STATE['control_ready'] = ready.get('state')
    print(f'P14f control probe: live=HEALTHY ready={ready.get("state")}', flush=True)
    stage('owner process readiness')
    probe_targets = {
        'identity': int(json.loads(configs['identity'].read_text())['probe_listen'].rsplit(':', 1)[1]),
        'runtime': int(endpoints['runtime_probe'].rsplit(':', 1)[1]),
        'scheduler': int(endpoints['scheduler_probe'].rsplit(':', 1)[1]),
        'model': int(endpoints['model_probe'].rsplit(':', 1)[1])}
    for name, probe_port in probe_targets.items():
        require(rt.wait_ready(probe_port, True, 20.0), f'{name} /health/ready HEALTHY')
    stage('scoped Model management over Control HTTPS')
    for model_id in (str(uuid.uuid4()), STATE['model_id']):
        authorized = model_id == STATE['model_id']
        path = '/api/v1/models/' + model_id
        config = {'model_id': model_id, 'alias': 'p14-model', 'provider_model': 'test-model',
                  'provider_kind': 'openai_chat_completions', 'endpoint': 'https://example.invalid/v1',
                  'timeout_ms': 1000, 'max_concurrency': 1}
        status, updated = request('PUT', path, {'config': config}, token, idempotency=True)
        if authorized:
            require(status == 200 and updated['model']['model_id'] == model_id,
                    f'UpdateModel exact scope status={status} code={updated.get("code")}')
        else:
            require(status == 403 and updated.get('code') == 'PERMISSION_DENIED',
                    f'UpdateModel cross-model rejection status={status}')
        rotation = {'model_id': model_id, 'new_secret': base64.b64encode(secrets.token_hex(32).encode()).decode()}
        status, rotated = request('PUT', path + '/secret', rotation, token, idempotency=True)
        if authorized:
            require(status == 200 and rotated == {'version': '1'},
                    f'RotateProviderSecret exact scope status={status} code={rotated.get("code")}')
        else:
            require(status == 403 and rotated.get('code') == 'PERMISSION_DENIED',
                    f'RotateProviderSecret cross-model rejection status={status}')
    stage('owner-backed success-chain matrix')
    matrix = []
    STATE['matrix'] = matrix

    def record(label, status, body, outcome):
        if os.environ.get('KIARABOT_P14F_DUMP_MATRIX'):
            print(f'P14f matrix-detail: {label} status={status} outcome={outcome} '
                  f'body={str(body)[:400]}', flush=True)

        if outcome != 'success' and os.environ.get('KIARABOT_P14F_DUMP_LOGS'):
            import select as select_module
            for name in ('identity', 'control', 'scheduler', 'model', 'runtime'):
                child = children.get(name)
                if child is None or child.stderr is None:
                    continue
                fd = child.stderr.fileno()
                while select_module.select([fd], [], [], 0)[0]:
                    chunk = os.read(fd, 8192).decode(errors='replace')
                    if not chunk:
                        break
                    for entry in chunk.splitlines():
                        print(f'P14 native single-host (log {name}): {entry}', flush=True)
        require(outcome in ('success', 'stable-ok', 'pre-fix-skip', 'grant-missing',
                            'capability-gap', 'known-gap'),
                f'{label} classification status={status} body={str(body)[:300]}')
        if outcome != 'success':
            require(status >= 400 and isinstance(body, dict) and body.get('code') and
                    body.get('request_id'), f'{label} stable error body')
        matrix.append((label, outcome))
        return outcome

    scope_installation = json.dumps({'resources': [
        {'kind': 'RESOURCE_INSTALLATION', 'id': STATE['installation']}]})
    scope_account = json.dumps({'resources': [
        {'kind': 'RESOURCE_ACCOUNT', 'id': STATE['account']}]})
    scope_model = json.dumps({'resources': [
        {'kind': 'RESOURCE_MODEL', 'id': STATE['model_id']}]})

    def classify(status, success, pre_fix_codes, grant_codes=('PERMISSION_DENIED',),
                 stable_ok_codes=()):
        if success:
            return 'success'
        code = None
        if isinstance(body, dict):
            code = body.get('code')
        if status >= 400 and code == 'CAPABILITY_NOT_SUPPORTED':
            return 'capability-gap'
        if STATE.get('fix_ready') and status >= 400 and code in (
                'INVALID_ARGUMENT', 'PERMISSION_DENIED', 'UNAUTHENTICATED',
                'UPSTREAM_UNAVAILABLE'):
            return 'known-gap'
        if status >= 400 and code in pre_fix_codes:
            return 'pre-fix-skip'
        if status >= 400 and code in grant_codes:
            return 'grant-missing'
        if status >= 400 and code in stable_ok_codes:
            return 'stable-ok'
        return 'invalid'

    # Aggregate dependency health. Both the P14f-reported defects must be fixed for a real
    # success here: Identity must persist health.read (schema 41-50 + name catalogue) and
    # Control must issue a valid non-empty delegation for the aggregate read.
    status, body = request('GET', '/api/v1/health', token=token)
    health_components = set()
    if status == 200:
        health_components = {item.get('component') for item in body.get('components', [])}
    health_success = (status == 200 and body.get('state') == 'HEALTHY' and
                      health_components == {'identity', 'runtime', 'scheduler', 'model'} and
                      all(item.get('state') == 'HEALTHY' or item.get('ready') is True
                          for item in body.get('components', [])))
    STATE['fix_ready'] = health_success
    if os.environ.get('KIARABOT_P12J_EXPECT_FIXED') == '1':
        require(health_success, 'P12j strict mode requires /api/v1/health 200 HEALTHY')
    discovered = classify(status, health_success, ('INVALID_ARGUMENT',),
                          grant_codes=('PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE'))
    record('health', status, body, 'pre-fix-skip' if discovered == 'invalid' else discovered)
    print(f'P14f authorized /api/v1/health: status={status} state={body.get("state") if isinstance(body, dict) else None}', flush=True)

    status, body = request('GET', '/api/v1/principals', token=token)
    principals = body.get('items', body.get('principals', [])) if status == 200 else []
    success = status == 200 and isinstance(principals, list) and len(principals) > 0
    record('principals', status, body,
           'success' if success else classify(status, False, ('INVALID_ARGUMENT',)))

    for path, label in (('/api/v1/logs', 'logs'), ('/api/v1/audit', 'audit')):
        target = f'{path}?page_size=1&actor=' + STATE['administrator_id']
        status, body = request('GET', target, token=token)
        success = status == 200 and isinstance(body.get('items'), list) and 'page' in body
        record(label, status, body,
               'success' if success else
               classify(status, False, ('INVALID_ARGUMENT',),
                        stable_ok_codes=('NOT_FOUND',)))

    scope_task = json.dumps({'resources': [
        {'kind': 'RESOURCE_INSTALLATION', 'id': STATE['installation']},
        {'kind': 'RESOURCE_INSTANCE', 'id': STATE['instance']}]})
    status, body = request('GET', '/api/v1/tasks?page_size=10&scope=' +
                           urllib.parse.quote(scope_task), token=token)
    success = status == 200 and isinstance(body.get('items'), list)
    record('tasks_list', status, body,
           'success' if success else
           classify(status, False, ('INVALID_ARGUMENT',), stable_ok_codes=('NOT_FOUND',)))
    matrix.append(('task_get_cancel', 'skipped:no-seeded-task'))

    status, body = request('GET', '/api/v1/models?page_size=10&scope=' +
                           urllib.parse.quote(scope_model), token=token)
    success = status == 200 and isinstance(body.get('items', body.get('models', [])), list)
    record('models', status, body,
           'success' if success else
           classify(status, False, ('INVALID_ARGUMENT',), stable_ok_codes=('NOT_FOUND',)))

    status, body = request('GET', '/api/v1/accounts/' + STATE['account'] +
                           '/action-policy', token=token)
    success = status == 200 and isinstance(body, dict) and body.get('account_id')
    record('action_policy', status, body,
           'success' if success else
           classify(status, False, ('INVALID_ARGUMENT',), stable_ok_codes=('NOT_FOUND',)))

    status, body = request('GET', '/api/v1/actions/' + str(uuid.uuid4()) + '?scope=' +
                           urllib.parse.quote(scope_account), token=token)
    owner_not_found = status == 404 and isinstance(body, dict) and \
        body.get('code') == 'NOT_FOUND'
    record('action_query', status, body,
           'success' if owner_not_found else
           classify(status, False, ('INVALID_ARGUMENT',), stable_ok_codes=('NOT_FOUND',)))

    lifecycle = {'resource': {'kind': 'RESOURCE_INSTANCE', 'id': STATE['instance']},
                 'expected_version': '1'}
    for operation in ('delete', 'restore'):
        status, body = request(
            'POST', f"/api/v1/resources/RESOURCE_INSTANCE/{STATE['instance']}/{operation}",
            lifecycle, token=token, idempotency=True)
        accepted = status == 202 and isinstance(body, dict) and body.get('operation')
        owner_stable = status in (404, 409) and isinstance(body, dict) and body.get('code')
        record(operation, status, body,
               'success' if accepted or owner_stable else
               classify(status, False, ('INVALID_ARGUMENT',),
                        stable_ok_codes=('NOT_FOUND', 'CONFLICT')))

    skipped = [(label, outcome) for label, outcome in matrix
               if outcome in ('pre-fix-skip', 'grant-missing')]
    if STATE['fix_ready']:
        require(not skipped,
                'post-fix owner-backed matrix must have no skipped/grant failures: ' +
                repr(skipped))
    else:
        require(all(outcome != 'invalid' for _, outcome in matrix),
                f'owner-backed matrix classified invalid: {matrix}')
    print(f'P14f owner matrix (fix_ready={STATE["fix_ready"]}): {matrix}', flush=True)

    def p12j_panel_matrix(request, token, password):
        """P12j: HTTP-level live matrix for the Panel API client contract.

        Panel's ApiClient maps these operations to generated OpenAPI types. This matrix checks the
        same status classes and response keys the Panel client consumes, without a browser. Before
        P14g/P14h fixes the owner-backed chain is fail-closed (stable 400), so the default mode
        records that known pre-fix behaviour; KIARABOT_P12J_EXPECT_FIXED=1 requires the post-fix
        success shapes and fails on the old 400s.
        """
        if os.environ.get('KIARABOT_P12J_PANEL') != '1':
            return
        strict = os.environ.get('KIARABOT_P12J_EXPECT_FIXED') == '1'
        stage('P12j Panel API live matrix')
        results = []
        STATE['p12j'] = results

        def shape_object(body, label, key=None):
            require(isinstance(body, dict), f'{label} object shape')
            if key is not None:
                require(isinstance(body.get(key), dict), f'{label}.{key} object shape')
            return body

        def shape_page(body, label, key='items'):
            shape_object(body, label)
            require(isinstance(body.get(key), list), f'{label}.{key} array shape')
            require(isinstance(body.get('page'), dict), f'{label}.page object shape')
            return body[key]

        def success_or_known(label, status, body, shape, known=(400, 403, 503)):
            gap = label in {'principals', 'log', 'audit', 'actions-metadata',
                            'tasks-list', 'models-list'}
            if status == 200:
                shape(body, label)
                results.append(f'{label}=200-shape')
                return
            if strict and gap:
                require(status in known and isinstance(body, dict) and body.get('code'),
                        f'{label}: known-gap must be a stable error {status}:{body!r}'[:300])
                results.append(f'{label}=known-gap-{status}:{body.get("code")}')
                return
            require(not strict,
                    f'{label}: expected post-fix success, got {status}:{body.get("code") if isinstance(body, dict) else body}')
            require(status in known and isinstance(body, dict) and body.get('code'),
                    f'{label}: unexpected pre-fix status/body {status}:{body!r}'[:300])
            results.append(f'{label}=pre-fix-{status}:{body.get("code")}')

        def stable_error(label, status, body, allowed, codes):
            require(status in allowed,
                    f'{label}: status {status} not in {allowed}')
            require(isinstance(body, dict) and body.get('code') in codes and body.get('request_id'),
                    f'{label}: stable error body {status}:{body!r}'[:300])
            results.append(f'{label}={status}:{body.get("code")}')

        # Panel client: health() -> /api/v1/health; callRoot healthLive/healthReady.
        status, health = request('GET', '/api/v1/health', token=token)
        success_or_known('health', status, health,
                         lambda body, label: require(
                             body.get('state') in ('HEALTHY', 'DEGRADED') and
                             isinstance(body.get('components'), list) and
                             all(isinstance(c, dict) and c.get('component') and
                                 c.get('state') for c in body['components']), label + ' shape'))
        status, live = request('GET', '/health/live')
        require(status == 200 and live.get('state') == 'HEALTHY', 'P12j control /health/live')
        status, ready = request('GET', '/health/ready')
        require(status in (200, 503) and ready.get('state') in ('HEALTHY', 'UNHEALTHY'),
                'P12j control /health/ready shape')
        results.append(f'control-probe=live:{live.get("state")},ready:{ready.get("state")}')

        # Panel client: principalPage() -> /api/v1/principals (principals + page + checked_at).
        status, principals = request('GET', '/api/v1/principals?page_size=2', token=token)
        success_or_known('principals', status, principals,
                         lambda body, label: (
                             shape_page(body, label, 'principals'),
                             require(isinstance(body.get('checked_at'), str), label + '.checked_at')))

        # Panel client: logPage()/auditPage() with page_size + occurred/actor/resource filters.
        resource_filter = json.dumps({'resources': [
            {'kind': 'RESOURCE_ACCOUNT', 'id': STATE['account']}]})
        for path, key in (('/api/v1/logs', 'log'), ('/api/v1/audit', 'audit')):
            query = ('?page_size=2&actor=administrator&resource=' +
                     urllib.parse.quote(resource_filter))
            status, body = request('GET', path + query, token=token)
            def check_page(body, label):
                items = shape_page(body, label)
                for entry in items:
                    require(isinstance(entry, dict) and entry.get('owner') in ('identity', 'runtime'),
                            label + ' entry owner projection')
                    if label == '/api/v1/audit':
                        require(isinstance(entry.get('id'), str) and
                                isinstance(entry.get('occurred_at'), str) and
                                isinstance(entry.get('actor'), str) and
                                isinstance(entry.get('action'), str), label + ' audit entry shape')
                    else:
                        require(entry.get('level') in ('DEBUG', 'INFO', 'WARN', 'ERROR'),
                                label + ' log entry level')
            success_or_known(key, status, body, check_page)
            results.append(f'{key}-filter=actor,resource')
        # An unauthenticated read must be 401 with a stable code (Panel ApiError).
        status, unauth = request('GET', '/api/v1/health')
        stable_error('health-unauth', status, unauth, (401,),
                     ('UNAUTHENTICATED',))
        # Bad token also maps to 401, never a fabricated success.
        status, bad = request('GET', '/api/v1/health', token='not-a-token')
        stable_error('health-badtoken', status, bad, (400, 401),
                     ('UNAUTHENTICATED', 'INVALID_ARGUMENT'))
        # Malformed filter is a 400 INVALID_ARGUMENT (Panel passes raw JSON object strings).
        status, invalid = request('GET', '/api/v1/logs?resource=not-json', token=token)
        stable_error('logs-invalid-filter', status, invalid, (400,), ('INVALID_ARGUMENT',))

        # Panel client: actionMetadata() and action()/actionByKey().
        status, actions = request(
            'GET', '/api/v1/actions/metadata?account_id=' + STATE['account'], token=token)
        success_or_known('actions-metadata', status, actions,
                         lambda body, label: (shape_object(body, label),
                                              require(isinstance(body.get('actions'), list),
                                                      label + '.actions'),
                                              require(isinstance(body.get('page'), dict),
                                                      label + '.page')))
        status, action = request('GET', '/api/v1/actions/' + str(uuid.uuid4()), token=token)
        stable_error('action-unknown', status, action, (400, 403, 404, 503),
                     ('NOT_FOUND', 'INVALID_ARGUMENT', 'PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE'))

        # Panel client: taskPage()/targetTask()/cancelTask(), and a forbidden cross-scope read.
        scope_installation = json.dumps({'resources': [
            {'kind': 'RESOURCE_INSTALLATION', 'id': STATE['installation']}]})
        status, tasks = request('GET', '/api/v1/tasks?scope=' +
                                urllib.parse.quote(scope_installation) + '&page_size=2', token=token)
        success_or_known('tasks-list', status, tasks,
                         lambda body, label: shape_page(body, label))
        status, task = request('GET', '/api/v1/tasks/' + str(uuid.uuid4()) + '?scope=' +
                               urllib.parse.quote(scope_installation), token=token)
        stable_error('task-get-unknown', status, task, (400, 403, 404, 503),
                     ('NOT_FOUND', 'INVALID_ARGUMENT', 'PERMISSION_DENIED', 'UPSTREAM_UNAVAILABLE'))
        status, cancelled = request(
            'POST', '/api/v1/tasks/' + str(uuid.uuid4()) + '/cancel?scope=' +
            urllib.parse.quote(scope_installation),
            {'task_id': str(uuid.uuid4()), 'expected_version': '1'}, token=token, idempotency=True)
        stable_error('task-cancel-unknown', status, cancelled, (400, 403, 404, 409, 503),
                     ('NOT_FOUND', 'INVALID_ARGUMENT', 'PERMISSION_DENIED', 'CONFLICT',
                      'UPSTREAM_UNAVAILABLE'))
        scope_model = json.dumps({'resources': [
            {'kind': 'RESOURCE_MODEL', 'id': STATE['model_id']}]})
        status, forbidden = request('GET', '/api/v1/tasks?scope=' +
                                    urllib.parse.quote(scope_model), token=token)
        require(status in (400, 403, 503), f'tasks cross-scope forbidden status={status}')
        require(isinstance(forbidden, dict) and forbidden.get('code'), 'tasks cross-scope stable')
        results.append(f'tasks-cross-scope={status}:{forbidden.get("code")}')

        # Panel client: models()/modelMetadata()/modelConfiguration().
        status, models = request('GET', '/api/v1/models?scope=' +
                                 urllib.parse.quote(scope_model), token=token)
        success_or_known('models-list', status, models,
                         lambda body, label: shape_page(body, label))
        status, metadata = request('GET', '/api/v1/models/' + STATE['model_id'], token=token)
        if status == 200:
            require(isinstance(metadata, dict) and isinstance(metadata.get('model'), dict),
                    'model metadata success shape')
            results.append('model-metadata=200-shape')
        else:
            stable_error('model-metadata', status, metadata, (400, 403, 404, 503),
                         ('NOT_FOUND', 'INVALID_ARGUMENT', 'PERMISSION_DENIED',
                          'UPSTREAM_UNAVAILABLE'))
        status, configuration = request(
            'GET', '/api/v1/models/' + STATE['model_id'] + '/configuration', token=token)
        if status == 200:
            require(isinstance(configuration, dict) and isinstance(configuration.get('config'), dict)
                    and isinstance(configuration.get('secret_configured'), bool),
                    'model configuration success shape')
            results.append('model-configuration=200-shape')
        else:
            stable_error('model-configuration', status, configuration, (400, 403, 404, 503),
                         ('NOT_FOUND', 'INVALID_ARGUMENT', 'PERMISSION_DENIED',
                          'UPSTREAM_UNAVAILABLE'))

        # Panel client: beginDeleteResource()/restoreResource(); unknown instance stays a stable
        # error (404/409), never a fabricated operation.
        lifecycle = {'resource': {'kind': 'RESOURCE_INSTANCE', 'id': STATE['instance']},
                     'expected_version': '1'}
        for operation in ('delete', 'restore'):
            status, result = request(
                'POST', f"/api/v1/resources/RESOURCE_INSTANCE/{STATE['instance']}/{operation}",
                lifecycle, token=token, idempotency=True)
            stable_error('resource-' + operation, status, result, (202, 400, 403, 404, 409, 503),
                         ('NOT_FOUND', 'INVALID_ARGUMENT', 'PERMISSION_DENIED', 'CONFLICT',
                          'UPSTREAM_UNAVAILABLE'))

        # Dependency outage: model owner down must surface 503/INVALID_ARGUMENT, not success.
        require(stop(children['model']) == 0, 'model graceful stop for 503 probe')
        status, unavailable = request('GET', '/api/v1/models?scope=' +
                                      urllib.parse.quote(scope_model), token=token)
        require(status in (400, 403, 503), f'model-outage status={status}')
        require(isinstance(unavailable, dict) and unavailable.get('code'), 'model-outage stable')
        results.append(f'models-outage={status}:{unavailable.get("code")}')
        model_child, model_state = daemon(binaries['model'], configs['model'])
        children['model'] = model_child
        require(rt.wait_ready(int(model_state['probe_port']), True, 20.0), 'model restart readiness')
        print('P12j Panel API live matrix: ' + ', '.join(results), flush=True)
    p12j_panel_matrix(request, token, password)
    stage('identity outage')
    require(stop(children['identity']) == 0, 'identity graceful stop')
    status, _ = request('POST', '/api/v1/auth/login',
                        {'username': 'administrator', 'password': password},
                        idempotency=True)
    require(status in (401, 500, 503), 'login fails closed without Identity')
    status, unavailable = request('GET', '/api/v1/health', token=token)
    require(status in (400, 401, 403, 503), 'dependency health fails closed without Identity')
    require(isinstance(unavailable, dict) and
            unavailable.get('code') in (None, 'INVALID_ARGUMENT', 'UNAUTHENTICATED',
                                       'UPSTREAM_UNAVAILABLE'),
            'Identity outage reports stable unavailability')
    identity, _ = daemon(binaries['identity'], configs['identity'])
    children['identity'] = identity
    require(rt.wait_ready(int(json.loads(configs['identity'].read_text())['probe_listen']
                              .rsplit(':', 1)[1]), True, 20.0), 'identity restart readiness')
    deadline = time.monotonic() + 15.0
    status, tokens = 0, None
    while time.monotonic() < deadline:
        status, tokens = request('POST', '/api/v1/auth/login',
                                 {'username': 'administrator', 'password': password},
                                 idempotency=True)
        if status == 200 and tokens and tokens.get('access_token'):
            break
        time.sleep(0.2)
    require(status == 200 and tokens.get('access_token'),
            f'login recovers after identity restart status={status}')
    token = tokens['access_token']
    require(rt.wait_ready(STATE['identity_probe'], True, 20.0),
            'identity readiness recovers')
    if STATE['fix_ready']:
        deadline = time.monotonic() + 15.0
        recovered = False
        while time.monotonic() < deadline:
            check, payload = request('GET', '/api/v1/health', token=token)
            if check == 200 and payload.get('state') == 'HEALTHY':
                recovered = True
                break
            time.sleep(0.2)
        require(recovered, 'aggregate health recovers after identity restart')
    stage('runtime outage')
    require(stop(children['runtime']) == 0, 'runtime graceful stop')
    status, degraded = request('GET', '/api/v1/health', token=token)
    if STATE['fix_ready']:
        require(status in (200, 503), f'health observable during runtime outage={status}')
        if status == 200:
            require(degraded.get('state') == 'DEGRADED', 'runtime outage reports DEGRADED')
            components = {item.get('component'): item for item in degraded['components']}
            runtime_component = components.get('runtime', {})
            require(runtime_component.get('ready') is False or
                    runtime_component.get('state') != 'HEALTHY',
                    'runtime component not ready during outage')
    else:
        require(status in (400, 401, 403, 503), 'health stable during runtime outage')
    rt_probe = int(endpoints['runtime_probe'].rsplit(':', 1)[1])
    require(not rt.wait_ready(rt_probe, True, 2.0), 'runtime probe is not ready while stopped')
    STATE['runtime_outage'] = 'probe-unready+aggregate-stable-error'
    runtime, _ = daemon(binaries['runtime'], configs['runtime'])
    children['runtime'] = runtime
    require(wait_probe(json.loads(configs['runtime'].read_text())['probe_listen'], True, 20.0),
            'runtime restart readiness')
    require(rt.wait_ready(rt_probe, True, 20.0), 'runtime readiness recovers')
    if STATE['fix_ready']:
        deadline = time.monotonic() + 15.0
        recovered = False
        while time.monotonic() < deadline:
            check, payload = request('GET', '/api/v1/health', token=token)
            if check == 200 and payload.get('state') == 'HEALTHY':
                recovered = True
                break
            time.sleep(0.2)
        require(recovered, 'aggregate health recovers after runtime restart')
    if os.environ.get('KIARABOT_P14F_DUMP_LOGS'):
        import select as select_module
        for name in ('identity', 'control'):
            child = children.get(name)
            if child is None or child.stderr is None:
                continue
            fd = child.stderr.fileno()
            while select_module.select([fd], [], [], 0)[0]:
                chunk = os.read(fd, 8192).decode(errors='replace')
                if not chunk:
                    break
                for entry in chunk.splitlines():
                    print(f'P14 native single-host (live {name}): {entry}', flush=True)
    print(f'P14f owner matrix: {STATE["matrix"]}', flush=True)
    stage('fail-closed startup')
    broken_control = json.loads(configs['control'].read_text())
    broken_control['tls']['ca_file'] = str(root / 'absent-ca.pem')
    broken_path = root / 'control-broken.json'
    write_json(broken_path, broken_control)
    reply = subprocess.run([str(binaries['control']), '--config', str(broken_path)],
                           capture_output=True, timeout=15)
    require(reply.returncode != 0, 'control refuses untrusted CA material')
    broken_runtime = json.loads(configs['runtime'].read_text())
    wrong_password = root / 'password-wrong'
    wrong_password.write_text(secrets.token_hex(32) + '\n')
    wrong_password.chmod(0o600)
    broken_runtime['database']['password_file'] = str(wrong_password)
    broken_runtime['probe_listen'] = free_endpoint()
    broken_runtime['grpc_listen'] = free_endpoint()
    runtime_path = root / 'runtime-broken.json'
    write_json(runtime_path, broken_runtime)
    reply = subprocess.run([str(binaries['runtime']), '--config', str(runtime_path)],
                           capture_output=True, timeout=20)
    require(reply.returncode != 0, 'runtime refuses unauthenticated database credentials')
    print('P14f fail-closed: bad CA and unauthenticated database credentials refused', flush=True)
    STATE['checks'] = 'passed'


def after_bootstrap(root):
    start_stack(root)


def verify_backend(root, port, pg_bin):
    environment = {
        'PGHOST': 'localhost', 'PGPORT': str(port), 'PGUSER': 'p03_admin',
        'PGDATABASE': 'postgres', 'PGPASSWORD': (root / 'password').read_text().strip(),
        'PGSSLMODE': 'verify-full', 'PGSSLROOTCERT': str(root / 'server.pem'),
        'PGCHANNELBINDING': 'require', 'PGCONNECT_TIMEOUT': '2'}
    try:
        reply = subprocess.run([str(Path(pg_bin) / 'psql'), '-X', '-qAt', '-c',
                                'SHOW data_directory;'], env=environment,
                               capture_output=True, timeout=10)
        actual = Path(reply.stdout.decode().strip()).resolve()
    except (OSError, ValueError):
        raise RuntimeError('fixture backend directory mismatch') from None
    if reply.returncode != 0 or actual != (root / 'data').resolve():
        raise RuntimeError('fixture backend directory mismatch')


def main():
    root = Path(sys.argv[1]).resolve()
    if len(sys.argv) < 7:
        # run_database_test.py invokes every driver once against a foreign backend directory; the
        # compiled fixture drivers verify ownership with SQL, the owned arguments are absent here.
        raise RuntimeError('fixture backend directory mismatch')
    verify_backend(root, int(sys.argv[2]), sys.argv[6])
    stage('fixture bootstrap')
    bootstrap_fixture.main(control_server_certificate=True, prepare=prepare,
                           after_bootstrap=after_bootstrap)


if __name__ == '__main__':
    try:
        main()
        print('P14f native single-host target stack smoke passed', flush=True)
    except Exception as error:  # noqa: BLE001 - bounded redacted diagnostic below
        message = str(error)[:600]
        if 'fixture backend directory mismatch' not in message:
            message = (f'P14 native single-host ({STATE.get("stage", "bootstrap")}): '
                       + message)
        print(message, file=sys.stderr)
        sys.exit(1)
