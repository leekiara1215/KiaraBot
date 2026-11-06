#!/usr/bin/env python3
"""P12 WS real-scale directory boundaries (owned PostgreSQL, real owner processes).

Two real-process boundaries for the Control WS adapters directory:
  * >126 authorized children: the compiled seed opens 127 real scale adapter bindings/credentials;
    this test registers all 127 adapters through the real AdapterBridge (one framed registration per
    binding), grants the administrator explicit adapter.read on the deployment adapter catalog plus
    the 127 exact adapter resources, then opens `/ws/adapters` in "exactly one deployment
    RESOURCE_CATALOG" directory mode. Control's catalog expansion must refuse with RATE_LIMITED
    (WS close 1013) instead of truncating or returning a successful empty/partial stream.
  * large-directory budget measurement: with the same real 127 adapters, a direct owner
    ListAdapters full immutable scan (catalog + 126 exact granted children, page_size=100) is timed
    page by page; the numbers back the docs/architecture.md operational guidance on how many authorized children
    a single directory scan may expose under the frozen WS snapshot poll budget.

Everything runs against the real Identity/Runtime/Control binaries, real owned PG16.4 and real
temporary mTLS material. No fixture owner pagination is involved.
"""
import asyncio
import hashlib
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
import p12_ws_owner_directory_e2e as baseline  # noqa: E402
import p06_bridge_acceptance as p06  # noqa: E402
import p14_native_single_host as p14  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402

import grpc  # noqa: E402
from kiarabot.v1 import (common_pb2, identity_pb2, identity_pb2_grpc, platform_pb2,  # noqa: E402
                         policy_pb2, runtime_pb2, runtime_pb2_grpc)

STATE = {}
SCALE_COUNT = 127
ORIGIN = 'https://localhost'


def stage(name):
    STATE['stage'] = name


def require(condition, label):
    if not condition:
        raise RuntimeError(f'P12 WS directory scale e2e ({STATE.get("stage", "?")}): {label}')


def scale_adapter_id(index):
    """Deterministic RFC4122 v4-shaped adapter id shared with the enrollment grants."""
    digest = hashlib.sha256(f'p12-scale-adapter-{index}'.encode()).hexdigest()
    return (f'{digest[0:8]}-{digest[8:12]}-4{digest[13:16]}-'
            f'{"89ab"[int(digest[16], 16) % 4]}{digest[17:20]}-{digest[20:32]}')


def rpc_context(proof, seconds=60):
    context = baseline.feed.request_context()
    context.deadline.CopyFrom(baseline.feed.timestamp_in(seconds))
    context.delegation = proof
    return context


def grant_record(permissions, scopes):
    return {'assignment_id': str(uuid.uuid4()), 'permissions': permissions, 'scope': scopes}


def prepare(root, enrolled):
    stage('bootstrap enrollment')
    p14.prepare(root, enrolled)
    catalogs = {family: str(uuid.uuid4())
                for family in ('accounts', 'installations', 'instances', 'adapters', 'hosts')}
    STATE['catalogs'] = catalogs
    STATE['root'] = root
    STATE['admin_principal'] = enrolled['administrator_id']
    STATE['adapter_ids'] = [scale_adapter_id(index) for index in range(1, SCALE_COUNT + 1)]
    read_scopes = [{'kind': 'RESOURCE_ADAPTER', 'id': adapter_id}
                   for adapter_id in STATE['adapter_ids']]
    grants = [
        (['adapter.read'], [{'kind': 'RESOURCE_CATALOG', 'id': catalogs['adapters']}]),
        (['adapter.read'], read_scopes),
    ]
    for permissions, scopes in grants:
        enrolled['administrator_assignments'].append(grant_record(permissions, scopes))
        enrolled['control_assignments'].append(grant_record(permissions, scopes))


def sql(database, statement):
    environment, pg_bin = STATE['pg']
    return rt.run_sql(({**environment, 'PGDATABASE': database}, pg_bin), statement)


def start_identity(root, binaries):
    rt.identity_security_material(root)
    config = json.loads((root / 'identity.json').read_text())
    config['probe_listen'] = p14.free_endpoint()
    config['grpc_listen'] = p14.free_endpoint()
    path = root / 'scale-identity.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    child, state = p14.daemon(binaries['identity'], path)
    STATE['children'].append(child)
    require(state.get('ready') == 'true', 'identity readiness')
    require(rt.wait_ready(int(state['probe_port']), True, 20.0), 'identity probe')
    return config['grpc_listen']


def start_runtime(root, binaries, database, identity_endpoint):
    endpoints = {'grpc': p14.free_endpoint(), 'probe': p14.free_endpoint(),
                 'bridge': p14.free_endpoint()}
    tls = STATE['tls']
    config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': endpoints['probe'], 'grpc_listen': endpoints['grpc'],
        'bridge_listen': endpoints['bridge'],
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': str(tls / 'ca.pem')},
        'database': database, 'identity_endpoint': identity_endpoint,
        'resource_catalogs': STATE['catalogs'],
    }
    path = root / 'scale-runtime.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    child, state = p14.daemon(binaries['runtime'], path)
    STATE['children'].append(child)
    require(rt.wait_ready(int(state['probe_port']), True, 20.0), 'runtime probe')
    STATE['runtime_grpc'] = endpoints['grpc']
    STATE['runtime_bridge'] = endpoints['bridge']
    return endpoints['grpc']


def register_scale_adapters(mapping):
    stage('real 127 adapter registrations')
    ca = STATE['tls'] / 'ca.pem'
    (_, adapters, _, _, _, _, _) = p06.proto_modules(REPO)
    grpc_module = p06._grpc()

    async def register_all():
        channel = grpc_module.aio.secure_channel(
            f'localhost:{STATE["runtime_bridge"].rsplit(":", 1)[1]}',
            grpc_module.ssl_channel_credentials(root_certificates=ca.read_bytes()))
        await channel.channel_ready()
        try:
            for index, entry in enumerate(mapping):
                stream = channel.stream_stream(
                    '/kiarabot.v1.AdapterBridge/Connect',
                    request_serializer=adapters.AdapterToCore.SerializeToString,
                    response_deserializer=adapters.CoreToAdapter.FromString)
                call = stream(metadata=(('authorization', 'Bearer ' + entry['bootstrap']),))
                await call.write(adapters.AdapterToCore(
                    register=adapters.RegisterAdapterRequest(
                        adapter_id=STATE['adapter_ids'][index],
                        implementation='scale-directory-e2e',
                        implementation_version='1.0.0', protocol_version='kiarabot.v1',
                        adapter_instance_id=entry['adapter_instance'],
                        accounts=[adapters.Account(account_id=entry['account'], enabled=True)],
                        capabilities=[common_pb2.Capability(
                            name='action.send_message.text.v1', version='1', supported=True)],
                        context=baseline.feed.request_context())))
                response = await call.read()
                require(response is not None and
                        response.WhichOneof('body') == 'register_result' and
                        response.register_result.accepted,
                        f'registration {index + 1}/{len(mapping)} accepted')
                call.cancel()
        finally:
            await channel.close()

    asyncio.run(register_all())


def owner_delegation(identity, permission, scope):
    return baseline.feed.issue_delegation(
        identity, STATE['tokens']['access_token'], 'runtime', scope, [permission],
        baseline.feed.leaf_fingerprint(STATE['tls'] / 'control.pem'), 'scale-read')


def login_admin():
    status, tokens = baseline.https_request(
        STATE['control_port'], 'POST', '/api/v1/auth/login',
        {'username': 'administrator',
         'password': (STATE['root'] / 'admin-password').read_text().strip()},
        idempotency=True)
    require(status == 200 and tokens and tokens.get('access_token'), 'administrator login')
    STATE['tokens'] = tokens


def measure_owner_scan(identity_endpoint, count):
    stage('owner large-directory scan measurement')
    identity_channel = baseline.feed.service_channel(
        identity_endpoint, STATE['tls'] / 'control.pem', STATE['tls'] / 'control.key',
        STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(identity_channel).result(timeout=15)
    runtime_channel = baseline.feed.service_channel(
        STATE['runtime_grpc'], STATE['tls'] / 'control.pem', STATE['tls'] / 'control.key',
        STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(runtime_channel).result(timeout=15)
    try:
        identity = identity_pb2_grpc.IdentityServiceStub(identity_channel)
        scope = platform_pb2.ResourceScope(resources=[
            platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                     id=STATE['catalogs']['adapters'])])
        for adapter_id in STATE['adapter_ids'][:count]:
            scope.resources.add(kind=platform_pb2.RESOURCE_ADAPTER, id=adapter_id)
        proof = owner_delegation(identity, policy_pb2.PERMISSION_ADAPTER_READ, scope)
        runtime = runtime_pb2_grpc.RuntimeServiceStub(runtime_channel)
        started = time.monotonic()
        token = ''
        pages = 0
        seen = 0
        while True:
            request = runtime_pb2.RuntimeListAdaptersRequest(
                context=rpc_context(proof),
                catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                                 id=STATE['catalogs']['adapters']),
                page=common_pb2.PageRequest(page_size=100))
            if token:
                request.page.page_token = token
            page = runtime.ListAdapters(request, timeout=60)
            pages += 1
            seen += len(page.adapters)
            token = page.snapshot.page.next_page_token
            if not token or pages > 8:
                break
        elapsed = int((time.monotonic() - started) * 1000)
        require(seen == count, f'owner scan returns every granted child ({seen}/{count})')
        print(f'INFO owner_scan count={count} pages={pages} total_ms={elapsed} '
              f'per_child_ms={elapsed / count:.1f}', flush=True)
        return elapsed, pages
    finally:
        identity_channel.close()
        runtime_channel.close()


def run_checks(root, identity_endpoint):
    stage('real owner adapter rows')
    rows = int(sql('p03_runtime', 'SELECT count(*) FROM kiarabot.adapters;'))
    require(rows == SCALE_COUNT, f'real scale adapter rows ({rows}/{SCALE_COUNT})')
    login_admin()
    target = f'catalogs={STATE["catalogs"]["adapters"]}'
    stage('WS >126 authorized children is RATE_LIMITED')
    client = baseline.WebSocketClient(STATE['control_port'], '/ws/adapters')
    try:
        client.send_text(baseline.hello(STATE['tokens']['access_token'],
                                        [('RESOURCE_CATALOG', STATE['catalogs']['adapters'])]))
        close_code = None
        frames = []
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            try:
                frames.append(json.loads(client.recv(max(0.05, deadline - time.monotonic()))))
            except baseline.WsClosed as error:
                close_code = error.code
                break
            except Exception:  # noqa: BLE001 - transport failure is also fail-closed
                break
        require(close_code == 1013,
                f'>126 expanded children must close RATE_LIMITED 1013, got {close_code} '
                f'frames={frames[:2]} {target}')
        require(all(frame.get('type') == 'SESSION_READY' for frame in frames),
                'no fabricated adapter updates before the rate-limited close')
    finally:
        client.close()

    stats = {}
    for count in (1, 5, 10, 30, 60, SCALE_COUNT - 1):
        elapsed, pages = measure_owner_scan(identity_endpoint, count)
        stats[count] = {'total_ms': elapsed, 'pages': pages}
    STATE['scan_stats'] = stats
    print(f'INFO scan_stats={json.dumps(stats, sort_keys=True)}', flush=True)


def after_bootstrap(root):
    STATE['root'] = root
    STATE['tls'] = root / 'bootstrap-tls'
    STATE['children'] = []
    binaries = STATE['binaries']
    port = int(sys.argv[2])
    stage('databases')
    runtime_database = p14.create_database(root, binaries['pg_bin'], port,
                                           binaries['initializer'], 'runtime')
    stage('scale seed')
    mapping_path = root / 'scale-adapters.json'
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('PG', 'KIARABOT_'))}
    seeded = subprocess.run(
        [str(binaries['seed']), str(root), str(port), baseline.ADAPTER_INSTANCE,
         baseline.ACCOUNT, baseline.PLUGIN_INSTANCE, baseline.ADAPTER_BOOTSTRAP,
         p06.PLUGIN_BOOTSTRAP, baseline.INSTALLATION, baseline.SHUTDOWN_INSTANCE,
         p06.SHUTDOWN_BOOTSTRAP, str(SCALE_COUNT), str(mapping_path)],
        capture_output=True, text=True, env=environment, timeout=180)
    require(seeded.returncode == 0 and 'SEEDED' in seeded.stdout,
            f'scale seed rc={seeded.returncode} err={seeded.stderr.strip()[:200]}')
    mapping = json.loads(mapping_path.read_text())
    require(len(mapping) == SCALE_COUNT, 'scale mapping count')
    STATE['pg'] = rt.postgres(root, binaries['pg_bin'], port)
    STATE['binaries'] = binaries
    baseline.STATE.update({'root': root, 'tls': STATE['tls'], 'binaries': binaries,
                           'children': STATE['children'], 'pg': STATE['pg']})
    try:
        stage('identity startup')
        identity_endpoint = start_identity(root, binaries)
        stage('runtime startup')
        runtime_endpoint = start_runtime(root, binaries, runtime_database, identity_endpoint)
        stage('control startup')
        STATE['root'] = root
        STATE['control_port'] = baseline.start_control(root, identity_endpoint, runtime_endpoint)
        register_scale_adapters(mapping)
        run_checks(root, identity_endpoint)
        print('P12 WS directory real-scale boundaries e2e passed', flush=True)
    finally:
        while STATE['children']:
            child = STATE['children'].pop()
            try:
                if child.poll() is None:
                    p14.stop(child)
            except Exception:  # noqa: BLE001 - bounded cleanup
                pass


def main():
    root = Path(sys.argv[1]).resolve()
    if len(sys.argv) < 12:
        raise RuntimeError('fixture backend directory mismatch')
    require(root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid(),
            'owned fixture root')
    require((root / 'p03-owned-marker').is_file() and not root.is_symlink(),
            'owned fixture marker')
    p14.verify_backend(root, int(sys.argv[2]), sys.argv[6])
    STATE['binaries'] = {
        'initializer': Path(sys.argv[3]).resolve(),
        'identity_bootstrap': Path(sys.argv[5]).resolve(),
        'pg_bin': Path(sys.argv[6]).resolve(),
        'identity': Path(sys.argv[7]).resolve(),
        'runtime': Path(sys.argv[8]).resolve(),
        'control': Path(sys.argv[9]).resolve(),
        'seed': Path(sys.argv[10]).resolve(),
        'openssl': Path(sys.argv[11]).resolve(),
    }
    bootstrap_fixture.main(control_server_certificate=True, prepare=prepare,
                           after_bootstrap=after_bootstrap)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:  # noqa: BLE001 - bounded redacted diagnostic below
        message = str(error)[:700]
        if 'fixture backend directory mismatch' not in message:
            message = f'P12 WS directory scale e2e ({STATE.get("stage", "bootstrap")}): ' + message
        print(message, file=sys.stderr)
        sys.exit(1)
