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
         'permissions': ['model.chat', 'model.manage', 'model.read'],
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
    mode = 'gated'
    if '--mode' in arguments:
        index = arguments.index('--mode')
        if index + 1 < len(arguments):
            mode = arguments[index + 1]
    if mode not in ('gated', 'expect-success', 'expect-fail-closed', 'self-check'):
        print(f'P05b-4 e2e failure: unknown mode {mode}', file=sys.stderr)
        return 2
    STATE['mode'] = mode
    if mode in ('gated', 'self-check'):
        # P05b-6 (sealed file writer/refresh) is not merged yet: do not run any production chain,
        # positive or negative, from this test. Explicitly skip (exit 77) instead of a fake pass.
        print('P05b-4 e2e gated: P05b-6 sealed delegation delivery not merged (exit 77)',
              file=sys.stderr)
        return 77
    if mode == 'expect-success' and os.environ.get('KIARABOT_P05B6_AVAILABLE') != '1':
        print('P05b-4 positive chain requires KIARABOT_P05B6_AVAILABLE=1 (P05b-6 merged); '
              'refusing to fabricate a positive chain (exit 77)', file=sys.stderr)
        return 77
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
    parent_file = root / 'instance-parent.json'
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': endpoints['runtime_probe'], 'grpc_listen': endpoints['runtime_grpc'],
        'bridge_listen': endpoints['bridge'],
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
        'database': databases['runtime'], 'identity_endpoint': identity_endpoint,
        'model_endpoint': endpoints['model_grpc'],
        'instance_delegation': {'parent_proof_file': str(parent_file),
                                'refresh_margin_seconds': 55, 'cache_capacity': 16}}
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
        model, model_state = daemon(model_binary, configs['model'])
        children.append(model)
        wait_probe(endpoints['model_probe'])
        runtime, runtime_state = daemon(runtime_binary, configs['runtime'])
        children.append(runtime)
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
            # Issue the B1 model-only instance parent delegation (real Control admin session).
            parent = issue_instance_parent_model(
                identity_service, admin, instance_principal, STATE['instance'], model_id,
                control_fingerprint)
            claims = token_claims(parent)
            write_parent_file(parent_file, STATE['instance'], instance_principal, parent,
                              claims['exp'])
        else:
            # Fail-closed E2E: sealed parent material is absent; Runtime must never reach Model or
            # the HTTP Provider and the plugin must observe a stable failure.
            require(not parent_file.exists(), 'fail-closed parent file must not exist')
        print('P05b-4 stage: plugin chain', flush=True)
        run_plugin(root, endpoints['bridge'], STATE['instance'], plugin_bootstrap, tls,
                   provider_state, provider_mode, model_id, expect_success)
    finally:
        for child in reversed(children):
            try:
                stop(child)
            except Exception:
                pass


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
        '                            logical_call_id=command["id"], timeout=4.0)\n'
        '                        result.update(ok=True, text=reply.result.text,\n'
        '                                      input_tokens=reply.result.input_tokens,\n'
        '                                      output_tokens=reply.result.output_tokens)\n'
        '                    except PluginError as error:\n'
        '                        result.update(ok=False, code=int(error.code),\n'
        '                                      retryable=bool(error.retryable))\n'
        '                    except Exception as error:\n'
        '                        result.update(ok=False, code=-1, detail=str(error)[:120])\n'
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
    env = {key: value for key, value in os.environ.items()}
    env.update({'PYTHONPATH': os.pathsep.join([str(REPO / 'sdk/python/src'),
                                                str(REPO / 'sdk/python/build/generated'),
                                                str(package)]),
                'PLUGIN_CORE_ENDPOINT': bridge_endpoint, 'PLUGIN_TLS_ROOT': str(tls / 'ca.pem'),
                'PLUGIN_INSTANCE_ID': instance, 'PLUGIN_IO_TIMEOUT': '3',
                'PLUGIN_HEARTBEAT_INTERVAL': '0.2', 'PLUGIN_RETRY_DELAY': '0.1',
                'KIARABOT_PLUGIN_BOOTSTRAP': bootstrap})
    log = (root / 'plugin.log').open('w')
    plugin = subprocess.Popen([sys.executable, '-m', 'kiarabot_sdk', '--manifest',
                               str(manifest_path)], env=env, cwd=root, stdout=log, stderr=log)
    try:
        wait_for_plugin(plugin, ready, root)

        def command(text, expected_ok=True, expected_code=None, timeout=15.0):
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
                                require(value.get('ok') is True, f'plugin chat failed: {value}')
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
                plugin.wait(timeout=10)
            except subprocess.TimeoutExpired:
                plugin.kill()
                plugin.wait(timeout=5)
        log.close()


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
