#!/usr/bin/env python3
"""BOOT-05 controlled bootstrap delivery end-to-end (real native stack, owned PostgreSQL).

Runs the real Identity, Runtime and Control binaries against a disposable PG16.4 fixture:
offline Identity enrollment, private Runtime database, real Control HTTPS/REST, real Runtime owner
mTLS, the BOOT-04 fixed-directory SecretDeliveryPort sink and the BOOT-02 delivery intent/report
chain. The only plaintext exists in the 0600 sink file; this driver scans REST bodies, both child
logs and a `pg_dump --data-only` of the owner databases for the canary and never prints a secret.

Identity choice: `kiarabot_control_identity_fixture` only implements Login/ValidateSession/
IssueDelegation, while the real Runtime authorizer needs CheckAuthorization + ExchangeDelegation, so
this driver uses the real Identity binary with the existing offline-bootstrap fixture instead of
extending a Control test double (no p04/p10/p11 fixture regression risk).

Coverage: first issuance (BindAdapter opt-in recipient -> Control sink -> report -> no-secret
query), omitted-recipient legacy compatibility, idempotent replay recovery, wrong recipient rejected
before mutation, explicit reissue version advance + old credential/delivery revocation, stale
expected version, Control restart persistence, unbind followed by late reissue fail-closed, and
whole-chain secret leakage scans. Deterministic sink ACK-loss/worker-crash phases are covered by the
BOOT-03 `p12_secret_delivery` fault harness (fork crash at each durable phase); a REST-triggerable
cancellation of an issued delivery does not exist in the frozen contract (not asserted here).
"""
import hashlib
import http.client
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import time
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
sys.path.insert(0, str(REPO / 'tests/architecture'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import p14_native_single_host as p14  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402

STATE = {}


def stage(name):
    STATE['stage'] = name


def require(condition, label):
    if not condition:
        raise RuntimeError(f'BOOT-05 bootstrap delivery ({STATE.get("stage", "?")}): {label}')


def certificate_sha256(pem_path):
    der = ssl.PEM_cert_to_DER_cert(pem_path.read_text())
    return hashlib.sha256(der).hexdigest()


def sha256_text(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value))
    path.chmod(0o600)


def prepare(root, enrolled):
    p14.prepare(root, enrolled)
    STATE['tls'] = p14.STATE['tls']
    STATE.update({
        'account': str(uuid.uuid4()), 'adapter': str(uuid.uuid4()),
        'account_two': str(uuid.uuid4()), 'adapter_two': str(uuid.uuid4()),
        'installation': str(uuid.uuid4()), 'plugin_type': str(uuid.uuid4())})
    grants = [
        (['resource.read', 'account.manage'],
         [{'kind': 'RESOURCE_ACCOUNT', 'id': STATE['account']}]),
        (['resource.read', 'account.manage'],
         [{'kind': 'RESOURCE_ACCOUNT', 'id': STATE['account_two']}]),
        (['plugin.manage'],
         [{'kind': 'RESOURCE_INSTALLATION', 'id': STATE['installation']}]),
    ]
    for permissions, scope in grants:
        for key in ('administrator_assignments', 'control_assignments'):
            enrolled[key].append({'assignment_id': str(uuid.uuid4()),
                                  'permissions': permissions, 'scope': scope})


def run_seed(root, port, binaries):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('PG', 'KIARABOT_'))}
    seeded = subprocess.run(
        [str(binaries['seed']), str(root), str(port), STATE['account'], STATE['adapter'],
         STATE['account_two'], STATE['adapter_two'], STATE['installation'],
         STATE['plugin_type']],
        capture_output=True, text=True, env=environment, timeout=60)
    require(seeded.returncode == 0 and 'SEEDED' in seeded.stdout,
            f'runtime bootstrap seed rc={seeded.returncode} err={seeded.stderr.strip()[:120]}')


def start_stack(root, binaries):
    port = int(sys.argv[2])
    shared = {key: value for key, value in os.environ.items()
              if not key.startswith(('PG', 'KIARABOT_'))}
    tls = STATE['tls']
    ca = str(tls / 'ca.pem')
    stage('runtime database initialization')
    database = p14.create_database(root, binaries['pg_bin'], port,
                                   str(binaries['initializer']), 'runtime')
    stage('bootstrap seed')
    run_seed(root, port, binaries)
    stage('identity security material')
    rt.identity_security_material(root)
    stage('delivery material')
    delivery_dir = root / 'bootstrap-delivery'
    delivery_dir.mkdir(mode=0o700)
    delivery_dir.chmod(0o700)
    recipient_id = 'adapter-recipient'
    registry = {
        'schema': 1,
        'recipients': [{
            'recipient_id': recipient_id,
            'resource_kind': 'account',
            'resource_id': STATE['account'],
            'peer_id': STATE['adapter'],
            'directory': str(delivery_dir),
            'owner': f'{os.geteuid()}:{os.getegid()}',
        }],
    }
    registry_path = root / 'bootstrap-recipients.json'
    write_json(registry_path, registry)
    STATE.update(recipient_id=recipient_id, delivery_dir=delivery_dir)

    identity_config = json.loads((root / 'identity.json').read_text())
    identity_config['probe_listen'] = p14.free_endpoint()
    identity_config['grpc_listen'] = p14.free_endpoint()
    write_json(root / 'identity.json', identity_config)
    endpoints = {name: p14.free_endpoint()
                 for name in ('runtime_grpc', 'runtime_probe', 'control_probe', 'control_listen')}
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': endpoints['runtime_probe'], 'grpc_listen': endpoints['runtime_grpc'],
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
        'database': database, 'identity_endpoint': identity_config['grpc_listen'],
        'control_runtime_peers': [
            {'certificate_sha256': certificate_sha256(tls / 'control.pem')}],
    }
    control_config = {
        'schema_version': 1, 'service': 'control',
        'probe_listen': endpoints['control_probe'],
        'tls': {'certificate_file': str(tls / 'control.pem'),
                'private_key_file': str(tls / 'control.key'), 'ca_file': ca},
        'control_api': {
            'listen': endpoints['control_listen'],
            'identity_endpoint': identity_config['grpc_listen'],
            'runtime_endpoint': endpoints['runtime_grpc'],
            'secret_delivery_recipients': str(registry_path),
        },
    }
    runtime_path = root / 'bootstrap-runtime.json'
    control_path = root / 'bootstrap-control.json'
    write_json(runtime_path, runtime_config)
    write_json(control_path, control_config)
    children = {}
    try:
        stage('identity startup')
        identity, identity_state = p14.daemon(binaries['identity'], root / 'identity.json')
        children['identity'] = identity
        if identity_state.get('ready') != 'true':
            tail = ''
            if identity.poll() is not None:
                tail = identity.stderr.read(600).decode(errors='replace')
            raise RuntimeError(
                f'identity startup state={identity_state} exit={identity.poll()} {tail[:500]}')
        require(rt.wait_ready(int(identity_config['probe_listen'].rsplit(':', 1)[1]), True, 20.0),
                'identity probe readiness')
        stage('runtime startup')
        runtime, runtime_state = p14.daemon(binaries['runtime'], runtime_path)
        children['runtime'] = runtime
        require(rt.wait_ready(int(runtime_state['probe_port']), True, 20.0),
                'runtime readiness')
        stage('control startup')
        control = subprocess.Popen([str(binaries['control']), '--config', str(control_path)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=shared)
        children['control'] = control
        https_port = None
        for _ in range(4):
            text = p14.line(control)
            if text.startswith('control_https_port='):
                https_port = int(text.split('=', 1)[1])
            if 'ready' in text:
                break
        require(https_port is not None, 'control https port')
        stage('delivery scenario')
        run_scenario(root, https_port, binaries, children)
    finally:
        stage('shutdown')
        for name in ('control', 'runtime', 'identity'):
            child = children.get(name)
            if child is not None and child.poll() is None:
                try:
                    p14.stop(child)
                except Exception:  # noqa: BLE001 - bounded cleanup
                    pass
        outputs = {}
        for name, child in children.items():
            for stream_name, stream in (('stdout', child.stdout), ('stderr', child.stderr)):
                try:
                    outputs[f'{name}.{stream_name}'] = (stream.read() or b'').decode(
                        errors='replace')
                except Exception:  # noqa: BLE001
                    outputs[f'{name}.{stream_name}'] = ''
        STATE['child_outputs'] = outputs


def login(port, context, root):
    password = (root / 'admin-password').read_text().strip()
    connection = http.client.HTTPSConnection('localhost', port, context=context, timeout=15)
    try:
        connection.request('POST', '/api/v1/auth/login',
                           json.dumps({'username': 'administrator', 'password': password}),
                           {'Content-Type': 'application/json',
                            'Idempotency-Key': str(uuid.uuid4())})
        response = connection.getresponse()
        payload = json.loads(response.read())
        require(response.status == 200 and payload.get('access_token'), 'control login')
        return payload['access_token']
    finally:
        connection.close()


def run_scenario(root, https_port, binaries, children):
    context = ssl.create_default_context(cafile=str(STATE['tls'] / 'ca.pem'))
    responses = []

    def request(method, target, body=None, token=None, key=None):
        headers = {}
        encoded = None
        if body is not None:
            encoded = json.dumps(body)
            headers['Content-Type'] = 'application/json'
        if token:
            headers['Authorization'] = 'Bearer ' + token
        if key:
            headers['Idempotency-Key'] = key
        connection = http.client.HTTPSConnection('localhost', https_port, context=context,
                                                 timeout=20)
        try:
            connection.request(method, target, encoded, headers)
            response = connection.getresponse()
            payload = response.read()
            parsed = json.loads(payload) if payload else None
            responses.append(parsed)
            return response.status, parsed
        finally:
            connection.close()

    token = login(https_port, context, root)
    account = STATE['account']
    adapter = STATE['adapter']
    recipient = STATE['recipient_id']
    delivery_dir = STATE['delivery_dir']
    runtime_env = rt.postgres(root, binaries['pg_bin'], PORT_NUMBER())[0]

    def sql_runtime(statement):
        return rt.run_sql(({**runtime_env, 'PGDATABASE': 'p03_runtime'},
                           binaries['pg_bin']), statement)

    binding_body = {
        'bootstrap_recipient_id': recipient,
        'binding': {'account_id': account, 'adapter_instance_id': adapter},
        'expected_version': '1',
    }
    stage('first issuance')
    status, body = request('PUT', f'/api/v1/adapter-bindings/{account}', binding_body, token,
                           str(uuid.uuid4()))
    if status != 200 or not isinstance(body, dict):
        debug = sql_runtime(
            f"SELECT 'binding='||version||':'||state FROM kiarabot.adapter_bindings "
            f"WHERE account='{account}';"
            f"SELECT 'credentials='||count(*) FROM kiarabot.bridge_credentials "
            f"WHERE resource_id='{adapter}';"
            f"SELECT 'deliveries='||count(*) FROM kiarabot.bootstrap_deliveries "
            f"WHERE resource_id='{account}';")
        require(False,
                f'first bind status {status} body {json.dumps(body)[:200]} db={debug[:200]}')
    require('bootstrap_credential' not in body, 'ordinary REST never echoes the plaintext')
    issued = body.get('delivery')
    require(isinstance(issued, dict), 'first bind returns no-secret delivery metadata')
    require(issued.get('state') == 'BOOTSTRAP_DELIVERY_DELIVERED', 'first delivery reported')
    require(issued.get('credential_version') == '1', 'first delivery version one')
    delivery_id = issued.get('delivery_id')
    require(delivery_id, 'first delivery identity')
    secret_file = delivery_dir / f'{adapter}.bootstrap'
    require(secret_file.is_file(), 'fixed recipient file installed')
    information = secret_file.lstat()
    import stat as stat_module
    require(stat_module.S_ISREG(information.st_mode) and
            stat_module.S_IMODE(information.st_mode) == 0o600 and information.st_nlink == 1,
            'installed file is a private regular file')
    canary_one = secret_file.read_text().strip()
    require(len(canary_one) == 64 and
            all(character in '0123456789abcdef' for character in canary_one),
            'installed secret shape')
    digest_one = sha256_text(canary_one)
    require(sql_runtime(f"SELECT count(*) FROM kiarabot.bridge_credentials "
                        f"WHERE resource_id='{adapter}' AND version=1 AND NOT revoked "
                        f"AND encode(digest,'hex')='{digest_one}';") == '1',
            'runtime persists only the digest/version')

    stage('no-secret query')
    status, body = request('GET',
                           f'/api/v1/accounts/{account}/bootstrap-deliveries/{delivery_id}',
                           token=token)
    require(status == 200 and body['delivery']['state'] == 'BOOTSTRAP_DELIVERY_DELIVERED',
            'delivery metadata query')

    stage('idempotent replay recovery')
    status, replay = request('PUT', f'/api/v1/adapter-bindings/{account}', binding_body, token,
                             str(uuid.uuid4()))
    require(status == 200 and isinstance(replay, dict), 'replay status')
    require('bootstrap_credential' not in replay, 'replay never re-exposes the plaintext')
    require(replay.get('delivery', {}).get('delivery_id') == delivery_id,
            'idempotent replay restores the original delivery identity')
    require(replay['delivery'].get('credential_version') == '1',
            'idempotent replay restores the delivery version')

    stage('omitted-recipient legacy compatibility')
    legacy_body = {'binding': {'account_id': STATE['account_two'],
                               'adapter_instance_id': STATE['adapter_two']},
                   'expected_version': '1'}
    status, legacy = request('PUT', f'/api/v1/adapter-bindings/{STATE["account_two"]}',
                             legacy_body, token, str(uuid.uuid4()))
    require(status == 200 and isinstance(legacy, dict), f'legacy bind status {status}')
    require('bootstrap_credential' not in legacy, 'legacy REST path strips the plaintext')
    require(not legacy.get('delivery'), 'omitted recipient creates no delivery intent')
    require(not (delivery_dir / f'{STATE["account_two"]}.bootstrap').exists(),
            'omitted recipient writes no fixed-recipient file')

    stage('wrong recipient rejected before mutation')
    wrong_body = {'bootstrap_recipient_id': recipient,
                  'binding': {'account_id': STATE['account_two'],
                              'adapter_instance_id': STATE['adapter_two']},
                  'expected_version': '1'}
    status, wrong = request('PUT', f'/api/v1/adapter-bindings/{STATE["account_two"]}', wrong_body,
                            token, str(uuid.uuid4()))
    require(status == 400 and wrong.get('code') == 'INVALID_ARGUMENT',
            'wrong fixed recipient is rejected')
    require(sql_runtime(f"SELECT count(*) FROM kiarabot.bridge_credentials "
                        f"WHERE resource_id='{STATE['adapter_two']}';") == '1',
            'recipient pre-validation leaves the legacy credential untouched')
    require(sql_runtime(f"SELECT count(*) FROM kiarabot.bootstrap_deliveries "
                        f"WHERE resource_id='{STATE['account_two']}';") == '0',
            'wrong recipient leaves no delivery intent')

    stage('explicit reissue')
    reissue_key = str(uuid.uuid4())
    status, reissued = request(
        'POST', f'/api/v1/accounts/{account}/bootstrap/reissue',
        {'recipient_id': recipient, 'expected_credential_version': '1'}, token, reissue_key)
    require(status == 200 and isinstance(reissued, dict), f'reissue status {status}')
    require('bootstrap_credential' not in reissued, 'REST reissue never echoes the plaintext')
    require(reissued['delivery']['credential_version'] == '2',
            'explicit reissue advances the persisted credential version')
    STATE['reissue_delivery'] = reissued['delivery']['delivery_id']
    canary_two = secret_file.read_text().strip()
    STATE['canaries'] = [canary_one, canary_two]
    require(canary_two != canary_one, 'reissue installed a new credential file')
    require(len(canary_two) == 64, 'reissued secret shape')
    digest_two = sha256_text(canary_two)
    require(sql_runtime(f"SELECT count(*) FROM kiarabot.bridge_credentials "
                        f"WHERE resource_id='{adapter}' AND version=2 AND NOT revoked "
                        f"AND encode(digest,'hex')='{digest_two}';") == '1',
            'reissue persists version two digest')
    require(sql_runtime(f"SELECT count(*) FROM kiarabot.bridge_credentials "
                        f"WHERE resource_id='{adapter}' AND version=1 AND revoked;") == '1',
            'reissue revokes the previous credential version')
    status, old = request('GET',
                          f'/api/v1/accounts/{account}/bootstrap-deliveries/{delivery_id}',
                          token=token)
    require(status == 200 and old['delivery']['state'] == 'BOOTSTRAP_DELIVERY_REVOKED',
            'previous delivery intent is revoked')

    stage('reissue replay and stale expected version')
    status, replay = request(
        'POST', f'/api/v1/accounts/{account}/bootstrap/reissue',
        {'recipient_id': recipient, 'expected_credential_version': '1'}, token, reissue_key)
    require(status == 200 and 'bootstrap_credential' not in replay, 'reissue replay no secret')
    require(replay['delivery']['delivery_id'] == STATE['reissue_delivery'],
            'reissue replay restores the original delivery identity')
    status, stale = request(
        'POST', f'/api/v1/accounts/{account}/bootstrap/reissue',
        {'recipient_id': recipient, 'expected_credential_version': '1'}, token,
        str(uuid.uuid4()))
    require(status == 409, f'stale expected version becomes CONFLICT, got {status}')

    stage('unbind then late reissue fail-closed')
    status, _ = request('DELETE', f'/api/v1/adapter-bindings/{account}',
                        {'binding': {'account_id': account, 'adapter_instance_id': adapter},
                         'expected_version': '2'}, token, str(uuid.uuid4()))
    require(status == 200, f'unbind status {status}')
    status, late = request(
        'POST', f'/api/v1/accounts/{account}/bootstrap/reissue',
        {'recipient_id': recipient, 'expected_credential_version': '2'}, token,
        str(uuid.uuid4()))
    require(status in (404, 409), f'late reissue after unbind fails closed, got {status}')

    stage('control restart persistence')
    control = children['control']
    p14.stop(control)
    early = {}
    for name, stream in (('stdout', control.stdout), ('stderr', control.stderr)):
        try:
            early[f'control.{name}'] = (stream.read() or b'').decode(errors='replace')
        except Exception:  # noqa: BLE001
            early[f'control.{name}'] = ''
    STATE.setdefault('early_outputs', {}).update(early)
    restarted = subprocess.Popen(
        [str(binaries['control']), '--config', str(root / 'bootstrap-control.json')],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env={key: value for key, value in os.environ.items()
             if not key.startswith(('PG', 'KIARABOT_'))})
    children['control'] = restarted
    https_port = None
    for _ in range(4):
        text = p14.line(restarted)
        if text.startswith('control_https_port='):
            https_port = int(text.split('=', 1)[1])
        if 'ready' in text:
            break
    require(https_port is not None, 'restarted control https port')
    token = login(https_port, context, root)
    status, after = request(
        'GET', f'/api/v1/accounts/{account}/bootstrap-deliveries/{STATE["reissue_delivery"]}',
        token=token)
    require(status == 200 and after['delivery']['state'] == 'BOOTSTRAP_DELIVERY_DELIVERED',
            'delivery metadata survives Control restart')
    require(secret_file.read_text().strip() == canary_two,
            'fixed recipient file survives Control restart')

    stage('secret leakage scan')
    for canary in (canary_one, canary_two):
        for index, response in enumerate(responses):
            require(canary not in json.dumps(response), f'secret leaked in response {index}')
        for metadata in delivery_dir.iterdir():
            if metadata.name.endswith('.bootstrap'):
                continue
            require(canary not in metadata.read_text(errors='replace'),
                    'secret leaked in sink metadata')
    for database in ('p03_runtime', 'p03_identity'):
        environment, pg_bin = rt.postgres(root, binaries['pg_bin'], PORT_NUMBER(), database)
        dumped = subprocess.run(
            [str(pg_bin / 'pg_dump'), '--data-only', '--no-owner', '--no-comments'],
            capture_output=True, text=True, env=environment, timeout=60)
        require(dumped.returncode == 0, f'{database} dump for leak scan')
        for canary in (canary_one, canary_two):
            require(canary not in dumped.stdout, f'secret leaked into {database}')
    print('BOOT-05 controlled bootstrap delivery end-to-end: first issue, sink, report, '
          'replay recovery, reissue revocation, restart persistence and no-secret scan passed',
          flush=True)


def PORT_NUMBER():
    return int(sys.argv[2])


def main():
    root = Path(sys.argv[1]).resolve()
    if len(sys.argv) < 11:
        # run_database_test.py invokes every driver once against a foreign backend directory.
        raise RuntimeError('fixture backend directory mismatch')
    port = int(sys.argv[2])
    require(root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid(),
            'owned fixture root')
    require((root / 'p03-owned-marker').is_file() and not root.is_symlink(),
            'owned fixture marker')
    p14.verify_backend(root, port, sys.argv[6])
    binaries = {
        'initializer': Path(sys.argv[3]).resolve(),
        'identity_bootstrap': Path(sys.argv[5]).resolve(),
        'pg_bin': Path(sys.argv[6]).resolve(),
        'identity': Path(sys.argv[7]).resolve(),
        'runtime': Path(sys.argv[8]).resolve(),
        'control': Path(sys.argv[9]).resolve(),
        'seed': Path(sys.argv[10]).resolve(),
    }
    STATE['binaries'] = binaries
    bootstrap_fixture.main(control_server_certificate=True, prepare=prepare,
                           after_bootstrap=after_bootstrap)


def scan_child_logs():
    for canary in STATE.get('canaries', []):
        for name, output in {**STATE.get('child_outputs', {}),
                             **STATE.get('early_outputs', {})}.items():
            require(canary not in output, f'secret leaked in child {name}')


def after_bootstrap(root):
    start_stack(root, STATE['binaries'])
    scan_child_logs()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:  # noqa: BLE001 - bounded redacted diagnostic below
        message = str(error)[:600]
        if 'fixture backend directory mismatch' not in message:
            message = f'BOOT-05 controlled bootstrap delivery ({STATE.get("stage", "bootstrap")}): ' + message
        print(message, file=sys.stderr)
        sys.exit(1)
