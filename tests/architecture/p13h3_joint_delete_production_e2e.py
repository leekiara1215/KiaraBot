#!/usr/bin/env python3
"""P13h-3 production joint lifecycle e2e: real Identity/Runtime/Scheduler/Host/plugin.

Owned-fixture driver (run_database_test.py) supplies: root, PG port, initializer, proxy, then
identity-bootstrap, pg-bin, identity, runtime, scheduler, runtime-bridge-seed, plugin-host binaries.
Real offline enrollment (Control administrator, Runtime/Scheduler platform peers, exact grants),
real mTLS Identity/Runtime/Scheduler processes, real plugin-host + managed SDK plugin, real
SchedulerService CreateTask seeding, then instance BeginDelete: BLOCKED while Host receipt is
pending and Scheduler cleanup is unavailable; real Host StopExecution receipt + Runtime
ListTasks/CancelTask cleanup -> participants Done -> operation DELETED (PG assertions); Restore
does not recreate tasks; repeated BeginDelete is idempotent; cross-instance tasks are untouched.
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import signal
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import time
import traceback
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
sys.path.insert(0, str(REPO / 'tests/architecture'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402
import p14_native_single_host as p14  # noqa: E402

ACCOUNT = '11111111-1111-4111-8111-111111111111'
INSTANCE = '44444444-4444-4444-8444-444444444444'
INSTALLATION = '55555555-5555-4555-8555-555555555555'
ADAPTER_INSTANCE = '33333333-3333-4333-8333-333333333333'
SHUTDOWN_INSTANCE = 'a1a1a1a1-a1a1-4a1a-8a1a-a1a1a1a1a1a1'
UNKNOWN_INSTANCE = 'b2b2b2b2-b2b2-4b2b-8b2b-b2b2b2b2b2b2'
ADAPTER_BOOTSTRAP = 'a' * 64
PLUGIN_BOOTSTRAP = 'b' * 64
SHUTDOWN_BOOTSTRAP = 'c' * 64
HOST_ID = 'host-a'
SAN_SCHEDULER = 'spiffe://kiarabot/scheduler-test'
SAN_HOST = 'spiffe://kiarabot/host-a'
STATE: dict = {}


class Failure(RuntimeError):
    pass


def require(condition, label):
    if not condition:
        raise Failure(label)


def stage(name):
    print(f'P13h-3 stage: {name}', file=sys.stderr, flush=True)


def prepare(root: Path, enrolled: dict):
    stage('prepare enrollment grants')
    tls = root / 'bootstrap-tls'
    p14.write_leaf(tls, 'scheduler', SAN_SCHEDULER)
    scheduler_principal, scheduler_credential = str(uuid.uuid4()), str(uuid.uuid4())
    enrolled['platform_peers'].append({
        'service': 'scheduler', 'principal_id': scheduler_principal,
        'credential_id': scheduler_credential,
        'certificate_chain_file': str(tls / 'scheduler.pem'), 'san': SAN_SCHEDULER,
        'assignments': [{'assignment_id': str(uuid.uuid4()),
                         'permissions': ['identity.authorization.check'],
                         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': scheduler_principal}]}]})
    # Runtime identity client also issues ExchangeDelegation for Scheduler task seeding.
    runtime_assignment = enrolled['platform_peers'][0]['assignments'][0]
    runtime_assignment['permissions'].append('identity.delegation.issue')
    enrolled['administrator_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['resource.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': INSTANCE}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['resource.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': SHUTDOWN_INSTANCE}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['resource.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': UNKNOWN_INSTANCE}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['task.schedule', 'task.read'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': INSTANCE},
                   {'kind': 'RESOURCE_INSTALLATION', 'id': INSTALLATION}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['task.schedule', 'task.read'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': SHUTDOWN_INSTANCE},
                   {'kind': 'RESOURCE_INSTALLATION', 'id': INSTALLATION}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.principal.manage'],
         'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': enrolled['administrator_id']}]},
    ])
    enrolled['control_assignments'].extend([
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': ACCOUNT}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': INSTANCE}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
         'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': INSTALLATION}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.principal.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': INSTANCE}]},
        {'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.principal.manage'],
         'scope': [{'kind': 'RESOURCE_INSTANCE', 'id': SHUTDOWN_INSTANCE}]},
    ])
    STATE.update(tls=tls, identity_principal=None)


def free_endpoint():
    return p14.free_endpoint()


def free_port():
    return int(p14.free_endpoint().rsplit(':', 1)[1])


def write_json(path: Path, value: dict):
    path.write_text(json.dumps(value))
    path.chmod(0o600)


def grpc_modules():
    import grpc
    from kiarabot.v1 import (identity_pb2, identity_pb2_grpc, model_pb2, model_pb2_grpc,
                             platform_pb2, policy_pb2, runtime_pb2, runtime_pb2_grpc,
                             scheduler_pb2, scheduler_pb2_grpc)
    return (grpc, identity_pb2, identity_pb2_grpc, platform_pb2, policy_pb2,
            runtime_pb2, runtime_pb2_grpc, scheduler_pb2, scheduler_pb2_grpc)


def channel(endpoint, tls, certificate='control.pem', key='control.key'):
    grpc, *_ = grpc_modules()
    return grpc.secure_channel(endpoint, grpc.ssl_channel_credentials(
        root_certificates=(tls / 'ca.pem').read_bytes(),
        private_key=(tls / key).read_bytes(),
        certificate_chain=(tls / certificate).read_bytes()))


def context(seconds=30, delegation='', idempotency=''):
    grpc, _, _, platform_pb2, *_ = grpc_modules()
    from google.protobuf.timestamp_pb2 import Timestamp
    stamp = Timestamp()
    stamp.FromNanoseconds(int((time.time() + seconds) * 1_000_000_000))
    value = platform_pb2.RequestContext(request_id=str(uuid.uuid4()),
                                        trace_id=str(uuid.uuid4()), deadline=stamp,
                                        idempotency_key=idempotency or str(uuid.uuid4()))
    if delegation:
        value.delegation = delegation
    return value


def run(command, env=None, timeout=90):
    reply = subprocess.run(command, env=env, capture_output=True, timeout=timeout)
    require(reply.returncode == 0,
            f'{Path(command[0]).name} failed code={reply.returncode}: '
            + reply.stderr.decode(errors='replace')[-700:])
    return reply


def daemon(binary: Path, config: Path):
    return p14.daemon(binary, config)


def wait_probe(endpoint, timeout=20.0):
    require(rt.wait_ready(int(endpoint.rsplit(':', 1)[1]), True, timeout),
            f'readiness {endpoint}')


def wait_probe_or_dump(child, name, endpoint, timeout=20.0):
    try:
        wait_probe(endpoint, timeout)
    except Failure:
        import select as select_module
        text = ''
        while select_module.select([child.stderr], [], [], 0)[0]:
            chunk = os.read(child.stderr.fileno(), 8192)
            if not chunk:
                break
            text += chunk.decode(errors='replace')
        print(f'P13h-3 {name} startup stderr: {" ".join(text.split())[-1200:]}',
              file=sys.stderr, flush=True)
        raise


def identity_login(stub, root):
    _, identity_pb2, _, *_ = grpc_modules()
    password = (root / 'admin-password').read_text().strip()
    reply = stub.Login(identity_pb2.IdentityLoginRequest(context=context(20),
                                                         username='administrator',
                                                         password=password), timeout=10)
    require(bool(reply.tokens.access_token), 'admin login')
    return reply.tokens.access_token


def insert_instance_principal(instance, root, pg_bin, port):
    # Fixture shortcut: the instance principal id must equal the instance resource id so the
    # frozen Scheduler owner-from-subject rule and the platform-scope lifecycle rule agree.
    environment, pg = rt.postgres(root, pg_bin, port, 'p03_identity')
    rt.run_sql((environment, pg),
               "INSERT INTO kiarabot.principals(id,kind,binding_kind,binding_id,version,state) "
               "VALUES('" + instance + "',3,4,'" + instance + "',1,0) "
               "ON CONFLICT (id) DO NOTHING; "
               "INSERT INTO kiarabot.authorizations(subject,version,invalidation_dirty) "
               "VALUES('" + instance + "',1,false) ON CONFLICT (subject) DO NOTHING;")
    return instance


def pem_fingerprint(path: Path) -> bytes:
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(path.read_text())).digest()


def issue_parent(stub, access, instance, permissions, scope, fingerprint):
    _, identity_pb2, _, platform_pb2, *_ = grpc_modules()
    reply = stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
        context=context(30), subject_proof=access, audience='runtime',
        scope=platform_pb2.ResourceScope(resources=scope),
        permissions=permissions, client_certificate_sha256=fingerprint,
        instance_id=instance), timeout=10)
    require(bool(reply.delegation.proof), 'parent delegation')
    return reply.delegation.proof


def exchange(stub, parent, scope, permissions, fingerprint):
    _, identity_pb2, _, platform_pb2, policy_pb2 = grpc_modules()[:5]
    reply = stub.ExchangeDelegation(identity_pb2.IdentityExchangeDelegationRequest(
        context=context(20), parent_proof=parent, audience='scheduler',
        scope=platform_pb2.ResourceScope(resources=scope), permissions=permissions,
        client_certificate_sha256=fingerprint), timeout=10)
    require(bool(reply.delegation.proof), 'scheduler child delegation')
    return reply.delegation.proof


def create_task(stub, delegation, instance, task_id, when_seconds=3600):
    _, _, _, platform_pb2, policy_pb2, _, _, scheduler_pb2, _ = grpc_modules()
    from google.protobuf.timestamp_pb2 import Timestamp
    from kiarabot.v1 import common_pb2
    scope = platform_pb2.ResourceScope(resources=[
        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE, id=instance)])
    target = common_pb2.ConversationRef(type=common_pb2.ConversationRef.GROUP, id='p13h3-group')
    when = Timestamp()
    when.FromNanoseconds(int((time.time() + when_seconds) * 1_000_000_000))
    reply = stub.CreateTask(scheduler_pb2.SchedulerCreateTaskRequest(
        context=context(20, delegation, str(uuid.uuid4())), task_id=task_id, target=target,
        payload=b'{}', run_at=when), timeout=15)
    return reply.task


def stop_process(process):
    p14.stop(process)


def open_sqlite(path: Path):
    return sqlite3.connect(path)


def sqlite_rows(path: Path, query: str):
    with sqlite3.connect(path) as database:
        return database.execute(query).fetchall()


async def main_async(args) -> None:
    _ = args
    bootstrap_fixture.main(control_server_certificate=False, prepare=prepare,
                           after_bootstrap=after_bootstrap)


def after_bootstrap(root: Path) -> None:
    stage('after bootstrap start')
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
    stage('create service databases')
    databases = {service: p14.create_database(root, pg_bin, port, initializer, service)
                 for service in ('runtime', 'scheduler', 'model')}
    rt.identity_security_material(root)
    identity_config = json.loads((root / 'identity.json').read_text())
    identity_config['probe_listen'] = free_endpoint()
    identity_config['grpc_listen'] = free_endpoint()
    write_json(root / 'identity.json', identity_config)
    identity_endpoint = identity_config['grpc_listen']

    stage('host certificate')
    p14.write_leaf(tls, 'host', SAN_HOST)
    host_fingerprint = pem_fingerprint(tls / 'host.pem').hex()
    runtime_fingerprint = pem_fingerprint(tls / 'runtime.pem').hex()
    runtime_probe, runtime_grpc = free_endpoint(), free_endpoint()
    bridge = free_endpoint()
    scheduler_probe, scheduler_grpc = free_endpoint(), free_endpoint()
    scheduler_config = {
        'schema_version': 1, 'service': 'scheduler',
        'probe_listen': scheduler_probe, 'grpc_listen': scheduler_grpc,
        'tls': {'certificate_file': str(tls / 'scheduler.pem'),
                'private_key_file': str(tls / 'scheduler.key'), 'ca_file': ca},
        'database': databases['scheduler'], 'identity_endpoint': identity_endpoint,
        'runtime_endpoint': runtime_grpc,
        'runtime_peers': [{'certificate_sha256': runtime_fingerprint,
                           'instance_id': INSTANCE, 'installation_id': INSTALLATION,
                           'account_id': ACCOUNT,
                           'permissions': ['task.read', 'task.schedule']}]}
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': runtime_probe, 'grpc_listen': runtime_grpc,
        'bridge_listen': bridge,
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
        'database': databases['runtime'], 'identity_endpoint': identity_endpoint,
        'scheduler_endpoint': scheduler_grpc,
        'hosts': [{'host_id': HOST_ID, 'certificate_sha256': host_fingerprint}]}
    write_json(root / 'scheduler.json', scheduler_config)
    write_json(root / 'runtime.json', runtime_config)
    STATE.update(tls=tls, runtime_grpc=runtime_grpc, scheduler_probe=scheduler_probe)

    environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
    stage('runtime seed')
    run([str(seed_binary), str(root), str(port), ADAPTER_INSTANCE, ACCOUNT, INSTANCE,
         ADAPTER_BOOTSTRAP, PLUGIN_BOOTSTRAP, INSTALLATION, SHUTDOWN_INSTANCE,
         SHUTDOWN_BOOTSTRAP], env=environment)

    children = {}
    try:
        stage('identity start')
        identity, state = daemon(identity_binary, root / 'identity.json')
        children['identity'] = identity
        require(state.get('ready') == 'true', 'identity readiness')
        wait_probe(identity_config['probe_listen'])
        stage('runtime start')
        runtime, runtime_state = daemon(runtime_binary, root / 'runtime.json')
        children['runtime'] = runtime
        wait_probe_or_dump(runtime, 'runtime', runtime_probe)
        stage('scheduler start')
        scheduler, _ = daemon(scheduler_binary, root / 'scheduler.json')
        children['scheduler'] = scheduler
        wait_probe_or_dump(scheduler, 'scheduler', scheduler_probe)

        identity_channel = channel(identity_endpoint, tls)
        from kiarabot.v1 import common_pb2
        _, identity_pb2, identity_grpc, platform_pb2, policy_pb2, runtime_pb2, runtime_grpc_pb, \
            scheduler_pb2, scheduler_grpc_pb = grpc_modules()
        identity_stub = identity_grpc.IdentityServiceStub(identity_channel)
        runtime_channel = channel(runtime_grpc, tls)
        runtime_stub = runtime_grpc_pb.RuntimeServiceStub(runtime_channel)
        runtime_identity_channel = channel(identity_endpoint, tls, 'runtime.pem', 'runtime.key')
        runtime_identity_stub = identity_grpc.IdentityServiceStub(runtime_identity_channel)
        scheduler_channel = channel(scheduler_grpc, tls, 'runtime.pem', 'runtime.key')
        scheduler_stub = scheduler_grpc_pb.SchedulerServiceStub(scheduler_channel)
        admin = identity_login(identity_stub, root)
        plugin_principal = insert_instance_principal(INSTANCE, root, pg_bin, port)
        shutdown_principal = insert_instance_principal(SHUTDOWN_INSTANCE, root, pg_bin, port)

        task_scope = [platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE, id=INSTANCE),
                      platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTALLATION,
                                               id=INSTALLATION)]
        parent = issue_parent(identity_stub, admin, INSTANCE,
                              [policy_pb2.PERMISSION_TASK_SCHEDULE, policy_pb2.PERMISSION_TASK_READ],
                              task_scope, pem_fingerprint(tls / 'control.pem'))
        child = exchange(runtime_identity_stub, parent, task_scope,
                         [policy_pb2.PERMISSION_TASK_SCHEDULE, policy_pb2.PERMISSION_TASK_READ],
                         pem_fingerprint(tls / 'runtime.pem'))
        scheduler_identity_channel = channel(identity_endpoint, tls, 'scheduler.pem',
                                               'scheduler.key')
        scheduler_identity_stub = identity_grpc.IdentityServiceStub(scheduler_identity_channel)
        check = scheduler_identity_stub.CheckAuthorization(
            identity_pb2.IdentityCheckAuthorizationRequest(
                context=context(20), subject_proof=child,
                full_method='kiarabot.v1.SchedulerService/CreateTask',
                scope=platform_pb2.ResourceScope(resources=[
                    platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                             id=INSTANCE)])), timeout=10)
        print('P13h-3 stage: CreateTask authorization '
              f'allowed={check.decision.allowed} version={check.decision.authorization_version}',
              file=sys.stderr, flush=True)
        create_task(scheduler_stub, child, INSTANCE, 'p13h3-target-task')
        shutdown_scope = [platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                   id=SHUTDOWN_INSTANCE),
                          platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTALLATION,
                                                   id=INSTALLATION)]
        shutdown_parent = issue_parent(identity_stub, admin, SHUTDOWN_INSTANCE,
                                       [policy_pb2.PERMISSION_TASK_SCHEDULE,
                                        policy_pb2.PERMISSION_TASK_READ],
                                       shutdown_scope, pem_fingerprint(tls / 'control.pem'))
        shutdown_child = exchange(runtime_identity_stub, shutdown_parent, shutdown_scope,
                                  [policy_pb2.PERMISSION_TASK_SCHEDULE,
                                   policy_pb2.PERMISSION_TASK_READ],
                                  pem_fingerprint(tls / 'runtime.pem'))
        create_task(scheduler_stub, shutdown_child, SHUTDOWN_INSTANCE, 'p13h3-cross-task')
        scheduler_environment, scheduler_pg = rt.postgres(root, pg_bin, port, 'p03_scheduler')
        task_rows = rt.run_sql(
            (scheduler_environment, scheduler_pg),
            "SELECT task_id,owner,installation,account FROM kiarabot.tasks "
            "ORDER BY task_id").splitlines()
        print('P13h-3 stage: scheduler task rows ' + ';'.join(task_rows),
              file=sys.stderr, flush=True)
        target_row = next(row for row in task_rows if row.startswith('p13h3-target-task|'))
        target_task_id, target_owner, target_installation, target_account = target_row.split('|')
        require(target_owner == INSTANCE and target_installation == INSTALLATION and
                target_account == '',
                'frozen CreateTask owner/installation present and account empty')

        stage('seed scheduler tasks')
        package, manifest, plugin_started, plugin_stopped = write_package(root, REPO)
        host_config_path, credentials, journal = host_config(
            root, REPO, f'127.0.0.1:{runtime_grpc.rsplit(":", 1)[1]}', bridge,
            package, manifest, tls / 'ca.pem', tls / 'host.pem', tls / 'host.key')
        # Seed the start command for the managed plugin (real plugin-host executes it).
        environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
        seed_host_start(pg_bin, root, port, manifest)
        stage('host start')
        host = start_process([str(host_binary), '--config', str(host_config_path)], root,
                             root / 'host.err')
        children['host'] = host
        wait_until(lambda: plugin_started.is_file(), 'managed plugin start', timeout=60)
        require(plugin_started.read_text() == 'started', 'plugin start marker')

        instance_state = lambda: runtime_sql(pg_bin, root, port,
                                            f"SELECT state,version FROM kiarabot.instances "
                                            f"WHERE id='{INSTANCE}'").strip()
        runtime_sql(pg_bin, root, port,
                    "UPDATE kiarabot.instances SET host='host-a' WHERE id='" + INSTANCE + "';")
        require(instance_state().startswith('0|'), f'instance active before delete: {instance_state()}')
        version = int(instance_state().split('|')[1])
        delegation = issue_admin_runtime(identity_stub, admin, INSTANCE,
                                         pem_fingerprint(tls / 'control.pem'))
        # Offline both participants: Host cannot deliver StopExecution and Scheduler cannot clean.
        stop_process(host)
        children.pop('host', None)
        # The managed SDK child may survive a Host SIGTERM; stop it so it cannot keep heartbeating
        # and bumping the instance version while BeginDelete's CAS runs.
        try:
            journal_path = root / 'host.sqlite'
            rows = sqlite_rows(journal_path, 'SELECT pid FROM managed_executions ORDER BY rowid DESC')
            for (child_pid,) in rows[:1]:
                if child_pid:
                    try:
                        os.kill(int(child_pid), signal.SIGKILL)
                    except OSError:
                        pass
        except Exception as error:  # noqa: BLE001
            print('P13h-3 stage: child cleanup ' + str(error), file=sys.stderr, flush=True)
        time.sleep(1.0)
        # Scheduler is online but denied CancelTask: keep the task readable while removing
        # task.schedule from the Runtime platform peer. The participant stays BLOCKED with a
        # stable authorization failure instead of a transport failure.
        stop_process(scheduler)
        children.pop('scheduler', None)
        set_scheduler_permissions(root, ['task.read'])
        scheduler, _ = daemon(scheduler_binary, root / 'scheduler.json')
        children['scheduler'] = scheduler
        wait_probe(scheduler_probe)
        require(instance_state().startswith('0|'), f'instance active before delete: {instance_state()}')
        version = int(instance_state().split('|')[1])
        stage('begin delete offline')
        print('P13h-3 stage: instance row before delete ' + runtime_sql(
            pg_bin, root, port,
            f"SELECT state,version,host,desired FROM kiarabot.instances WHERE id='{INSTANCE}'"),
            file=sys.stderr, flush=True)
        print('P13h-3 stage: tombstones ' + runtime_sql(
            pg_bin, root, port,
            f"SELECT count(*) FROM kiarabot.lifecycle_tombstones "
            f"WHERE resource_id='{INSTANCE}'"), file=sys.stderr, flush=True)
        first = None
        last_error = None
        for attempt in range(5):
            try:
                first = begin_delete(runtime_stub, delegation, INSTANCE, version,
                                     'p13h3-delete-key-0001')
                break
            except Exception as error:  # noqa: BLE001
                last_error = error
                time.sleep(0.3)
        if first is None:
            print('P13h-3 stage: begin delete retries exhausted: ' + str(last_error),
                  file=sys.stderr, flush=True)
            raise Failure('BeginDelete conflict')
        require(bool(first.operation.operation_id), 'first BeginDelete operation')
        require('p13h3-target-task' in scheduler_task_states(pg_bin, root, port),
                'scheduler task seeded')
        blocked = wait_operation(runtime_sql, pg_bin, root, port, first.operation.operation_id,
                                 lambda state, parts: _blocked_participants(state, parts),
                                 'blocked participants', timeout=45)
        # Idempotent repeat with the same key returns the same operation.
        second = begin_delete(runtime_stub, delegation, INSTANCE, version, 'p13h3-delete-key-0001')
        require(second.operation.operation_id == first.operation.operation_id,
                'idempotent BeginDelete operation')
        print(f'P13h-3 blocked state: {blocked}', flush=True)
        # Real Host returns; real Scheduler comes back and Runtime cleans target tasks.
        stage('host return')
        host = start_process([str(host_binary), '--config', str(host_config_path)], root,
                             root / 'host-return.err')
        children['host'] = host
        set_scheduler_permissions(root, ['task.read', 'task.schedule'])
        stop_process(scheduler)
        children.pop('scheduler', None)
        scheduler, _ = daemon(scheduler_binary, root / 'scheduler.json')
        children['scheduler'] = scheduler
        wait_probe(scheduler_probe)
        try:
            listed = scheduler_stub.ListTasks(scheduler_pb2.SchedulerListTasksRequest(
                context=context(20), scope=platform_pb2.ResourceScope(resources=[
                    platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                             id=INSTANCE)]),
                page=platform_pb2.SnapshotPage(page_size=10)), timeout=10)
            print('P13h-3 stage: platform ListTasks ok tasks=' + str(len(listed.tasks)),
                  file=sys.stderr, flush=True)
            pending_list = scheduler_stub.ListTasks(scheduler_pb2.SchedulerListTasksRequest(
                context=context(20), scope=platform_pb2.ResourceScope(resources=[
                    platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                             id=INSTANCE)]),
                page=platform_pb2.SnapshotPage(page_size=10),
                state=common_pb2.SCHEDULED_TASK_PENDING), timeout=10)
            print('P13h-3 stage: platform ListTasks pending=' + str(len(pending_list.tasks)),
                  file=sys.stderr, flush=True)
        except Exception as error:  # noqa: BLE001
            print('P13h-3 stage: platform ListTasks failed ' + ' '.join(str(error).split())[:300],
                  file=sys.stderr, flush=True)
        print('P13h-3 stage: host commands ' + runtime_sql(
            pg_bin, root, port,
            f"SELECT command_id,kind,state,receipt_persisted,receipt_outcome "
            f"FROM kiarabot.host_commands WHERE instance_id='{INSTANCE}' ORDER BY kind"),
            file=sys.stderr, flush=True)
        print('P13h-3 stage: scheduler tasks ' + str(scheduler_task_states(pg_bin, root, port)),
              file=sys.stderr, flush=True)
        print('P13h-3 stage: participants ' + runtime_sql(
            pg_bin, root, port,
            f"SELECT participant,state,reason,next_attempt, "
            f"(next_attempt/1000000000.0 - extract(epoch from clock_timestamp()))::bigint "
            f"AS seconds_left FROM kiarabot.operation_participants "
            f"WHERE operation_id='{first.operation.operation_id}' ORDER BY participant"),
            file=sys.stderr, flush=True)
        deleted = wait_operation(runtime_sql, pg_bin, root, port,
                                 first.operation.operation_id,
                                 lambda state, parts: state == 3 and
                                 all(part[1] == 3 for part in parts), 'deleted operation',
                                 timeout=90)
        print('P13h-3 stage: final participants ' + runtime_sql(
            pg_bin, root, port,
            f"SELECT participant,state,reason FROM kiarabot.operation_participants "
            f"WHERE operation_id='{first.operation.operation_id}' ORDER BY participant"),
            file=sys.stderr, flush=True)
        print('P13h-3 stage: final scheduler tasks ' + str(scheduler_task_states(pg_bin, root, port)),
              file=sys.stderr, flush=True)
        require(instance_state().startswith('3|'), f'instance deleted: {instance_state()}')
        require(host_command_receipted(pg_bin, root, port, INSTANCE),
                'Host StopExecution receipt persisted')
        require(scheduler_task_states(pg_bin, root, port).get('p13h3-target-task') in (5, None),
                'target scheduler task must be Cancelled/removed')
        require(scheduler_task_states(pg_bin, root, port).get('p13h3-cross-task') == 0,
                'cross-instance scheduler task must stay Pending')
        # Restore revives the instance and never recreates tasks/leases.
        restore = restore_resource(runtime_stub, delegation, INSTANCE, version)
        require(bool(restore.operation.operation_id), 'Restore operation')
        wait_until(lambda: instance_state().startswith('0|'), 'instance restored', timeout=60)
        require(scheduler_task_states(pg_bin, root, port).get('p13h3-target-task') in (5, None),
                'restore recreated target task')

        # ---- P13h-3 remaining matrix: re-delete and Host UNKNOWN. ----
        stage('matrix stop real host')
        stop_process(host)
        children.pop('host', None)
        stop_plugin_children(root)
        time.sleep(0.8)

        def delete_target(instance, key):
            delegation = issue_admin_runtime(identity_stub, admin, instance,
                                             pem_fingerprint(tls / 'control.pem'))
            last = None
            for _ in range(5):
                try:
                    operation = begin_delete(runtime_stub, delegation, instance, 1, key)
                    require(bool(operation.operation.operation_id),
                            f'{instance} deletion operation')
                    return operation.operation.operation_id
                except Exception as error:  # noqa: BLE001
                    last = error
                    time.sleep(0.4)
            raise Failure(f'{instance} BeginDelete retries exhausted: {last}')

        fake, fake_state = start_fake_host(root, 'known')
        children['fake-known'] = fake
        second_operation = delete_target(INSTANCE, 'p13h3-redelete-key-0002')
        wait_until(fake_state.is_file, 're-delete Host stop', timeout=30)
        wait_operation(runtime_sql, pg_bin, root, port, second_operation,
                       lambda state, parts: state == 3 and all(part[1] == 3 for part in parts),
                       're-delete DELETED', timeout=90)
        require(instance_state().startswith('3|'), 're-delete instance DELETED')
        stop_process(fake)
        children.pop('fake-known', None)
        stage('matrix re-delete DELETED')

        insert_managed_instance(pg_bin, root, port, UNKNOWN_INSTANCE)
        print('P13h-3 stage: unknown instance row ' + runtime_sql(
            pg_bin, root, port,
            f"SELECT state,version,host,desired,installation,plugin_type FROM kiarabot.instances "
            f"WHERE id='{UNKNOWN_INSTANCE}'"), file=sys.stderr, flush=True)
        fake, fake_state = start_fake_host(root, 'unknown')
        children['fake-unknown'] = fake
        unknown_operation = delete_target(UNKNOWN_INSTANCE, 'p13h3-unknown-key-0001')
        wait_until(fake_state.is_file, 'unknown Host stop', timeout=30)
        with open(fake_state, 'r', encoding='utf-8') as handle:
            unknown_receipt = json.load(handle)
        require(unknown_receipt['instance_id'] == UNKNOWN_INSTANCE, 'unknown Host stop instance')
        wait_operation(runtime_sql, pg_bin, root, port, unknown_operation,
                       lambda state, parts: _blocked_participants(state, parts),
                       'unknown receipt blocked', timeout=60)
        time.sleep(2.5)
        require(runtime_sql(pg_bin, root, port,
                            "SELECT state FROM kiarabot.operations WHERE id='" +
                            unknown_operation + "'").strip() == '2',
                'UNKNOWN receipt must not complete the operation')
        require(runtime_sql(pg_bin, root, port,
                            "SELECT state FROM kiarabot.instances WHERE id='" +
                            UNKNOWN_INSTANCE + "'").strip() == '2',
                'UNKNOWN receipt instance remains DELETING')
        stop_process(fake)
        children.pop('fake-unknown', None)
        stage('matrix unknown BLOCKED')
        require(scheduler_task_states(pg_bin, root, port).get('p13h3-cross-task') == 0,
                'cross-instance task remains pending after matrix')
        print(f'P13h-3 production joint delete e2e passed: {deleted}', flush=True)
    finally:
        for name, child in list(children.items()):
            try:
                stop_process(child)
            except Exception:
                pass
        import select as select_module
        for name, child in list(children.items()):
            if child is not None and child.stderr is not None:
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
                    print(f'P13h-3 {name} stderr: ' + ' '.join(text.split())[-2000:],
                          file=sys.stderr)
        for name in ('host.err', 'host-return.err', 'runtime.err', 'scheduler.err', 'identity.err',
                     'plugin.log'):
            path = root / name
            if path.is_file():
                print(f'P13h-3 {name}: ' + path.read_text(errors='replace')[-2500:],
                      file=sys.stderr)


# ---- host/plugin helpers (adapted from the P08e/P13h-3 step-1 harness) ----

def write_package(directory: Path, repo: Path):
    package = directory / 'package'
    package.mkdir(parents=True, exist_ok=True)
    applied = directory / 'applied.jsonl'
    started = directory / 'plugin.started'
    stopped = directory / 'plugin.stopped'
    (package / 'subject.py').write_text(
        'import json\n'
        'from pathlib import Path\n'
        'from kiarabot_sdk import Plugin\n'
        'class Subject(Plugin):\n'
        '    async def on_start(self, context):\n'
        f'        Path({str(started)!r}).write_text("started")\n'
        '    async def on_stop(self, reason):\n'
        f'        Path({str(stopped)!r}).write_text(str(reason))\n'
        'def create():\n'
        '    return Subject()\n')
    manifest = json.loads((repo / 'plugins/event-observer/plugin.json').read_text())
    manifest['entrypoint'] = 'subject:create'
    manifest_path = package / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    return package, manifest_path, started, stopped


def host_config(directory: Path, repo: Path, runtime_endpoint: str, bridge_endpoint: str,
                package: Path, manifest: Path, ca: Path, host_cert: Path, host_key: Path):
    credentials = directory / 'credentials'
    credentials.mkdir(mode=0o700, exist_ok=True)
    response = json.dumps({'instance': {'instance_id': INSTANCE},
                           'bootstrap_credential': PLUGIN_BOOTSTRAP})
    helper = repo / 'scripts/deploy/target_host_credential.py'
    result = subprocess.run([sys.executable, str(helper), 'store', '--directory',
                             str(credentials)], input=response, text=True,
                            capture_output=True, env={**os.environ}, timeout=20)
    require(result.returncode == 0, f'credential helper failed: {result.stderr[-400:]}')
    return (store_credential(directory, repo, runtime_endpoint, bridge_endpoint, package,
                             manifest, ca, host_cert, host_key, credentials),
            credentials, directory / 'host.sqlite')


def store_credential(directory, repo, runtime_endpoint, bridge_endpoint, package, manifest,
                     ca, host_cert, host_key, credentials):
    environment_names = ['PYTHONPATH', 'PLUGIN_CORE_ENDPOINT', 'PLUGIN_TLS_ROOT',
                         'PLUGIN_INSTANCE_ID', 'PLUGIN_IO_TIMEOUT', 'PLUGIN_HEARTBEAT_INTERVAL',
                         'PLUGIN_RETRY_DELAY', 'KIARABOT_PLUGIN_BOOTSTRAP']
    environment = {
        'PYTHONPATH': os.pathsep.join([str(repo / 'sdk/python/src'),
                                       str(repo / 'sdk/python/build/generated'),
                                       str(package)]),
        'PLUGIN_CORE_ENDPOINT': bridge_endpoint, 'PLUGIN_TLS_ROOT': str(ca),
        'PLUGIN_INSTANCE_ID': INSTANCE, 'PLUGIN_IO_TIMEOUT': '2',
        'PLUGIN_HEARTBEAT_INTERVAL': '0.2', 'PLUGIN_RETRY_DELAY': '0.1'}
    runner = directory / 'run_plugin.sh'
    runner.write_text('#!/bin/sh\nexec "' + sys.executable + '" -m kiarabot_sdk "$@" '
                      '>>"' + str(directory / 'plugin.log') + '" 2>&1\n')
    runner.chmod(0o700)
    config = {
        'schema_version': 1, 'service': 'plugin-host',
        'probe_listen': f'127.0.0.1:{free_port()}', 'grpc_listen': f'127.0.0.1:{free_port()}',
        'tls': {'certificate_file': str(host_cert), 'private_key_file': str(host_key),
                'ca_file': str(ca)},
        'host': {'host_id': HOST_ID, 'sqlite_path': str(directory / 'host.sqlite'),
                 'runtime_endpoint': runtime_endpoint,
                 'credential_directory': str(credentials),
                 'packages': [{'plugin_type_id': 'event-observer', 'root': str(package),
                               'sha256': 'a' * 64, 'launch_profile': 'python',
                               'allowed_argument_names': ['--manifest']}],
                 'profiles': [{'name': 'python', 'executable': str(runner),
                               'fixed_arguments': [str(runner)],
                               'allowed_environment_names': environment_names,
                               'credential_environment': 'KIARABOT_PLUGIN_BOOTSTRAP'}],
                 'environment': environment}}
    path = directory / 'host.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    return path


def start_process(command, cwd: Path, stderr_path: Path):
    error_file = stderr_path.open('wb')
    process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=error_file,
                               env={key: value for key, value in os.environ.items()})
    return process


def insert_managed_instance(pg_bin: Path, root: Path, port: int, instance: str):
    runtime_sql(pg_bin, root, port,
                "INSERT INTO kiarabot.instances(id,installation,plugin_type,desired_version,"
                "applied_config_version,fence_instance,runtime_epoch,execution_id,"
                "execution_generation,connection_id,connection_generation,version,state,host,"
                "desired) SELECT '" + instance + "',installation,plugin_type,1,0,"
                "NULL,NULL,NULL,NULL,NULL,NULL,1,0,'" + HOST_ID + "',0 FROM kiarabot.instances "
                "WHERE id='" + INSTANCE + "' ON CONFLICT (id) DO UPDATE SET state=0,host='" +
                HOST_ID + "'")


def start_fake_host(root: Path, mode: str):
    ready = root / f'fake-host-{mode}.ready'
    state = root / f'fake-host-{mode}.state'
    for path in (ready, state):
        if path.exists():
            path.unlink()
    tls = STATE['tls']
    process = start_process([
        sys.executable, str(REPO / 'tests/architecture/p13h3_fake_host.py'),
        '--endpoint', STATE['runtime_grpc'], '--ca', str(tls / 'ca.pem'),
        '--cert', str(tls / 'host.pem'), '--key', str(tls / 'host.key'),
        '--host-id', HOST_ID, '--mode', mode, '--ready', str(ready), '--state', str(state)],
        root, root / f'fake-host-{mode}.err')
    wait_until(ready.is_file, f'fake host {mode} ready', timeout=30)
    return process, state


def stop_plugin_children(root: Path):
    try:
        journal_path = root / 'host.sqlite'
        rows = sqlite_rows(journal_path, 'SELECT pid FROM managed_executions ORDER BY rowid DESC')
        for (child_pid,) in rows[:1]:
            if child_pid:
                try:
                    os.kill(int(child_pid), signal.SIGKILL)
                except OSError:
                    pass
    except Exception:
        pass


def set_scheduler_permissions(root: Path, permissions: list[str]):
    config = json.loads((root / 'scheduler.json').read_text())
    config['runtime_peers'][0]['permissions'] = permissions
    write_json(root / 'scheduler.json', config)


def seed_host_start(pg_bin: Path, root: Path, port: int, manifest: Path):
    environment, pg = rt.postgres(root, pg_bin, port, 'p03_runtime')
    runtime_password = (root / 'password-p03_runtime_runtime').read_text().strip()
    now_sql = '(extract(epoch from clock_timestamp())*1000000000)::bigint'
    args_sql = "'--manifest=" + str(manifest).replace("'", "''") + "'"
    env_sql = ("ARRAY['PYTHONPATH','PLUGIN_CORE_ENDPOINT','PLUGIN_TLS_ROOT',"
               "'PLUGIN_INSTANCE_ID','PLUGIN_IO_TIMEOUT','PLUGIN_HEARTBEAT_INTERVAL',"
               "'PLUGIN_RETRY_DELAY','KIARABOT_PLUGIN_BOOTSTRAP']")
    statement = ("INSERT INTO kiarabot.host_commands(command_id,host_id,instance_id,installation_id,"
                 "kind,execution_generation,desired_version,desired_state,package_sha256,"
                 "launch_profile,arguments,environment_names,payload_sha256,state,created_at,updated_at) "
                 "VALUES('hc-p13h3-start','host-a','" + INSTANCE + "','" + INSTALLATION + "',1,1,1,0,"
                 "repeat('a',64),'python',ARRAY[" + args_sql + "]," + env_sql +
                 ",repeat('b',64),0," + now_sql + "," + now_sql + ");")
    reply = subprocess.run([str(pg / 'psql'), '-X', '-qAt', '-v', 'ON_ERROR_STOP=1'],
                           input=statement, text=True, env=environment,
                           capture_output=True, timeout=15)
    if reply.returncode != 0:
        print('P13h-3 host seed SQL: ' + ' '.join(reply.stderr.split())[:800],
              file=sys.stderr, flush=True)
        raise Failure('host seed SQL')


def wait_until(predicate, label, timeout=45.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise Failure(f'timeout: {label}')


def runtime_sql(pg_bin: Path, root: Path, port: int, statement: str) -> str:
    environment, pg = rt.postgres(root, pg_bin, port, 'p03_runtime')
    try:
        return rt.run_sql((environment, pg), statement)
    except Exception:
        print('P13h-3 stage: runtime SQL failed: ' + ' '.join(statement.split())[:500],
              file=sys.stderr, flush=True)
        reply = subprocess.run([str(pg / 'psql'), '-X', '-qAt', '-v', 'ON_ERROR_STOP=1'],
                               input=statement, text=True, env=environment,
                               capture_output=True, timeout=15)
        print('P13h-3 stage: runtime SQL error rc=' + str(reply.returncode) + ': ' + str(reply.stderr)[-1500:],
              file=sys.stderr, flush=True)
        raise


def scheduler_task_states(pg_bin: Path, root: Path, port: int) -> dict:
    environment, pg = rt.postgres(root, pg_bin, port, 'p03_scheduler')
    rows = rt.run_sql((environment, pg),
                      "SELECT task_id,state FROM kiarabot.tasks ORDER BY task_id").splitlines()
    result = {}
    for row in rows:
        task_id, state = row.split('|')
        result[task_id] = int(state)
    return result


def host_command_receipted(pg_bin: Path, root: Path, port: int, instance: str) -> bool:
    rows = runtime_sql(pg_bin, root, port,
                       "SELECT kind,state,receipt_persisted FROM kiarabot.host_commands "
                       "WHERE instance_id='" + instance + "' ORDER BY kind")
    values = [line.split('|') for line in rows.splitlines() if line]
    return any(kind == '2' and state == '2' and persisted == 't'
               for kind, state, persisted in values)


def _blocked_participants(state, parts):
    by_name = dict(parts)
    return (state == 2 and by_name.get('runtime') == 3 and
            by_name.get('scheduler') in (0, 1, 2) and by_name.get('host') in (0, 1, 2))


def wait_operation(query, pg_bin, root, port, operation_id,
                   predicate, label, timeout=60):
    last = None

    last_rows = []

    def check():
        nonlocal last
        state = query(pg_bin, root, port,
                      "SELECT state FROM kiarabot.operations WHERE id='" + operation_id + "'").strip()
        rows = query(pg_bin, root, port,
                     "SELECT participant,state,reason,next_attempt, "
                     "(next_attempt/1000000000.0 - extract(epoch from clock_timestamp()))::bigint "
                     "FROM kiarabot.operation_participants WHERE operation_id='" +
                     operation_id + "' ORDER BY participant").splitlines()
        parts = []
        for row in rows:
            if not row:
                continue
            values = row.split('|')
            participant, value = values[0], values[1]
            parts.append((participant, int(value)))
        if parts:
            last_rows = rows
        last = (state, parts)
        try:
            return predicate(int(state), parts)
        except (TypeError, ValueError):
            return False

    try:
        wait_until(check, label, timeout=timeout)
    except Failure:
        raise Failure(f'{label}: last={last}') from None
    return last


def issue_admin_runtime(identity_stub, access, instance, fingerprint):
    _, identity_pb2, _, platform_pb2, policy_pb2, *_ = grpc_modules()
    reply = identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
        context=context(30), subject_proof=access, audience='runtime',
        scope=platform_pb2.ResourceScope(resources=[
            platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE, id=instance)]),
        permissions=[policy_pb2.PERMISSION_RESOURCE_MANAGE],
        client_certificate_sha256=fingerprint), timeout=10)
    require(bool(reply.delegation.proof), 'admin runtime resource.manage delegation')
    return reply.delegation.proof


def begin_delete(runtime_stub, delegation, instance, version, key):
    _, _, _, platform_pb2, _, runtime_pb2, *_ = grpc_modules()
    from google.protobuf.timestamp_pb2 import Timestamp
    request = runtime_pb2.RuntimeBeginDeleteRequest(
        context=context(30, delegation, key),
        resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE, id=instance))
    return runtime_stub.BeginDelete(request, timeout=20)


def restore_resource(runtime_stub, delegation, instance, version):
    _, _, _, platform_pb2, _, runtime_pb2, *_ = grpc_modules()
    request = runtime_pb2.RuntimeRestoreResourceRequest(
        context=context(30, delegation),
        resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE, id=instance))
    return runtime_stub.RestoreResource(request, timeout=20)


def main() -> int:
    arguments = sys.argv[1:]
    if len(arguments) < 6:
        print('fixture backend directory mismatch', file=sys.stderr)
        return 1
    try:
        print('P13h-3 stage: argv=' + ' '.join(sys.argv[1:6]), file=sys.stderr, flush=True)
        asyncio.run(main_async(argparse.Namespace()))
        return 0
    except SystemExit:
        raise
    except Exception as error:  # noqa: BLE001
        frames = traceback.extract_tb(sys.exc_info()[2])
        location = ' <- '.join(f'{frame.filename.rsplit("/", 1)[-1]}:{frame.lineno}'
                               for frame in frames[-3:])
        message = ' '.join(str(error).split())
        print(f'P13h-3 production joint e2e failure: {type(error).__name__}: '
              f'{message[:1200]} at {location}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
