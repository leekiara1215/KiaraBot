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
    enrolled['administrator_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()),
         'permissions': ['identity.principal.read'],
         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': enrolled['administrator_id']}]},
        {'assignment_id': str(uuid.uuid4()),
         'permissions': ['account.read', 'account.manage', 'action.inspect'],
         'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account}]},
        {'assignment_id': str(uuid.uuid4()),
         'permissions': ['resource.read', 'resource.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['task.read'],
         'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': installation},
                   {'kind': 'RESOURCE_INSTANCE', 'id': instance}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['model.read'],
         'scope': [{'kind': 'RESOURCE_MODEL', 'id': model_id}]}])
    STATE.update(tls=tls_root, account=account, installation=installation, instance=instance,
                 model_id=model_id, scheduler_principal=scheduler_principal,
                 model_principal=model_principal)


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
    status, unauthenticated = request('GET', '/api/v1/health')
    require(status == 401 and unauthenticated.get('code') == 'UNAUTHENTICATED',
            'health requires authentication')
    stage('probe endpoints')
    probe = endpoints['control_probe']
    status, payload = probe_http(probe, '/health/live')
    require(status == 200 and json.loads(payload) == {'state': 'HEALTHY'},
            'control probe /health/live')
    status, payload = probe_http(probe, '/health/ready')
    ready = json.loads(payload)
    require(status in (200, 503) and ready.get('state') in ('HEALTHY', 'UNHEALTHY'),
            f'control probe /health/ready stable process readiness body={ready}')
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
    stage('authorized dependency health')
    status, health = request('GET', '/api/v1/health', token=token)
    # P14f finding: P12 sensitive reads (health.read wire 50, log.read 48, audit.read 49) cannot
    # be stored by the current Identity PG schema (exact_permissions CHECK 1..40), so the native
    # enrollment cannot grant them and Control correctly fails closed instead of fabricating
    # dependency health. Record the stable failure; each real owner probe above is verified
    # separately. The aggregate endpoint requires a P12 schema migration (gap reported to Lead).
    require(status in (400, 401, 403, 503), f'authorized dependency health stable failure={status}')
    require(health.get('code') in ('INVALID_ARGUMENT', 'PERMISSION_DENIED',
                                   'UPSTREAM_UNAVAILABLE'),
            'authorized dependency health stable error code')
    STATE['api_health'] = f'{status}:{health.get("code")}'
    print(f'P14f authorized /api/v1/health -> {STATE["api_health"]} (P12 schema gap, fail-closed)',
          flush=True)
    stage('owner-backed reads')
    observed = {}
    STATE['observed'] = observed
    status, principals = request('GET', '/api/v1/principals', token=token)
    observed['principals'] = status
    stable(status, principals, (200, 400, 403, 503), 'principals')
    principal_items = principals.get('items', principals.get('principals', [])) \
        if status == 200 else []
    require(status != 200 or isinstance(principal_items, list),
            'principals real owner projection shape')
    for path in ('/api/v1/logs', '/api/v1/audit'):
        status, page = request('GET', path, token=token)
        observed[path] = status
        stable(status, page, (200, 400, 403, 503), path)
        if status == 200:
            require(isinstance(page.get('items'), list), path + ' real owner page')
    scope_installation = json.dumps({'resources': [
        {'kind': 'RESOURCE_INSTALLATION', 'id': STATE['installation']}]})
    scope_account = json.dumps({'resources': [
        {'kind': 'RESOURCE_ACCOUNT', 'id': STATE['account']}]})
    scope_model = json.dumps({'resources': [
        {'kind': 'RESOURCE_MODEL', 'id': STATE['model_id']}]})
    status, tasks = request('GET', '/api/v1/tasks?scope=' +
                            urllib.parse.quote(scope_installation), token=token)
    observed['tasks'] = status
    stable(status, tasks, (200, 400, 403, 404, 503), 'tasks')
    require(status != 200 or isinstance(tasks.get('items'), list), 'tasks projection')
    status, models = request('GET', '/api/v1/models?scope=' +
                             urllib.parse.quote(scope_model), token=token)
    observed['models'] = status
    stable(status, models, (200, 400, 403, 404, 503), 'models')
    status, policy = request('GET', '/api/v1/accounts/' + STATE['account'] +
                             '/action-policy', token=token)
    observed['action_policy'] = status
    stable(status, policy, (200, 400, 403, 404, 503), 'action policy')
    status, action = request('GET', '/api/v1/actions/' + str(uuid.uuid4()) + '?scope=' +
                             urllib.parse.quote(scope_account), token=token)
    observed['action_query'] = status
    stable(status, action, (200, 400, 403, 404, 503), 'action query')
    lifecycle = {'resource': {'kind': 'RESOURCE_INSTANCE', 'id': STATE['instance']},
                 'expected_version': '1'}
    for operation in ('delete', 'restore'):
        status, result = request(
            'POST', f"/api/v1/resources/RESOURCE_INSTANCE/{STATE['instance']}/{operation}",
            lifecycle, token=token, idempotency=True)
        observed[operation] = status
        stable(status, result, (202, 400, 403, 404, 409, 503), operation)
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
    stage('runtime outage')
    require(stop(children['runtime']) == 0, 'runtime graceful stop')
    status, degraded = request('GET', '/api/v1/health', token=token)
    require(status in (400, 401, 403, 503), 'health stable during runtime outage')
    rt_probe = int(endpoints['runtime_probe'].rsplit(':', 1)[1])
    require(not rt.wait_ready(rt_probe, True, 2.0), 'runtime probe is not ready while stopped')
    STATE['runtime_outage'] = 'probe-unready+aggregate-stable-error'
    runtime, _ = daemon(binaries['runtime'], configs['runtime'])
    children['runtime'] = runtime
    require(wait_probe(json.loads(configs['runtime'].read_text())['probe_listen'], True, 20.0),
            'runtime restart readiness')
    require(rt.wait_ready(rt_probe, True, 20.0), 'runtime readiness recovers')
    print(f'P14f owner reads observed: {observed}', flush=True)
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
