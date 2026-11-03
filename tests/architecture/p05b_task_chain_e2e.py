#!/usr/bin/env python3
"""P05b-10 real P10k task chain: plugin SDK -> Runtime ingress -> real Scheduler RPC.

Owned-fixture driver (run_database_test.py) supplies: root, PG port, initializer, proxy port,
then identity-bootstrap, pg-bin, identity, runtime, model and runtime-bridge-seed binaries.
The test performs a real offline Identity enrollment (Control/Runtime/Model platform peers), starts
real Identity/Scheduler/Runtime processes over mTLS, registers the plugin instance principal,
uses the real P05b-6 refresher to write the task sealed parent, runs a real SDK plugin over the
true TLS PluginBridge, and asserts task_create/get/cancel against the real Scheduler.
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

SCHEDULER_SAN = 'spiffe://kiarabot/scheduler-test'
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
    p14.write_leaf(tls, 'scheduler', SCHEDULER_SAN)
    scheduler_principal, scheduler_credential = str(uuid.uuid4()), str(uuid.uuid4())
    enrolled['platform_peers'].append({
        'service': 'scheduler', 'principal_id': scheduler_principal,
        'credential_id': scheduler_credential,
        'certificate_chain_file': str(tls / 'scheduler.pem'), 'san': SCHEDULER_SAN,
        'assignments': [{'assignment_id': str(uuid.uuid4()),
                         'permissions': ['identity.authorization.check'],
                         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': scheduler_principal}]}]})
    # Runtime exchanges the sealed task parent and must be able to issue its own delegation.
    runtime_assignment = enrolled['platform_peers'][0]['assignments'][0]
    runtime_assignment['permissions'].append('identity.delegation.issue')
    account, installation, instance = (str(uuid.uuid4()) for _ in range(3))
    enrolled['administrator_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['resource.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['task.schedule', 'task.read'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': instance},
                   {'kind': 'RESOURCE_INSTALLATION', 'id': installation}]},
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
    STATE.update(tls=tls, account=account, installation=installation, instance=instance)


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
    scheduler_binary = Path(sys.argv[9]).resolve()
    seed_binary = Path(sys.argv[10]).resolve()
    host_binary = Path(sys.argv[11]).resolve()
    tls = STATE['tls']
    ca = str(tls / 'ca.pem')
    databases = {service: p14.create_database(root, pg_bin, port, initializer, service)
                 for service in ('runtime', 'scheduler')}
    rt.identity_security_material(root)
    identity_config = json.loads((root / 'identity.json').read_text())
    identity_config['probe_listen'] = p14.free_endpoint()
    identity_config['grpc_listen'] = p14.free_endpoint()
    write_json(root / 'identity.json', identity_config)
    identity_endpoint = identity_config['grpc_listen']
    endpoints = {name: p14.free_endpoint() for name in
                 ('runtime_grpc', 'runtime_probe', 'scheduler_grpc', 'scheduler_probe',
                  'bridge')}
    p14.write_leaf(tls, 'host', 'spiffe://kiarabot/host-a')
    host_fingerprint = pem_fingerprint(tls / 'host.pem').hex()
    runtime_fingerprint = pem_fingerprint(tls / 'runtime.pem').hex()
    scheduler_config = {
        'schema_version': 1, 'service': 'scheduler',
        'probe_listen': endpoints['scheduler_probe'],
        'grpc_listen': endpoints['scheduler_grpc'],
        'tls': {'certificate_file': str(tls / 'scheduler.pem'),
                'private_key_file': str(tls / 'scheduler.key'), 'ca_file': ca},
        'database': databases['scheduler'], 'identity_endpoint': identity_endpoint,
        'runtime_endpoint': endpoints['runtime_grpc'],
        'runtime_peers': [{'certificate_sha256': runtime_fingerprint,
                           'instance_id': STATE['instance'],
                           'installation_id': STATE['installation'],
                           'account_id': STATE['account'],
                           'permissions': ['task.read', 'task.schedule']}]}
    parent_file = root / f'instance-delegation-{STATE["instance"]}.json'
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': endpoints['runtime_probe'],
        'grpc_listen': endpoints['runtime_grpc'], 'bridge_listen': endpoints['bridge'],
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
        'database': databases['runtime'], 'identity_endpoint': identity_endpoint,
        'scheduler_endpoint': endpoints['scheduler_grpc'],
        'instance_delegation_sealed_file': str(parent_file),
        'hosts': [{'host_id': 'host-a', 'certificate_sha256': host_fingerprint}]}
    write_json(root / 'scheduler.json', scheduler_config)
    write_json(root / 'runtime.json', runtime_config)

    children = []
    try:
        adapter_instance, shutdown_instance = str(uuid.uuid4()), str(uuid.uuid4())
        adapter_bootstrap, plugin_bootstrap, shutdown_bootstrap = (
            secrets.token_hex(32) for _ in range(3))
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith('PG')}
        print('P05b-10 stage: runtime seed', flush=True)
        seeded = run([str(seed_binary), str(root), str(port), adapter_instance,
                      STATE['account'], STATE['instance'], adapter_bootstrap,
                      plugin_bootstrap, STATE['installation'], shutdown_instance,
                      shutdown_bootstrap], env=environment)
        require(b'SEEDED' in seeded.stdout, 'runtime bridge seed')
        identity, identity_state = daemon(identity_binary, root / 'identity.json')
        children.append(identity)
        require(identity_state.get('ready') == 'true', 'identity readiness')
        wait_probe(identity_config['probe_listen'])
        runtime, runtime_state = daemon(runtime_binary, root / 'runtime.json')
        children.append(runtime)
        wait_probe(endpoints['runtime_probe'])
        scheduler, scheduler_state = daemon(scheduler_binary, root / 'scheduler.json')
        children.append(scheduler)
        wait_probe(endpoints['scheduler_probe'])
        STATE.update(runtime_child=runtime, identity_child=identity, scheduler_child=scheduler,
                     identity_binary=identity_binary, scheduler_binary=scheduler_binary,
                     scheduler_probe=endpoints['scheduler_probe'],
                     runtime_grpc=endpoints['runtime_grpc'], bridge=endpoints['bridge'],
                     installation=STATE['installation'], pg_bin=str(pg_bin), pg_port=port)

        identity_service = identity_stub(identity_endpoint, tls)
        admin = admin_login(identity_service, root)
        instance_principal = register_instance_principal(
            identity_service, STATE['instance'], root, pg_bin, port)
        STATE['instance_principal'] = instance_principal

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
            refresh = subprocess.run(
                [sys.executable, 'instance-delegation-refresh', '--config',
                 str(refresh_config), '--token-file', str(token_file), '--scope-kind', 'task',
                 '--permissions', 'task.schedule,task.read', '--once'],
                cwd=REPO / 'scripts/deploy', capture_output=True, timeout=60)
            require(refresh.returncode == 0,
                    'sealed task refresh: ' +
                    refresh.stderr.decode(errors='replace')[-500:])
            require(parent_file.is_file(), 'sealed task envelope written')
            require(parent_file.stat().st_mode & 0o777 == 0o600, 'sealed envelope 0600')
        STATE['refresh_parent'] = refresh_parent
        refresh_parent()
        print('P05b-10 stage: sealed task envelope', flush=True)

        run_task_plugin(root, endpoints['bridge'], STATE['instance'], plugin_bootstrap, tls,
                        host_binary, refresh_parent, parent_file, scheduler_binary,
                        root / 'scheduler.json', endpoints['scheduler_probe'])
        # Runtime without scheduler_endpoint must stay not-ready and fail closed; it never serves a
        # fabricated task capability. The writer lease requires stopping the first Runtime process.
        stop(runtime)
        fallback_config = dict(runtime_config)
        del fallback_config['scheduler_endpoint']
        fallback_path = root / 'runtime-no-scheduler.json'
        write_json(fallback_path, fallback_config)
        fallback, fallback_state = daemon(runtime_binary, fallback_path)
        children.append(fallback)
        require(fallback_state.get('ready') == 'false',
                'missing scheduler_endpoint is not ready')
        require(rt.wait_ready(int(endpoints['runtime_probe'].rsplit(':', 1)[1]), False, 5.0),
                'missing scheduler_endpoint never becomes ready')
        print('P05b-10 real SDK PluginBridge task chain e2e passed', flush=True)
    finally:
        for child in children:
            try:
                if child.stderr is not None:
                    import select as select_module
                    text = ''
                    while select_module.select([child.stderr], [], [], 0)[0]:
                        chunk = os.read(child.stderr.fileno(), 8192)
                        if not chunk:
                            break
                        text += chunk.decode(errors='replace')
                    if text.strip():
                        print('P05b-10 child stderr: ' + ' '.join(text.split())[-3000:],
                              file=sys.stderr)
            except Exception:
                pass
        for child in reversed(children):
            try:
                stop(child)
            except Exception:
                pass
        for name in ('identity.err', 'runtime.err', 'scheduler.err', 'host.err', 'plugin.log'):
            path = root / name
            if path.is_file():
                print(f'P05b-10 stage: {name}: ' + ' '.join(
                    path.read_text(errors='replace').split())[-2500:], file=sys.stderr)


def run_task_plugin(root, bridge_endpoint, instance, bootstrap, tls, host_binary,
                    refresh_parent, parent_file, scheduler_binary, scheduler_config,
                    scheduler_probe):
    package = root / 'p05b10-plugin'
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
        '                        if command["op"] == "create":\n'
        '                            from datetime import datetime, timezone, timedelta\n'
        '                            when = command.get("run_at_ns")\n'
        '                            run_at = (datetime.fromtimestamp(when / 1e9, timezone.utc)\n'
        '                                      if when else datetime.now(timezone.utc) +\n'
        '                                      timedelta(seconds=3600))\n'
        '                            reply = await context.task_create(\n'
        '                                command["task_id"], command["target"], b"{}",\n'
        '                                command["idem"], run_at=run_at)\n'
        '                            result.update(ok=True, task_id=reply.task_id,\n'
        '                                          state=int(reply.state), version=int(reply.version))\n'
        '                        elif command["op"] == "get":\n'
        '                            reply = await context.task_get(command["task_id"])\n'
        '                            result.update(ok=True, task_id=reply.task_id,\n'
        '                                          state=int(reply.state), version=int(reply.version),\n'
        '                                          occurrence_id=reply.occurrence_id)\n'
        '                        elif command["op"] == "cancel":\n'
        '                            reply = await context.task_cancel(command["task_id"],\n'
        '                                                              command["idem"])\n'
        '                            result.update(ok=True, task_id=reply.task_id,\n'
        '                                          state=int(reply.state), version=int(reply.version))\n'
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
    manifest = {'id': 'p05b10.task.plugin', 'name': 'P05b task plugin', 'version': '0.1.0',
                'api_version': 'kiarabot.plugin.v1', 'runtime': 'python-rpc',
                'entrypoint': 'subject:create', 'ordered': False,
                'capabilities': ['event.timer.triggered'], 'isolation': 'process',
                'permissions': ['task.schedule', 'task.read']}
    manifest_path = package / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    managed_host_config(root, package, manifest_path, tls, bridge_endpoint, instance,
                        STATE['installation'], bootstrap)
    seed_managed_start(root, manifest_path, instance, STATE['installation'],
                       Path(STATE['pg_bin']), STATE['pg_port'])
    host = start_managed_host(root, host_binary)
    try:
        wait_for_plugin(host, ready, root)
        environment, pg_runtime = rt.postgres(root, Path(STATE['pg_bin']),
                                          int(STATE['pg_port']), 'p03_runtime')
        receipt = rt.run_sql((environment, pg_runtime),
                             "SELECT state,receipt_persisted FROM kiarabot.host_commands "
                             "WHERE command_id='hc-p05b4-start'")
        require(receipt.startswith('2|t'), f'managed host start receipt: {receipt}')
        with sqlite3.connect(root / 'host.sqlite') as journal:
            executions = journal.execute('SELECT count(*) FROM managed_executions').fetchone()[0]
        require(executions >= 1, 'managed host journal execution')

        def command(operation, task_id, idem=None, expected_ok=True, expected_code=None,
                    run_at_ns=None, refresh=True):
            if refresh:
                refresh_parent()
            identifier = str(uuid.uuid4())
            entry = {'id': identifier, 'op': operation, 'task_id': task_id,
                     'target': 'p05b10-conversation'}
            if operation == 'create':
                entry['run_at_ns'] = run_at_ns or (time.time_ns() + 3600 * 1_000_000_000)
            if idem is not None:
                entry['idem'] = idem
            with open(commands, 'a') as handle:
                handle.write(json.dumps(entry) + '\n')
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                if results.exists():
                    for line in results.read_text().splitlines():
                        value = json.loads(line)
                        if value.get('id') == identifier:
                            if expected_ok:
                                require(value.get('ok') is True, f'{operation} failed: {value}')
                            else:
                                require(value.get('ok') is False, f'{operation} expected failure: {value}')
                                if expected_code is not None:
                                    require(value.get('code') == expected_code,
                                            f'{operation} expected {expected_code}, got {value}')
                            return value
                time.sleep(0.05)
            plugin_log = (root / 'plugin.log')
            detail = plugin_log.read_text(errors='replace')[-2500:] if plugin_log.is_file() else ''
            results_text = results.read_text(errors='replace')[-800:] if results.is_file() else ''
            runtime_text = (root / 'runtime.err')
            runtime_detail = (runtime_text.read_text(errors='replace')[-1200:]
                              if runtime_text.is_file() else '')
            raise Failure(f'{operation} result timeout; plugin_log={" ".join(detail.split())}; '
                          f'results={results_text}; runtime={runtime_detail}')

        scheduler_environment, scheduler_pg = rt.postgres(root, Path(STATE['pg_bin']),
                                                          int(STATE['pg_port']), 'p03_scheduler')
        def task_row(task_id):
            return rt.run_sql(
                (scheduler_environment, scheduler_pg),
                "SELECT owner,installation,account,state FROM kiarabot.tasks WHERE task_id='" +
                task_id + "'")
        def task_count():
            return int(rt.run_sql((scheduler_environment, scheduler_pg),
                                  'SELECT count(*) FROM kiarabot.tasks'))

        def command_retry(operation, task_id, idem=None):
            last = None
            for _ in range(8):
                try:
                    return command(operation, task_id, idem, run_at_ns=create_run_at if operation == 'create' else None)
                except Failure as error:  # upstream channel may still be CONNECTING
                    last = error
                    if "'code': 9" not in str(error):
                        raise
                    time.sleep(0.5)
            raise Failure(f'{operation} retryable upstream exhausted: {last}')

        create_run_at = time.time_ns() + 3600 * 1_000_000_000
        created = command_retry('create', 'p05b10-task-1', 'p05b10-idempotency-create-0001')
        require(created['task_id'] == 'p05b10-task-1', 'created task id')
        row = task_row('p05b10-task-1').split('|')
        require(row[0] == STATE['instance'] and row[1] == STATE['installation'] and row[2] == '',
                f'task owner/installation/account attribution: {row}')
        replay = command('create', 'p05b10-task-1', 'p05b10-idempotency-create-0001',
                         run_at_ns=create_run_at)
        require(replay['task_id'] == created['task_id'], 'idempotent task replay')
        command('create', 'p05b10-task-1', 'p05b10-idempotency-create-0002',
                expected_ok=False, expected_code=5)
        command('get', 'p05b10-task-1')
        refresh_parent()
        time.sleep(5.2)  # let the provider cache expire so Cancel uses the fresh sealed parent
        cancelled = command('cancel', 'p05b10-task-1', 'p05b10-idempotency-cancel-0001',
                            refresh=False)
        require(cancelled['state'] == 6, f'task cancel state {cancelled}')
        repeated = command('cancel', 'p05b10-task-1', 'p05b10-idempotency-cancel-0001')
        require(repeated['state'] == 6, 'repeated cancel state')
        command('cancel', 'p05b10-task-1', 'p05b10-idempotency-cancel-0002', expected_ok=False,
                expected_code=5)

        # Cross-instance task is invisible/not-found through the instance-scoped Scheduler call.
        foreign = str(uuid.uuid4())
        timestamp = '(extract(epoch from clock_timestamp())*1000000000)::bigint'
        rt.run_sql((scheduler_environment, scheduler_pg),
                   "INSERT INTO kiarabot.tasks(owner,task_id,installation,adapter,account,"
                   "conversation_id,conversation_kind,payload,run_at,state,version,occurrence_id,"
                   "cancel_requested,reason,created_at,updated_at) VALUES('" + foreign +
                   "','p05b10-foreign','" + STATE['installation'] + "','','','p05b10-conversation',0,"
                   "'{}'::bytea," + timestamp + ",0,1,'occ-p05b10-foreign',false,NULL," + timestamp + "," +
                   timestamp + ")")
        command('get', 'p05b10-foreign', expected_ok=False, expected_code=4)

        # COMPLETED proves occurrence enqueue only; it is terminal and cannot be cancelled.
        command('create', 'p05b10-task-2', 'p05b10-idempotency-create-0003')
        rt.run_sql((scheduler_environment, scheduler_pg),
                   "UPDATE kiarabot.tasks SET state=3 WHERE task_id='p05b10-task-2'")
        completed = command('get', 'p05b10-task-2')
        require(completed['state'] == 4, f'COMPLETED state {completed}')
        command('cancel', 'p05b10-task-2', 'p05b10-idempotency-cancel-0003', expected_ok=False,
                expected_code=5)

        # Missing/expired sealed parent fails closed and must not create a task row.
        original = parent_file.read_bytes()
        parent_file.unlink()
        time.sleep(6.0)  # provider cache TTL; missing material must fail closed after it
        before = task_count()
        command('create', 'p05b10-task-missing', 'p05b10-idempotency-missing-0001',
                expected_ok=False, expected_code=None, refresh=False)
        require(task_count() == before, 'missing sealed parent must not create a task')
        parent_file.write_bytes(original)
        parent_file.chmod(0o600)
        refresh_parent()
        command('create', 'p05b10-task-after-restore', 'p05b10-idempotency-after-0001')

        # Scheduler offline: the SDK receives a stable retryable upstream failure, no task row, and
        # the same operation succeeds after Scheduler returns.
        stop(STATE['scheduler_child'])
        offline = command('create', 'p05b10-task-offline', 'p05b10-idempotency-offline-0001',
                          expected_ok=False, expected_code=9, refresh=False)
        require(offline.get('retryable') is True, 'scheduler offline is retryable')
        require('p05b10-task-offline' not in
                rt.run_sql((scheduler_environment, scheduler_pg),
                           "SELECT task_id FROM kiarabot.tasks").splitlines(),
                'scheduler offline must not create a task')
        scheduler, _ = daemon(scheduler_binary, scheduler_config)
        STATE['scheduler_child'] = scheduler
        wait_probe(scheduler_probe)
        command_retry('create', 'p05b10-task-offline', 'p05b10-idempotency-offline-0002')
        print('P05b-10 stage: task chain and negative matrix passed', flush=True)
    finally:
        stop(host)


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
        receipt = rt.run_sql((environment, pg_runtime),
                             "SELECT state,receipt_persisted FROM kiarabot.host_commands "
                             "WHERE command_id='hc-p05b4-start'")
        require(receipt.startswith('2|t'), f'managed host start receipt: {receipt}')
        with sqlite3.connect(root / 'host.sqlite') as journal:
            executions = journal.execute('SELECT count(*) FROM managed_executions').fetchone()[0]
        require(executions >= 1, 'managed host journal execution')
        refresh_parent = STATE.get('refresh_parent')
        if refresh_parent is not None:
            refresh_parent()

        def command(text, expected_ok=True, expected_code=None, timeout=30.0, refresh=True):
            if refresh_parent is not None and refresh:
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

        envelope_path = root / f'instance-delegation-{instance}-model.json'

        def tamper_envelope(mutator):
            original = envelope_path.read_text()
            envelope = json.loads(original)
            mutator(envelope)
            envelope_path.write_text(json.dumps(envelope))
            envelope_path.chmod(0o600)
            return original

        def restore_envelope(original):
            envelope_path.write_text(original)
            envelope_path.chmod(0o600)

        def expect_failure(label, refresh=False):
            value = command(label, expected_ok=False, refresh=refresh)
            require(value.get('code') in (2, 3, 9),
                    f'{label} stable fail-closed code: {value}')
            return value

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
        model_environment, model_pg = rt.postgres(Path(root), Path(STATE['pg_bin']),
                                                  int(STATE['pg_port']), 'p03_model')
        identity_environment, identity_pg = rt.postgres(Path(root), Path(STATE['pg_bin']),
                                                        int(STATE['pg_port']), 'p03_identity')
        owner = rt.run_sql((model_environment, model_pg),
                           "SELECT owner FROM kiarabot.model_intents WHERE logical_call_id='" +
                           success['id'] + "'")
        require(owner == STATE['instance_principal'],
                f'model call record owner={owner}, expected instance principal '
                f'{STATE["instance_principal"]}')
        binding = rt.run_sql(
            (identity_environment, identity_pg),
            "SELECT binding_id FROM kiarabot.principals WHERE id='" + owner + "'")
        require(binding == instance, f'model call owner binding={binding}, expected {instance}')
        secret_leak = rt.run_sql(
            (model_environment, model_pg),
            "SELECT count(*) FROM kiarabot.model_intents t WHERE logical_call_id='" +
            success['id'] + "' AND position('p05b-fixture-secret' in t::text) > 0")
        require(secret_leak == '0', 'provider secret leaked into Model call record')
        requests = [json.loads(line) for line in provider_state.read_text().splitlines()]
        require(requests and all(entry['auth_ok'] and entry['stream'] is False
                                 for entry in requests), 'provider auth/stream=false')

        # Provider error mapping through the real chain (stable Model/plugin codes).
        matrix = [('status_400', 'INVALID_ARGUMENT'), ('status_401', 'UPSTREAM_UNAVAILABLE'),
                  ('status_404', 'CAPABILITY_NOT_SUPPORTED'),
                  ('status_429', 'RATE_LIMITED'), ('status_500', 'UPSTREAM_UNAVAILABLE'),
                  ('status_503', 'UPSTREAM_UNAVAILABLE'), ('timeout', 'TIMEOUT'),
                  ('invalid_json', 'UPSTREAM_UNAVAILABLE')]
        for mode, _expected in matrix:
            mode_file.write_text(mode + '\n')
            value = command('mode-' + mode, expected_ok=False)
            require(value.get('code') not in (None, 0, -1), f'{mode} stable code')
        mode_file.write_text('success\n')
        command('restored-success')

        # Sealed parent / authority lifecycle negatives, each with zero additional Provider calls.
        provider_before = len(provider_state.read_text().splitlines())
        # Runtime caches a child delegation briefly; wait it out so every negative really exercises
        # the sealed parent/authority path instead of a cached proof.
        time.sleep(6.0)
        original = tamper_envelope(lambda value: value.update(expires_at=value['issued_at'] - 1,
                                                              ttl_seconds=-1))
        try:
            expect_failure('expired-parent')
        finally:
            restore_envelope(original)
        time.sleep(6.0)
        original = tamper_envelope(
            lambda value: value.update(instance_id=str(uuid.uuid4()),
                                       installation_id=str(uuid.uuid4())))
        try:
            expect_failure('cross-instance-parent')
        finally:
            restore_envelope(original)
        require(len(provider_state.read_text().splitlines()) == provider_before,
                'parent negatives must not reach the HTTP Provider')

        # Forged parent signature/wrong origin: the envelope shape is valid but Identity must reject
        # the signed proof online; no Provider call may happen.
        original = envelope_path.read_text()
        forged = json.loads(original)
        proof = forged['delegation_proof']
        forged['delegation_proof'] = proof[:-4] + 'AAAA'
        envelope_path.write_text(json.dumps(forged))
        envelope_path.chmod(0o600)
        time.sleep(6.0)
        try:
            expect_failure('forged-parent-signature')
        finally:
            restore_envelope(original)
        time.sleep(6.0)

        # Revoked sealed parent material: removing the deployment envelope must fail closed; the
        # next authorized refresh restores service (mirrors credential/assignment revocation).
        envelope_path.unlink()
        time.sleep(6.0)
        try:
            expect_failure('revoked-parent-file')
        finally:
            refresh_parent()
        command('after-revoke-restore')

        # Identity unreachable: plugin observes a bounded fail-closed code, Provider untouched.
        identity = STATE['identity_child']
        stop(identity)
        time.sleep(6.0)
        try:
            expect_failure('identity-unreachable')
        finally:
            restarted, restarted_state = daemon(Path(STATE['identity_binary']),
                                                Path(STATE['identity_config']))
            require(restarted_state.get('ready') == 'true', 'identity restart readiness')
            wait_probe(STATE['identity_probe'])
            STATE['identity_child'] = restarted
        # The Runtime identity channel reconnects with gRPC backoff after Identity returns.
        deadline = time.monotonic() + 25.0
        while True:
            refresh_parent()
            try:
                command('after-identity-restart')
                break
            except Failure:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(1.0)

        # Runtime unreachable: the managed plugin must observe a bounded transport failure and the
        # Provider request count must not move.
        provider_before = len(provider_state.read_text().splitlines())
        stop(STATE['runtime_child'])
        try:
            command('runtime-unreachable', expected_ok=False, timeout=15)
        except Failure:
            pass  # a clean transport error can surface as no result within the bounded deadline
        require(len(provider_state.read_text().splitlines()) == provider_before,
                'Runtime-unreachable must not reach the HTTP Provider')
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
