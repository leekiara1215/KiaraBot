#!/usr/bin/env python3
"""HOST-04 deployment chain: two real Host processes behind one Control multi-host registry.

Owned-fixture driver supplies PG16.4/initializer/binaries. Offline bootstrap enrolls the Identity
administrator, Control peer, Runtime peer and two Host peers (both with exact host.execute), then
real processes run: one real Identity, one real Runtime (two HostBridge connections/fences), two
real plugin-host processes on fixed owner mTLS endpoints, and one real Control with the fixed
offline registry `host_endpoints = {host-1: ep1, host-2: ep2}`.

Asserts per-host routing/isolation: Begin/GetMaintenance/Resume/GetOperation on both hosts,
Control projection == Host SQLite == Runtime PG gate row, cross-host operation 404, unregistered
host 503, one Host outage does not affect the healthy Host, one gate row per host (R25 no FK),
and Host restart keeps the durable gate/maintenance version while a later Begin still advances.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import ssl
import subprocess
import sys
import time
import traceback
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
sys.path.insert(0, str(REPO / 'tests/architecture'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402
import p14_native_single_host as p14  # noqa: E402
import p12_host_real_chain_e2e as chain  # noqa: E402

CROSS_HOST = '99999999-9999-4999-8999-999999999999'
UNREGISTERED_HOST = '88888888-8888-4888-8888-888888888888'


class Failure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise Failure(label)


def stage(name):
    print('P12H2 stage: ' + name, file=sys.stderr, flush=True)


def verify_owned_root():
    root = Path(sys.argv[1]).resolve()
    marker = root / 'p03-owned-marker'
    token = marker.read_text().strip() if marker.is_file() else ''
    valid = (root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid() and
             not root.is_symlink() and marker.is_file() and not marker.is_symlink() and
             len(token) == 64 and all(character in '0123456789abcdef' for character in token) and
             (root / 'data').is_dir() and not (root / 'data').is_symlink() and
             (root / 'data').resolve() == (root / 'data') and
             (root / 'password').is_file())
    if not valid:
        print('fixture backend directory mismatch', file=sys.stderr, flush=True)
        raise SystemExit(1)


def prepare(root: Path, enrolled: dict):
    """Admin gets exact host.read/host.execute on both enrolled hosts (Control delegation)."""
    for name in ('host-1', 'host-2'):
        host = bootstrap_fixture.HOST_STATE[name]
        enrolled['administrator_assignments'].append({
            'assignment_id': str(uuid.uuid4()), 'permissions': ['host.read', 'host.execute'],
            'scope': [{'kind': 'RESOURCE_HOST', 'id': host['host_id']}]})
        enrolled['administrator_assignments'].append({
            'assignment_id': str(uuid.uuid4()), 'permissions': ['host.read'],
            'scope': [{'kind': 'RESOURCE_HOST', 'id': CROSS_HOST}]})
        enrolled['administrator_assignments'].append({
            'assignment_id': str(uuid.uuid4()), 'permissions': ['host.read'],
            'scope': [{'kind': 'RESOURCE_HOST', 'id': UNREGISTERED_HOST}]})
    # Host owners call Identity.CheckAuthorization as themselves for owner RPCs.
    for peer in enrolled['platform_peers']:
        if peer['service'] != 'host':
            continue
        peer['assignments'].append({
            'assignment_id': str(uuid.uuid4()),
            'permissions': ['identity.authorization.check'],
            'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': peer['principal_id']}]})


def stop_any(child):
    if child is None:
        return
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
    try:
        child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait(timeout=5)


def start_host(binary, config, root, name):
    path = root / f'{name}.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    child = subprocess.Popen([str(binary), '--config', str(path)],
                             stdout=subprocess.PIPE, stderr=open(root / f'{name}.err', 'w'))
    first = p14.line(child, 30)
    fields = dict(item.split('=', 1) for item in first.split() if '=' in item)
    require(fields.get('grpc_port'), name + ' startup: ' + str(fields))
    return child, int(fields['grpc_port'])


def host_config(root, tls, host, host_id, endpoint, runtime_grpc, identity_endpoint, sqlite):
    return {
        'schema_version': 1, 'service': 'plugin-host',
        'probe_listen': '127.0.0.1:0', 'grpc_listen': endpoint,
        'tls': {'certificate_file': host['certificate_file'],
                'private_key_file': host['key_file'], 'ca_file': str(tls / 'ca.pem')},
        'identity_endpoint': identity_endpoint,
        'host': {
            'host_id': host_id,
            'sqlite_path': str(sqlite),
            'runtime_endpoint': runtime_grpc,
            'credential_directory': str(root),
            'packages': [{'plugin_type_id': 'fixture', 'root': str(root),
                          'sha256': 'aa' * 32, 'launch_profile': 'fixture',
                          'allowed_argument_names': []}],
            'profiles': [{'name': 'fixture', 'executable': '/bin/sh',
                          'fixed_arguments': ['sh', '-c', 'true'],
                          'allowed_environment_names': [], 'credential_environment': None}],
            'environment': {},
        },
    }


def main():
    verify_owned_root()
    bootstrap_fixture.main(control_server_certificate=True, prepare=prepare,
                           after_bootstrap=after_bootstrap, host_enrollment=True,
                           host_permissions={'host-1': ['host.execute'],
                                             'host-2': ['host.execute']})


def after_bootstrap(root: Path):
    stage('after bootstrap')
    port = int(sys.argv[2])
    initializer = str(Path(sys.argv[3]).resolve())
    pg_bin = Path(sys.argv[6]).resolve()
    identity_binary = Path(sys.argv[7]).resolve()
    runtime_binary = Path(sys.argv[8]).resolve()
    control_binary = Path(sys.argv[9]).resolve()
    host_binary = Path(sys.argv[10]).resolve()
    stage('create runtime database')
    runtime_database = p14.create_database(root, pg_bin, port, initializer, 'runtime')
    rt.identity_security_material(root)
    tls = root / 'bootstrap-tls'
    ca = tls / 'ca.pem'

    identity_config = json.loads((root / 'identity.json').read_text())
    identity_config['probe_listen'] = p14.free_endpoint()
    identity_config['grpc_listen'] = p14.free_endpoint()
    (root / 'identity.json').write_text(json.dumps(identity_config))
    (root / 'identity.json').chmod(0o600)
    identity_endpoint = identity_config['grpc_listen']

    runtime_probe, runtime_grpc = p14.free_endpoint(), p14.free_endpoint()
    host_endpoints = {}
    host_ports = {}
    for name in ('host-1', 'host-2'):
        host_ports[name] = p14.free_endpoint()
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': runtime_probe, 'grpc_listen': runtime_grpc,
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': str(ca)},
        'database': runtime_database, 'identity_endpoint': identity_endpoint,
        'hosts': [{'host_id': bootstrap_fixture.HOST_STATE[name]['host_id'],
                   'certificate_sha256': chain.pem_fingerprint(
                       tls / f'{name}.pem').hex()} for name in ('host-1', 'host-2')],
    }
    (root / 'runtime.json').write_text(json.dumps(runtime_config))
    (root / 'runtime.json').chmod(0o600)

    control_config = {
        'schema_version': 1, 'service': 'control', 'probe_listen': p14.free_endpoint(),
        'tls': {'certificate_file': str(tls / 'control.pem'),
                'private_key_file': str(tls / 'control.key'), 'ca_file': str(ca)},
        'control_api': {'listen': '127.0.0.1:0', 'identity_endpoint': identity_endpoint},
    }

    children = {}
    try:
        stage('identity start')
        identity = subprocess.Popen([str(identity_binary), '--config',
                                     str(root / 'identity.json')],
                                    stdout=subprocess.PIPE,
                                    stderr=open(root / 'identity.err', 'w'))
        children['identity'] = identity
        state = {}
        for _ in range(2):
            text = p14.line(identity)
            state.update(dict(item.split('=', 1) for item in text.split() if '=' in item))
            if 'ready' in state:
                break
        require(state.get('ready') == 'true', 'identity readiness: ' + str(state))
        chain.wait_ready(identity_config['probe_listen'])

        stage('runtime start')
        runtime = subprocess.Popen([str(runtime_binary), '--config', str(root / 'runtime.json')],
                                    stdout=subprocess.PIPE,
                                    stderr=open(root / 'runtime.err', 'w'))
        children['runtime'] = runtime
        chain.wait_ready(runtime_probe)

        stage('host start x2')
        host_configs = {}
        for name in ('host-1', 'host-2'):
            host = bootstrap_fixture.HOST_STATE[name]
            config = host_config(root, tls, host, host['host_id'], host_ports[name],
                                 runtime_grpc, identity_endpoint, root / f'{name}.sqlite')
            host_configs[name] = config
            process, bound_port = start_host(host_binary, config, root, name)
            children[name] = process
            require(f'127.0.0.1:{bound_port}' == host_ports[name],
                    name + ' bound planned fixed endpoint: ' + str(bound_port))
            host_endpoints[host['host_id']] = host_ports[name]

        stage('control start')
        control_config['control_api'].pop('host_endpoint', None)
        control_config['control_api']['host_endpoints'] = host_endpoints
        control_path = root / 'control.json'
        control_path.write_text(json.dumps(control_config))
        control_path.chmod(0o600)
        control_process = subprocess.Popen(
            [str(control_binary), '--config', str(control_path)],
            stdout=subprocess.PIPE, stderr=open(root / 'control.err', 'w'))
        children['control'] = control_process
        first = p14.line(control_process)
        require(first.startswith('control_https_port='), 'control startup: ' + first)
        control = chain.ControlRest(int(first.split('=')[1]), ca)
        control.login(root)

        def maintenance(host_id):
            status, body = control.request(
                'GET', '/api/v1/hosts/' + host_id + '/maintenance')
            require(status == 200, 'maintenance GET ' + host_id + ': ' +
                    str((status, body))[:300])
            return body['maintenance']

        def operation(host_id, operation_id):
            scope = '?scope=' + __import__('urllib.parse').parse.quote(
                json.dumps({'resources': [{'kind': 'RESOURCE_HOST', 'id': host_id}]}))
            status, body = control.request(
                'GET', '/api/v1/operations/host/' + operation_id + scope)
            return status, body

        def begin(host_id, grace_ms=1000):
            status, body = control.request(
                'POST', '/api/v1/hosts/' + host_id + '/maintenance',
                {'expected_version': int(maintenance(host_id)['version']),
                 'grace_ms': grace_ms})
            require(status == 202, 'begin ' + host_id + ': ' + str((status, body))[:300])
            return body

        def resume(host_id):
            status, body = control.request(
                'POST', '/api/v1/hosts/' + host_id + '/resume',
                {'expected_version': int(maintenance(host_id)['version'])})
            require(status in (200, 202), 'resume ' + host_id + ': ' +
                    str((status, body))[:300])
            return body

        def wait_operation(host_id, operation_id, predicate, label, timeout=60.0):
            deadline = time.monotonic() + timeout
            current = None
            while time.monotonic() < deadline:
                status, body = operation(host_id, operation_id)
                if status == 200:
                    current = body['operation']
                    if predicate(current):
                        return current
                elif status not in (200, 503):
                    require(False, label + ' status=' + str(status) + ' ' + str(body)[:200])
                time.sleep(0.1)
            raise Failure('timeout: ' + label + ' last=' + str(current)[:300])

        host1 = bootstrap_fixture.HOST_STATE['host-1']
        host2 = bootstrap_fixture.HOST_STATE['host-2']
        host1_id, host2_id = host1['host_id'], host2['host_id']

        # 1) Both real Host owner endpoints are reachable through the single Control registry.
        first_state = maintenance(host1_id)
        second_state = maintenance(host2_id)
        require(first_state['host_id'] == host1_id and second_state['host_id'] == host2_id,
                'registry routes each host id to its own owner endpoint')
        require(first_state['admission'] == second_state['admission'] == 'HOST_ADMISSION_OPEN',
                'both hosts start with open admission')

        # 2) Begin on both hosts: two independent gates, each persisted in its own Runtime PG row.
        begin1, begin2 = begin(host1_id), begin(host2_id)
        operation1 = begin1['operation']['ref']['operation_id']
        operation2 = begin2['operation']['ref']['operation_id']
        require(operation1 != operation2, 'distinct owner operations per host')
        wait_operation(host1_id, operation1,
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'host-1 begin SUCCEEDED')
        wait_operation(host2_id, operation2,
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'host-2 begin SUCCEEDED')
        gate1 = chain.runtime_gate_row(root, pg_bin, port, host1_id)
        gate2 = chain.runtime_gate_row(root, pg_bin, port, host2_id)
        require(gate1 is not None and gate2 is not None,
                'Runtime PG has one gate row per host (R25 no FK): ' + str((gate1, gate2)))
        require(gate1['host_id'] == host1_id and gate2['host_id'] == host2_id and
                gate1['admission'] == gate2['admission'] == 2,
                'each gate row closes its own host admission')
        require(gate1['operation_id'] == operation1 and gate2['operation_id'] == operation2,
                'gate rows keep the own host operation identity')
        local1 = chain.host_state(root, root / 'host-1.sqlite')
        local2 = chain.host_state(root, root / 'host-2.sqlite')
        require(local1['admission'] == local2['admission'] == 0,
                'both Host SQLite admissions closed')
        require(local1['gate'] == gate1['gate_version'] and
                local2['gate'] == gate2['gate_version'],
                'Host SQLite gate versions match their Runtime PG rows')
        require(int(maintenance(host1_id)['version']) == local1['version'] and
                int(maintenance(host2_id)['version']) == local2['version'],
                'Control projections match each Host SQLite version')

        # 3) Resume both: independent monotonic gate versions, no cross-host coupling.
        resume1, resume2 = resume(host1_id), resume(host2_id)
        wait_operation(host1_id, resume1['operation']['ref']['operation_id'],
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'host-1 resume SUCCEEDED')
        wait_operation(host2_id, resume2['operation']['ref']['operation_id'],
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'host-2 resume SUCCEEDED')
        reopened1 = chain.runtime_gate_row(root, pg_bin, port, host1_id)
        reopened2 = chain.runtime_gate_row(root, pg_bin, port, host2_id)
        require(reopened1['admission'] == reopened2['admission'] == 1,
                'both gate rows reopened')
        require(reopened1['gate_version'] > gate1['gate_version'] and
                reopened2['gate_version'] > gate2['gate_version'],
                'each gate version advanced monotonically')
        require(maintenance(host1_id)['admission'] == 'HOST_ADMISSION_OPEN' and
                maintenance(host2_id)['admission'] == 'HOST_ADMISSION_OPEN',
                'both Control projections reopen admission')

        # 4) Owner GetOperation routes by the stored operation host: own host 200, cross host 404.
        own1 = operation(host1_id, operation1)
        own2 = operation(host2_id, operation2)
        require(own1[0] == 200 and own2[0] == 200, 'own-host operation queries succeed')
        cross = operation(host2_id, operation1)
        require(cross[0] == 404 and cross[1]['code'] == 'NOT_FOUND',
                'cross-host operation is invisible (404): ' + str(cross)[:300])
        cross_reverse = operation(host1_id, operation2)
        require(cross_reverse[0] == 404 and cross_reverse[1]['code'] == 'NOT_FOUND',
                'reverse cross-host operation is invisible (404)')
        unregistered = control.request(
            'GET', '/api/v1/hosts/' + UNREGISTERED_HOST + '/maintenance')
        require(unregistered[0] == 503 and unregistered[1]['code'] == 'UPSTREAM_UNAVAILABLE',
                'unregistered host fails closed (503): ' + str(unregistered)[:300])

        # 5) One Host fault must not disturb the other. Stop host-1; the healthy host-2 keeps
        # exercising its own gate; host-1 requests fail closed (endpoint down) without fabricated
        # success. host-1 SQLite keeps its durable closed/open facts.
        stage('host-1 fault isolation')
        stop_any(children.pop('host-1'))
        time.sleep(0.5)
        host1_down = control.request('GET', '/api/v1/hosts/' + host1_id + '/maintenance')
        require(host1_down[0] == 503 and host1_down[1]['code'] == 'UPSTREAM_UNAVAILABLE',
                'faulted host endpoint fails closed: ' + str(host1_down)[:300])
        isolation_begin = begin(host2_id)
        wait_operation(host2_id, isolation_begin['operation']['ref']['operation_id'],
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'healthy host-2 begin while host-1 down')
        isolation_resume = resume(host2_id)
        wait_operation(host2_id, isolation_resume['operation']['ref']['operation_id'],
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'healthy host-2 resume while host-1 down')
        require(maintenance(host2_id)['admission'] == 'HOST_ADMISSION_OPEN',
                'healthy host admission open after isolation run')
        local1_after_fault = chain.host_state(root, root / 'host-1.sqlite')
        require(local1_after_fault['admission'] == 1,
                'faulted host keeps its durable open admission without fake progress')
        gate1_after_fault = chain.runtime_gate_row(root, pg_bin, port, host1_id)
        require(gate1_after_fault == reopened1, 'faulted host gate row unchanged')

        # 6) Restart host-1 on the same fixed owner endpoint: durable gate/maintenance versions
        # persist and the next real gate round advances that host only.
        stage('host-1 restart persistence')
        restarted, bound = start_host(host_binary, host_configs['host-1'], root, 'host-1-restart')
        children['host-1'] = restarted
        require(f'127.0.0.1:{bound}' == host_ports['host-1'],
                'restarted host keeps its fixed owner endpoint')
        # The restarted owner listener/channel may need a moment to answer; retry only the
        # transport availability, never synthesize a snapshot.
        ready_deadline = time.monotonic() + 20
        while True:
            status, body = control.request(
                'GET', '/api/v1/hosts/' + host1_id + '/maintenance')
            if status == 200:
                persisted = body['maintenance']
                break
            require(status == 503 and time.monotonic() < ready_deadline,
                    'restarted host readiness: ' + str((status, body))[:300])
            time.sleep(0.2)
        require(persisted['host_id'] == host1_id, 'restarted host owner identity')
        require(int(persisted['version']) > int(first_state['version']),
                'restarted Host keeps durable maintenance version: ' + str(persisted)[:200])
        require(persisted['admission'] == 'HOST_ADMISSION_OPEN',
                'restarted Host keeps open admission')
        require(chain.runtime_gate_row(root, pg_bin, port, host1_id) == reopened1,
                'restarted Host does not rewrite the Runtime gate row')
        restart_begin = begin(host1_id)
        wait_operation(host1_id, restart_begin['operation']['ref']['operation_id'],
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'post-restart host-1 gate SUCCEEDED')
        gate1_restart = chain.runtime_gate_row(root, pg_bin, port, host1_id)
        require(gate1_restart['gate_version'] > reopened1['gate_version'],
                'post-restart gate version advances monotonically')
        require(chain.host_state(root, root / 'host-1.sqlite')['gate'] ==
                gate1_restart['gate_version'],
                'post-restart Host SQLite matches the Runtime gate row')
        status, body = control.request(
            'POST', '/api/v1/hosts/' + host1_id + '/resume',
            {'expected_version': int(maintenance(host1_id)['version'])})
        require(status in (200, 202), 'post-restart host-1 resume accepted')
        wait_operation(host1_id, body['operation']['ref']['operation_id'],
                       lambda value: value['state'] == 'OPERATION_SUCCEEDED',
                       'post-restart host-1 resume SUCCEEDED')
        require(maintenance(host2_id)['admission'] == 'HOST_ADMISSION_OPEN',
                'host-2 untouched by host-1 restart')
        gate_rows = chain.runtime_sql(
            root, pg_bin, port,
            'SELECT host_id FROM kiarabot.host_admission_gates ORDER BY host_id')
        require(sorted(gate_rows.splitlines()) == sorted([host1_id, host2_id]),
                'exactly one Runtime gate row per host: ' + str(gate_rows))
        print('P12H2 dual host registry chain passed: ' + operation1 + ' / ' + operation2,
              flush=True)
    finally:
        for name, child in reversed(list(children.items())):
            try:
                stop_any(child)
            except Exception:  # noqa: BLE001
                pass
        if sys.exc_info()[0] is not None:
            import shutil
            destination = REPO / 'build/logs/capability-expansion'
            destination.mkdir(parents=True, exist_ok=True)
            for name in ('identity', 'runtime', 'host-1', 'host-2', 'host-1-restart', 'control'):
                source = root / f'{name}.err'
                if source.exists():
                    shutil.copyfile(source, destination / f'p12-dual-{name}.err')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:  # noqa: BLE001
        print('P12H2 stage: failure ' + type(error).__name__ + ': ' + str(error)[:600],
              file=sys.stderr, flush=True)
        traceback.print_exc()
        raise SystemExit(1)
