#!/usr/bin/env python3
"""P05b-4 real P11 single chain: plugin SDK -> Runtime model_chat -> Model -> real HTTP Provider.

Owned-fixture driver (run_database_test.py) supplies: root, PG port, initializer, proxy port,
then identity-bootstrap, pg-bin, identity, runtime, model and runtime-bridge-seed binaries.
The test performs a real offline Identity enrollment (Control/Runtime/Model platform peers), starts
real Identity/Model/Runtime processes over mTLS, configures the Model provider through a real
Control-origin delegation, registers the plugin instance principal, asks the real Control
administrator to issue a B1 instance parent delegation (short-lived file source), runs a real SDK
plugin over the TLS PluginBridge, and asserts the full model_chat chain plus negative matrix.

No external provider: tests/architecture/p05b_provider_fixture.py is a local real HTTP server.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import secrets
import select
import sqlite3
import signal
import socket
import ssl
import subprocess
import sys
import time
import urllib.parse
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tests/architecture'))
sys.path.insert(0, str(REPO / 'core/tests/platform'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402
import p14_native_single_host as p14  # noqa: E402

MODEL_SAN = 'spiffe://kiarabot/model-test'
STATE: dict = {}


class Failure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise Failure(label)


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def write_json(path: Path, value):
    path.write_text(json.dumps(value))
    path.chmod(0o600)


def daemon(binary, config_path):
    child = subprocess.Popen([str(binary), '--config', str(config_path)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        state = {}
        for _ in range(2):
            text = p14.line(child)
            if '=' in text:
                state.update(dict(item.split('=', 1) for item in text.split() if '=' in item))
            if 'ready' in state:
                break
        return child, state
    except Exception as error:
        tail = child.stderr.read(800).decode(errors='replace') if child.poll() is not None else ''
        raise Failure(f'{Path(binary).name} startup failed: {error}; {tail[:800]}') from None


def stop(child):
    if child is None:
        return
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
    try:
        child.wait(timeout=10)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait(timeout=5)


def wait_http(host: str, port: int, path: str = '/__health', timeout=20.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            connection = http.client.HTTPConnection(host, port, timeout=0.5)
            connection.request('GET', path)
            response = connection.getresponse()
            body = response.read()
            connection.close()
            if response.status == 200:
                return body
        except OSError:
            time.sleep(0.05)
    raise Failure(f'HTTP readiness timeout {host}:{port}')


def wait_probe(endpoint: str, timeout=20.0):
    require(rt.wait_ready(int(endpoint.rsplit(':', 1)[1]), True, timeout),
            f'readiness {endpoint}')


def pem_fingerprint(path: Path) -> bytes:
    der = ssl.PEM_cert_to_DER_cert(path.read_text())
    return hashlib.sha256(der).digest()


def token_claims(token: str) -> dict:
    _, payload, _ = token.split('.')
    return json.loads(base64.urlsafe_b64decode(payload + '=' * (-len(payload) % 4)))


def prepare(root: Path, enrolled: dict):
    tls = root / 'bootstrap-tls'
    p14.write_leaf(tls, 'model', MODEL_SAN)
    model_principal, model_credential = str(uuid.uuid4()), str(uuid.uuid4())
    enrolled['platform_peers'].append({
        'service': 'model', 'principal_id': model_principal,
        'credential_id': model_credential,
        'certificate_chain_file': str(tls / 'model.pem'), 'san': MODEL_SAN,
        'assignments': [{'assignment_id': str(uuid.uuid4()),
                         'permissions': ['identity.authorization.check'],
                         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': model_principal}]}]})
    # Exchange requires the actual Runtime delegator's explicit issuance capability.
    runtime_assignment = enrolled['platform_peers'][0]['assignments'][0]
    runtime_assignment['permissions'].append('identity.delegation.issue')
    account, installation, instance, model_id = (str(uuid.uuid4()) for _ in range(4))
    admin_model_assignment = str(uuid.uuid4())
    enrolled['administrator_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['resource.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.principal.manage'],
         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': enrolled['administrator_id']}]},
        {'assignment_id': admin_model_assignment,
         'permissions': ['model.chat', 'model.call.inspect', 'model.manage', 'model.read'],
         'scope': [{'kind': 'RESOURCE_MODEL', 'id': model_id}]},
    ])
    enrolled['control_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': installation}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.principal.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance}]},
    ])
    STATE.update(tls=tls, account=account, installation=installation, instance=instance,
                 model_id=model_id, model_principal=model_principal,
                 admin_model_assignment=admin_model_assignment)


def _grpc_modules():
    import grpc
    from kiarabot.v1 import (identity_pb2, identity_pb2_grpc, model_pb2, model_pb2_grpc,
                             platform_pb2, policy_pb2)
    return grpc, identity_pb2, identity_pb2_grpc, model_pb2, model_pb2_grpc, platform_pb2, policy_pb2


def identity_stub(endpoint, tls):
    grpc, _, identity_pb2_grpc, _, _, _, _ = _grpc_modules()
    return identity_pb2_grpc.IdentityServiceStub(rt.channel(endpoint, tls))


def model_stub(endpoint, tls):
    grpc, _, _, _, model_pb2_grpc, _, _ = _grpc_modules()
    return model_pb2_grpc.ModelServiceStub(rt.channel(endpoint, tls))


def admin_login(stub, root):
    _, identity_pb2, _, _, _, _, _ = _grpc_modules()
    password = (root / 'admin-password').read_text().strip()
    reply = stub.Login(identity_pb2.IdentityLoginRequest(
        context=rt.context(20), username='administrator', password=password), timeout=10)
    token = reply.tokens.access_token
    require(bool(token), 'admin login')
    return token


def issue(identity_stub, access, audience, scope, permissions, fingerprint, instance_id=None):
    _, identity_pb2, _, _, _, platform_pb2, _ = _grpc_modules()
    request = identity_pb2.IdentityIssueDelegationRequest(
        context=rt.context(30), subject_proof=access, audience=audience,
        scope=platform_pb2.ResourceScope(resources=scope), permissions=permissions,
        client_certificate_sha256=fingerprint)
    if instance_id is not None:
        request.instance_id = instance_id
    reply = identity_stub.IssueDelegation(request, timeout=10)
    require(bool(reply.delegation.proof), 'delegation proof')
    return reply.delegation.proof


def issue_model_manage(identity_stub, access, model_id, fingerprint):
    _, _, _, _, _, platform_pb2, policy_pb2 = _grpc_modules()
    return issue(identity_stub, access, 'model',
                 [platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_MODEL, id=model_id)],
                 [policy_pb2.PERMISSION_MODEL_MANAGE], fingerprint)


def model_update(stub, proof, config):
    _, _, _, model_pb2, _, _, _ = _grpc_modules()
    request = model_pb2.ModelUpdateModelRequest(context=rt.context(30, proof),
                                                config=model_pb2.ModelConfiguration(**config))
    reply = stub.UpdateModel(request, timeout=15)
    require(bool(reply.model.model_id), 'model update')


def model_rotate(stub, proof, model_id, secret):
    _, _, _, model_pb2, _, _, _ = _grpc_modules()
    request = model_pb2.ModelRotateProviderSecretRequest(
        context=rt.context(30, proof), model_id=model_id, new_secret=secret)
    reply = stub.RotateProviderSecret(request, timeout=15)
    require(reply.version > 0, 'provider secret rotation')


def register_instance_principal(identity_stub, instance, root, pg_bin, port):
    _, identity_pb2, _, _, _, platform_pb2, _ = _grpc_modules()
    reply = identity_stub.RegisterPrincipal(identity_pb2.IdentityRegisterPrincipalRequest(
        context=rt.context(20), principal_kind='plugin',
        binding=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE, id=instance),
        allowed_services=['runtime']), timeout=10)
    principal = reply.principal.principal_id
    require(bool(principal), 'instance principal registration')
    identity_sql = rt.postgres(root, pg_bin, port, 'p03_identity')
    rt.run_sql(identity_sql,
               "UPDATE kiarabot.principals SET state=0 WHERE id='" + principal + "'; "
               "INSERT INTO kiarabot.authorizations(subject,version,invalidation_dirty) "
               "VALUES('" + principal + "',1,false) ON CONFLICT (subject) DO NOTHING;")
    return principal


def issue_instance_parent_model(identity_stub, access, instance_principal, instance, model_id,
                                fingerprint):
    _, _, _, _, _, platform_pb2, policy_pb2 = _grpc_modules()
    parent = issue(identity_stub, access, 'runtime',
                   [platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_MODEL, id=model_id)],
                   [policy_pb2.PERMISSION_MODEL_CHAT], fingerprint, instance_id=instance)
    claims = token_claims(parent)
    require(claims.get('sub') == instance_principal, 'parent subject is instance principal')
    require(claims.get('aud') == 'runtime', 'parent audience runtime')
    require(claims.get('exp', 0) - claims.get('iat', 0) <= 56, 'parent TTL <= 56s')
    return parent


def write_parent_file(path, instance, subject, proof, expires):
    path.write_text(json.dumps({'schema_version': 1,
                                'instances': {instance: {'proof': proof, 'subject': subject,
                                                         'expires_at': int(expires)}}}))
    path.chmod(0o600)


def run(command, env=None):
    reply = subprocess.run(command, env=env, capture_output=True, timeout=60)
    if reply.returncode != 0:
        lines = [line for line in reply.stderr.decode(errors='replace').splitlines()
                 if 'seed' in line.lower() or 'error' in line.lower()]
        detail = ' | '.join(lines[:6]) if lines else reply.stderr.decode(errors='replace')[-400:]
        raise Failure(f'{Path(command[0]).name} failed code={reply.returncode}: {detail[:900]}')
    return reply


def revoke_admin_model_grants(root, pg_bin, port, assignment):
    identity_sql = rt.postgres(root, pg_bin, port, 'p03_identity')
    rt.run_sql(identity_sql,
               f"DELETE FROM kiarabot.exact_permissions WHERE assignment='{assignment}'; "
               f"DELETE FROM kiarabot.exact_roles WHERE assignment='{assignment}';")


def main() -> int:
    arguments = sys.argv[1:]
    if len(arguments) < 6:
        # run_database_test.py invokes every owned-fixture driver once against a foreign backend
        # directory (root, port, initializer, proxy only); the compiled fixtures verify ownership
        # with SQL, this pure-Python driver rejects the incomplete invocation.
        print('fixture backend directory mismatch', file=sys.stderr)
        return 1
    mode = 'expect-success'
    if '--mode' in arguments:
        index = arguments.index('--mode')
        if index + 1 < len(arguments):
            mode = arguments[index + 1]
    if mode not in ('gated', 'expect-success', 'expect-fail-closed', 'self-check'):
        print(f'P05b-4 e2e failure: unknown mode {mode}', file=sys.stderr)
        return 2
    STATE['mode'] = mode
    bootstrap_fixture.main(control_server_certificate=False, prepare=prepare,
                           after_bootstrap=after_bootstrap)
    return 0


def after_bootstrap(root: Path) -> None:
    pg_bin = Path(sys.argv[6]).resolve()
    port = int(sys.argv[2])
    initializer = str(Path(sys.argv[3]).resolve())
    identity_binary = Path(sys.argv[7]).resolve()
    runtime_binary = Path(sys.argv[8]).resolve()
    model_binary = Path(sys.argv[9]).resolve()
    seed_binary = Path(sys.argv[10]).resolve()
    model_seed_binary = Path(sys.argv[11]).resolve()
    host_binary = Path(sys.argv[12]).resolve()
    tls = STATE['tls']
    ca = str(tls / 'ca.pem')
    databases = {service: p14.create_database(root, pg_bin, port, initializer, service)
                 for service in ('runtime', 'model')}
    rt.identity_security_material(root)
    identity_config = json.loads((root / 'identity.json').read_text())
    identity_config['probe_listen'] = p14.free_endpoint()
    identity_config['grpc_listen'] = p14.free_endpoint()
    write_json(root / 'identity.json', identity_config)
    identity_endpoint = identity_config['grpc_listen']
    endpoints = {name: p14.free_endpoint() for name in
                 ('runtime_grpc', 'runtime_probe', 'model_grpc', 'model_probe', 'bridge')}
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
    p14.write_leaf(tls, 'host', 'spiffe://kiarabot/host-a')
    host_fingerprint = hashlib.sha256(
        ssl.PEM_cert_to_DER_cert((tls / 'host.pem').read_text())).hexdigest()
    parent_file = root / f'instance-delegation-{STATE["instance"]}-model.json'
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': endpoints['runtime_probe'], 'grpc_listen': endpoints['runtime_grpc'],
        'bridge_listen': endpoints['bridge'],
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
        'database': databases['runtime'], 'identity_endpoint': identity_endpoint,
        'model_endpoint': endpoints['model_grpc'],
        'model_instance_delegation_sealed_file': str(parent_file),
        'hosts': [{'host_id': 'host-a', 'certificate_sha256': host_fingerprint}]}
    configs = {'identity': root / 'identity.json', 'model': root / 'model.json',
               'runtime': root / 'runtime.json'}
    write_json(configs['model'], model_config)
    write_json(configs['runtime'], runtime_config)

    provider_state = root / 'provider-requests.jsonl'
    provider_mode = root / 'provider-mode'
    provider_mode.write_text('assert_stream_false\n')
    provider_port = free_port()
    provider = subprocess.Popen(
        [sys.executable, str(REPO / 'tests/architecture/p05b_provider_fixture.py'),
         '--host', '127.0.0.1', '--port', str(provider_port),
         '--secret', 'p05b-fixture-secret', '--mode-file', str(provider_mode),
         '--state-file', str(provider_state)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    children = [provider]
    try:
        wait_http('127.0.0.1', provider_port)
        # Seed the runtime account/adapter/plugin instance + one-time bootstrap credential before
        # the Runtime process opens its writer session.
        adapter_instance, shutdown_instance = str(uuid.uuid4()), str(uuid.uuid4())
        adapter_bootstrap, plugin_bootstrap, shutdown_bootstrap = (
            secrets.token_hex(32) for _ in range(3))
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith('PG')}
        print('P05b-4 stage: runtime seed', flush=True)
        seeded = run([str(seed_binary), str(root), str(port), adapter_instance,
                      STATE['account'], STATE['instance'], adapter_bootstrap,
                      plugin_bootstrap, STATE['installation'], shutdown_instance,
                      shutdown_bootstrap], env=environment)
        require(b'SEEDED' in seeded.stdout, 'runtime bridge seed')
        identity, identity_state = daemon(identity_binary, configs['identity'])
        children.append(identity)
        require(identity_state.get('ready') == 'true', 'identity readiness')
        wait_probe(identity_config['probe_listen'])
        model_log = (root / 'model.err').open('wb')
        model = subprocess.Popen([str(model_binary), '--config', str(configs['model'])],
                                 stdout=subprocess.PIPE, stderr=model_log)
        model_state = {}
        for _ in range(2):
            text = p14.line(model)
            if '=' in text:
                model_state.update(
                    dict(item.split('=', 1) for item in text.split() if '=' in item))
            if 'ready' in model_state:
                break
        children.append(model)
        wait_probe(endpoints['model_probe'])
        runtime_log = (root / 'runtime.err').open('wb')
        runtime = subprocess.Popen([str(runtime_binary), '--config', str(configs['runtime'])],
                                   stdout=subprocess.PIPE, stderr=runtime_log)
        try:
            runtime_state = {}
            for _ in range(2):
                text = p14.line(runtime)
                if '=' in text:
                    runtime_state.update(
                        dict(item.split('=', 1) for item in text.split() if '=' in item))
                if 'ready' in runtime_state:
                    break
        except Exception:
            pass
        children.append(runtime)
        STATE['runtime_log'] = runtime_log
        wait_probe(endpoints['runtime_probe'])

        # Real Control administrator session over the Control leaf (identity-bootstrap control.pem).
        identity_service = identity_stub(identity_endpoint, tls)
        model_service = model_stub(endpoints['model_grpc'], tls)
        print('P05b-4 stage: admin login', flush=True)
        admin = admin_login(identity_service, root)
        control_fingerprint = pem_fingerprint(tls / 'control.pem')
        model_id = STATE['model_id']
        endpoint = f'http://127.0.0.1:{provider_port}/v1/chat/completions'

        # Seed the real Model DB through the production PG/crypto adapters. The real Control
        # management RPC path is still blocked by missing scope_kinds metadata (reported
        # separately); this fixture driver never substitutes production behavior.
        print('P05b-4 stage: model seed', flush=True)
        seeded_material = run([str(model_seed_binary), model_id, 'p05b-fixture-secret',
                               str(root_key), '1', 'x', 'x'])
        material = json.loads(seeded_material.stdout.decode())
        now_ns = time.time_ns()
        model_sql = rt.postgres(root, pg_bin, port, 'p03_model')
        rt.run_sql(model_sql,
                   "INSERT INTO kiarabot.model_configs(model_id,alias,provider_kind,endpoint,"
                   "provider_model,timeout_ms,max_concurrency,lifecycle_state,config_version,"
                   "created_at,observed_at) VALUES('" + model_id + "','p05b-model',"
                   "'openai_chat_completions','" + endpoint + "','p05b-fixture-model',1000,1,"
                   "0,1," + str(now_ns) + "," + str(now_ns) + "); "
                   "INSERT INTO kiarabot.encrypted_provider_secrets(model_id,secret_version,"
                   "root_key_version,aead,nonce,sealed_secret,created_at) VALUES('" + model_id +
                   "',1,1,1,decode('" + material['nonce'] + "','hex'),decode('" +
                   material['sealed'] + "','hex')," + str(now_ns) + ");")


        # Register the plugin instance principal in Identity (P05b-2 B1 source of subject).
        print('P05b-4 stage: register instance principal', flush=True)
        instance_principal = register_instance_principal(
            identity_service, STATE['instance'], root, pg_bin, port)
        expect_success = STATE.get('mode') == 'expect-success'
        if expect_success:
            # P05b-7 production sealed writer: administrator access token feeds the real refresh
            # tool, which issues the model-only instance parent through Control mTLS and writes the
            # 0600 envelope atomically. Runtime only validates/exchanges it.
            print('P05b-4 stage: sealed model envelope', flush=True)
            token_file = root / 'admin.token'
            token_file.write_text(admin + '\n')
            token_file.chmod(0o600)
            refresh_config = root / 'delegation-refresh.json'
            write_json(refresh_config, {
                'identity_endpoint': identity_endpoint,
                'ca_file': str(tls / 'ca.pem'),
                'control_certificate_file': str(tls / 'control.pem'),
                'control_private_key_file': str(tls / 'control.key'),
                'instance_id': STATE['instance'],
                'installation_id': STATE['installation'],
                'output_dir': str(root),
                'token_file': str(token_file)})
            def refresh_parent():
                """Run the real P05b-7 refresher for one fresh 56s envelope.

                Refreshing after the plugin is registered and before every command keeps the
                short-lived parent fresh across the long negative matrix (the sealed contract
                forbids exceeding its own TTL).
                """
                refresh = subprocess.run(
                    [sys.executable, 'instance-delegation-refresh', '--config',
                     str(refresh_config), '--token-file', str(token_file), '--scope-kind',
                     'model', '--model-id', model_id, '--once'],
                    cwd=REPO / 'scripts/deploy', capture_output=True, timeout=60)
                require(refresh.returncode == 0,
                        'sealed model refresh: ' +
                        refresh.stderr.decode(errors='replace')[-500:])
                require(parent_file.is_file(), 'sealed model envelope written')
                require(parent_file.stat().st_mode & 0o777 == 0o600, 'sealed envelope 0600')

            STATE['refresh_parent'] = refresh_parent
        else:
            # Fail-closed E2E: sealed parent material is absent; Runtime must never reach Model or
            # the HTTP Provider and the plugin must observe a stable failure.
            require(not parent_file.exists(), 'fail-closed parent file must not exist')
        print('P05b-4 stage: plugin chain', flush=True)
        STATE.update(pg_bin=str(pg_bin), pg_port=port, host_binary=host_binary,
                     runtime_grpc=endpoints['runtime_grpc'],
                     bridge=endpoints['bridge'], installation=STATE['installation'])
        run_plugin(root, endpoints['bridge'], STATE['instance'], plugin_bootstrap, tls,
                   provider_state, provider_mode, model_id, expect_success)
    finally:
        import select as select_module
        for child in children:
            if child.stderr is not None:
                text = ''
                try:
                    while select_module.select([child.stderr], [], [], 0)[0]:
                        chunk = os.read(child.stderr.fileno(), 8192)
                        if not chunk:
                            break
                        text += chunk.decode(errors='replace')
                except Exception:
                    pass
                if text.strip():
                    print('P05b-4 stage: service stderr: ' + ' '.join(text.split())[-2000:],
                          file=sys.stderr)
        log_path = root / 'runtime.err'
        if log_path.is_file():
            print('P05b-4 stage: runtime log: ' + ' '.join(
                log_path.read_text(errors='replace').split())[-2500:], file=sys.stderr)
        for name in ('model', 'runtime'):
            path = root / (name + '.err')
            if path.is_file():
                print(f'P05b-4 stage: {name} stderr: ' + ' '.join(
                    path.read_text(errors='replace').split())[-3000:], file=sys.stderr)
        for child in reversed(children):
            try:
                stop(child)
            except Exception:
                pass


def free_port():
    import socket
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def managed_host_config(root, package, manifest, tls, bridge_endpoint, instance, installation,
                        bootstrap):
    credentials = root / 'credentials'
    credentials.mkdir(mode=0o700, exist_ok=True)
    response = json.dumps({'instance': {'instance_id': instance},
                           'bootstrap_credential': bootstrap})
    helper = REPO / 'scripts/deploy/target_host_credential.py'
    result = subprocess.run([sys.executable, str(helper), 'store', '--directory',
                             str(credentials)], input=response, text=True,
                            capture_output=True)
    require(result.returncode == 0, 'host credential helper failed: ' +
            str(result.stderr)[-400:])
    environment_names = ['PYTHONPATH', 'PLUGIN_CORE_ENDPOINT', 'PLUGIN_TLS_ROOT',
                         'PLUGIN_INSTANCE_ID', 'PLUGIN_IO_TIMEOUT',
                         'PLUGIN_HEARTBEAT_INTERVAL', 'PLUGIN_RETRY_DELAY',
                         'KIARABOT_PLUGIN_BOOTSTRAP']
    environment = {
        'PYTHONPATH': os.pathsep.join([str(REPO / 'sdk/python/src'),
                                       str(REPO / 'sdk/python/build/generated'),
                                       str(package)]),
        'PLUGIN_CORE_ENDPOINT': bridge_endpoint,
        'PLUGIN_TLS_ROOT': str(tls / 'ca.pem'),
        'PLUGIN_INSTANCE_ID': instance,
        'PLUGIN_IO_TIMEOUT': '3', 'PLUGIN_HEARTBEAT_INTERVAL': '0.2',
        'PLUGIN_RETRY_DELAY': '0.1'}
    runner = root / 'run_plugin.sh'
    runner.write_text('#!/bin/sh\nexec "' + sys.executable + '" -m kiarabot_sdk "$@" '
                      '>>"' + str(root / 'plugin.log') + '" 2>&1\n')
    runner.chmod(0o700)
    config = {
        'schema_version': 1, 'service': 'plugin-host',
        'probe_listen': f'127.0.0.1:{free_port()}',
        'grpc_listen': f'127.0.0.1:{free_port()}',
        'tls': {'certificate_file': str(tls / 'host.pem'),
                'private_key_file': str(tls / 'host.key'),
                'ca_file': str(tls / 'ca.pem')},
        'host': {'host_id': 'host-a', 'sqlite_path': str(root / 'host.sqlite'),
                 'runtime_endpoint': f'127.0.0.1:{STATE["runtime_grpc"].rsplit(":", 1)[1]}',
                 'credential_directory': str(credentials),
                 'packages': [{'plugin_type_id': 'event-observer', 'root': str(package),
                               'sha256': 'a' * 64, 'launch_profile': 'python',
                               'allowed_argument_names': ['--manifest']}],
                 'profiles': [{'name': 'python', 'executable': str(runner),
                               'fixed_arguments': [str(runner)],
                               'allowed_environment_names': environment_names,
                               'credential_environment': 'KIARABOT_PLUGIN_BOOTSTRAP'}],
                 'environment': environment}}
    path = root / 'host.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    return path


def seed_managed_start(root, manifest, instance, installation, pg_bin, port):
    environment, pg = rt.postgres(root, pg_bin, port, 'p03_runtime')
    arguments = ['--manifest=' + str(manifest)]
    names = ['PYTHONPATH', 'PLUGIN_CORE_ENDPOINT', 'PLUGIN_TLS_ROOT', 'PLUGIN_INSTANCE_ID',
             'PLUGIN_IO_TIMEOUT', 'PLUGIN_HEARTBEAT_INTERVAL', 'PLUGIN_RETRY_DELAY',
             'KIARABOT_PLUGIN_BOOTSTRAP']
    now_sql = '(extract(epoch from clock_timestamp())*1000000000)::bigint'
    args_sql = 'ARRAY[' + ','.join("'" + value.replace("'", "''") + "'"
                                   for value in arguments) + ']'
    env_sql = 'ARRAY[' + ','.join("'" + value + "'" for value in names) + ']'
    payload = hashlib.sha256(b'p05b4-managed-start').hexdigest()
    statement = (
        "INSERT INTO kiarabot.host_commands(command_id,host_id,instance_id,installation_id,"
        "kind,execution_generation,desired_version,desired_state,package_sha256,"
        "launch_profile,arguments,environment_names,payload_sha256,state,created_at,updated_at) "
        "VALUES('hc-p05b4-start','host-a','" + instance + "','" + installation + "',1,1,1,0,"
        "repeat('a',64),'python'," + args_sql + ',' + env_sql + ",'" + payload + "',0," +
        now_sql + ',' + now_sql + ");")
    reply = subprocess.run([str(pg / 'psql'), '-X', '-qAt', '-v', 'ON_ERROR_STOP=1'],
                           input=statement, text=True, env=environment, capture_output=True,
                           timeout=15)
    require(reply.returncode == 0, 'managed host command seed: ' +
            ' '.join(str(reply.stderr).split())[-500:])


def start_managed_host(root, host_binary):
    log = (root / 'host.err').open('wb')
    host = subprocess.Popen([str(host_binary), '--config', str(root / 'host.json')],
                            cwd=root, stdout=subprocess.DEVNULL, stderr=log)
    host._p05b_log = log  # keep the file handle alive for the process lifetime
    return host


def run_plugin(root, bridge_endpoint, instance, bootstrap, tls, provider_state, mode_file,
               model_id, expect_success):
    package = root / 'p05b-plugin'
    package.mkdir(exist_ok=True)
    commands = root / 'plugin-commands.jsonl'
    results = root / 'plugin-results.jsonl'
    ready = root / 'plugin-ready'
    commands.write_text('')
    results.write_text('')
    (package / 'subject.py').write_text(
        'import asyncio, json\n'
        'from pathlib import Path\n'
        'from kiarabot_sdk import Plugin, PluginError\n'
        'class Subject(Plugin):\n'
        '    async def on_start(self, context):\n'
        f'        Path({str(ready)!r}).write_text("ready")\n'
        '        self._task = asyncio.create_task(self._loop(context))\n'
        '    async def on_stop(self, reason):\n'
        '        self._task.cancel()\n'
        '    async def _loop(self, context):\n'
        '        seen = 0\n'
        '        while True:\n'
        '            try:\n'
        '                lines = Path(' + repr(str(commands)) + ').read_text().splitlines()\n'
        '                for line in lines[seen:]:\n'
        '                    command = json.loads(line)\n'
        '                    result = {"id": command["id"]}\n'
        '                    try:\n'
        '                        reply = await context.model_chat(\n'
        '                            command.get("model", ' + repr(model_id) + '),\n'
        '                            [{"role": "user", "text": command["text"]}],\n'
        '                            logical_call_id=command["id"], timeout=12.0)\n'
        '                        result.update(ok=True, text=reply.text,\n'
        '                                      input_tokens=reply.input_tokens,\n'
        '                                      output_tokens=reply.output_tokens)\n'
        '                    except PluginError as error:\n'
        '                        result.update(ok=False, code=int(error.code),\n'
        '                                      retryable=bool(error.retryable))\n'
        '                    except Exception as error:\n'
        '                        result.update(ok=False, code=-1, detail=repr(error)[:300])\n'
        '                    with open(' + repr(str(results)) + ', "a") as handle:\n'
        '                        handle.write(json.dumps(result) + "\\n")\n'
        '                seen = len(lines)\n'
        '            except FileNotFoundError:\n'
        '                pass\n'
        '            await asyncio.sleep(0.05)\n'
        'def create():\n'
        '    return Subject()\n')
    manifest = {'id': 'p05b.model.plugin', 'name': 'P05b model plugin', 'version': '0.1.0',
                'api_version': 'kiarabot.plugin.v1', 'runtime': 'python-rpc',
                'entrypoint': 'subject:create', 'capabilities': [],
                'isolation': 'process', 'permissions': ['model.chat']}
    manifest_path = package / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    managed_host_config(root, package, manifest_path, tls, bridge_endpoint, instance,
                        STATE['installation'], bootstrap)
    seed_managed_start(root, manifest_path, instance, STATE['installation'],
                       Path(STATE['pg_bin']), STATE['pg_port'])
    plugin = start_managed_host(root, Path(STATE['host_binary']))
    try:
        wait_for_plugin(plugin, ready, root)
        # Prove the plugin was launched by the real managed Host, not by the test: the Runtime host
        # command must be receipted and the Host journal must hold a managed execution.
        environment, pg_bin = rt.postgres(Path(root), Path(STATE['pg_bin']),
                                       int(STATE['pg_port']), 'p03_runtime')
        receipt = rt.run_sql((environment, pg_bin),
                             "SELECT state,receipt_persisted FROM kiarabot.host_commands "
                             "WHERE command_id='hc-p05b4-start'")
        require(receipt.startswith('2|t'), f'managed host start receipt: {receipt}')
        with sqlite3.connect(root / 'host.sqlite') as journal:
            executions = journal.execute('SELECT count(*) FROM managed_executions').fetchone()[0]
        require(executions >= 1, 'managed host journal execution')
        refresh_parent = STATE.get('refresh_parent')
        if refresh_parent is not None:
            refresh_parent()

        def command(text, expected_ok=True, expected_code=None, timeout=30.0):
            if refresh_parent is not None:
                refresh_parent()
            identifier = str(uuid.uuid4())
            with open(commands, 'a') as handle:
                handle.write(json.dumps({'id': identifier, 'text': text}) + '\n')
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if results.exists():
                    for line in results.read_text().splitlines():
                        value = json.loads(line)
                        if value.get('id') == identifier:
                            if expected_ok:
                                if value.get('ok') is not True:
                                    provider = ''
                                    if provider_state.is_file():
                                        provider = provider_state.read_text()[:600]
                                    log = ''
                                    try:
                                        log = (root / 'plugin.log').read_text(errors='replace')
                                    except Exception:
                                        pass
                                    raise Failure(
                                        f'plugin chat failed: {value}; provider={provider}; '
                                        f'log={" ".join(log.split())[-1200:]}')
                            else:
                                require(value.get('ok') is False, f'expected failure: {value}')
                                if expected_code is not None:
                                    require(value.get('code') == expected_code,
                                            f'expected code {expected_code}, got {value}')
                            return value
                time.sleep(0.05)
            raise Failure('plugin command timeout')

        if not expect_success:
            failed = command('no-parent-file', expected_ok=False)
            require(failed.get('code') in (2, 9),
                    f'fail-closed wire code must be UNAUTHENTICATED/UPSTREAM_UNAVAILABLE: {failed}')
            recorded = provider_state.read_text().splitlines() if provider_state.exists() else []
            require(recorded == [], 'fail-closed chain must not reach the HTTP Provider')
            print('P05b model chain fail-closed E2E passed: stable '
                  f'code={failed.get("code")}, provider requests=0')
            return
        time.sleep(2.0)
        success = command('hello')
        require(success.get('text') == 'p05b-fixture reply', 'provider reply text')
        require(success.get('input_tokens') == 7 and success.get('output_tokens') == 5,
                'provider usage tokens')
        requests = [json.loads(line) for line in provider_state.read_text().splitlines()]
        require(requests and all(entry['auth_ok'] and entry['stream'] is False
                                 for entry in requests), 'provider auth/stream=false')

        # Provider error mapping through the real chain (stable Model/plugin codes).
        matrix = [('status_401', 'UPSTREAM_UNAVAILABLE'), ('status_429', 'RATE_LIMITED'),
                  ('status_500', 'UPSTREAM_UNAVAILABLE'), ('timeout', 'TIMEOUT'),
                  ('invalid_json', 'UPSTREAM_UNAVAILABLE')]
        for mode, _expected in matrix:
            mode_file.write_text(mode + '\n')
            value = command('mode-' + mode, expected_ok=False)
            require(value.get('code') not in (None, 0, -1), f'{mode} stable code')
        mode_file.write_text('success\n')
        command('restored-success')
        print('P05b model single chain: plugin->Runtime->Model->real HTTP provider passed')
    finally:
        if plugin.poll() is None:
            plugin.send_signal(signal.SIGTERM)
            try:
                plugin.wait(timeout=15)
            except subprocess.TimeoutExpired:
                plugin.kill()
                plugin.wait(timeout=5)
        log_handle = getattr(plugin, '_p05b_log', None)
        if log_handle is not None:
            log_handle.close()


def wait_for_plugin(plugin, ready, root):
    deadline = time.monotonic() + 25.0
    while time.monotonic() < deadline:
        if plugin.poll() is not None:
            log = (root / 'plugin.log').read_text(errors='replace')[:2500]
            raise Failure(f'plugin exited early: {plugin.returncode}: {" ".join(log.split())[:2000]}')
        if ready.exists():
            return
        time.sleep(0.05)
    log = (root / 'plugin.log').read_text(errors='replace')[:2500]
    raise Failure(f'plugin never registered: {log}')


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except SystemExit:
        raise
    except Exception as error:  # noqa: BLE001 - one bounded redacted diagnostic line
        import traceback
        frames = traceback.extract_tb(sys.exc_info()[2])
        location = ' <- '.join(f'{frame.filename.rsplit("/", 1)[-1]}:{frame.lineno}'
                               for frame in frames[-4:])
        message = ' '.join(str(error).split())
        print(f'P05b-4 e2e failure: {type(error).__name__}: {message[:800]} at {location}',
              file=sys.stderr)
        raise SystemExit(1)
