#!/usr/bin/env python3
"""Real Scheduler<->Runtime TimerTrigger handoff e2e on an owned PG16.4 cluster.

Real Identity (platform peers + delegation), real Scheduler and Runtime daemons, real mTLS and
real gRPC. Scheduler RPCs are authorized by a Control-signed Scheduler-audience delegation; the
internal Scheduler->Runtime.AcceptTimerTrigger hop uses the enrolled Scheduler platform peer
(mTLS leaf SAN) + handoff integrity fields, per the P10b platform-peer decision.
"""
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import ssl
import subprocess
import sys
import time
import uuid

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402

STATE = {}
SAN = 'spiffe://kiarabot/scheduler-test'


def require(condition, stage):
    if not condition:
        raise RuntimeError('p10 timer e2e assertion: ' + stage)


def write_scheduler_certificate(tls_root):
    extensions = tls_root / 'scheduler.ext'
    extensions.write_text(
        f'subjectAltName=URI:{SAN},DNS:localhost,IP:127.0.0.1\n'
        'extendedKeyUsage=clientAuth,serverAuth\nbasicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature\nsubjectKeyIdentifier=hash\n'
        'authorityKeyIdentifier=keyid:always\n')
    commands = [
        ['req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
         '-subj', '/CN=Scheduler', '-keyout', 'scheduler.key', '-out', 'scheduler.csr'],
        ['x509', '-req', '-in', 'scheduler.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
         '-CAcreateserial', '-days', '1', '-extfile', 'scheduler.ext', '-out', 'scheduler.pem'],
    ]
    for arguments in commands:
        reply = subprocess.run(['openssl', *arguments], cwd=tls_root, capture_output=True,
                               timeout=10)
        require(reply.returncode == 0, 'scheduler certificate generation')
    (tls_root / 'scheduler.key').chmod(0o600)


def write_extra_runtime_certificate(tls_root, name, san):
    extensions = tls_root / f'{name}.ext'
    extensions.write_text(
        f'subjectAltName=URI:{san},DNS:localhost,IP:127.0.0.1\n'
        'extendedKeyUsage=clientAuth,serverAuth\nbasicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature\nsubjectKeyIdentifier=hash\n'
        'authorityKeyIdentifier=keyid:always\n')
    commands = [
        ['req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
         '-subj', f'/CN={name}', '-keyout', f'{name}.key', '-out', f'{name}.csr'],
        ['x509', '-req', '-in', f'{name}.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
         '-CAcreateserial', '-days', '1', '-extfile', f'{name}.ext', '-out', f'{name}.pem'],
    ]
    for arguments in commands:
        reply = subprocess.run(['openssl', *arguments], cwd=tls_root, capture_output=True,
                               timeout=10)
        require(reply.returncode == 0, f'{name} certificate generation')
    (tls_root / f'{name}.key').chmod(0o600)


def certificate_sha256(path):
    der = ssl.PEM_cert_to_DER_cert(path.read_text())
    return hashlib.sha256(der).hexdigest()


def scheduler_database(root, pg_bin, port, initializer):
    migration, runtime = secrets.token_hex(32), secrets.token_hex(32)
    migration_file = root / 'password-p03_scheduler_migration'
    runtime_file = root / 'password-p03_scheduler_runtime'
    for path, value in [(migration_file, migration), (runtime_file, runtime)]:
        path.write_text(value + '\n')
        path.chmod(0o600)
    environment = rt.postgres(root, pg_bin, port)
    constraints = ('LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION '
                   'NOBYPASSRLS')
    rt.run_sql(environment,
               f"CREATE ROLE p03_scheduler_migration {constraints} PASSWORD '{migration}';\n"
               f"CREATE ROLE p03_scheduler_runtime {constraints} PASSWORD '{runtime}';\n"
               'CREATE DATABASE p03_scheduler OWNER p03_scheduler_migration;\n'
               'REVOKE ALL ON DATABASE p03_scheduler FROM PUBLIC;\n'
               'GRANT CONNECT ON DATABASE p03_scheduler TO p03_scheduler_runtime;\n')
    initialized = subprocess.run(
        [initializer, '--initialize-empty', 'scheduler', 'localhost', str(port), 'p03_scheduler',
         'p03_scheduler_migration', 'p03_scheduler_runtime', str(root / 'server.pem'),
         str(migration_file)],
        capture_output=True, text=True, env={k: v for k, v in os.environ.items()
                                             if not k.startswith('PG')}, timeout=20)
    require(initialized.returncode == 0, 'owned scheduler initialization')
    return {'host': 'localhost', 'port': int(port), 'name': 'p03_scheduler',
            'migration_role': 'p03_scheduler_migration',
            'runtime_role': 'p03_scheduler_runtime',
            'password_file': str(runtime_file), 'ca_file': str(root / 'server.pem')}


def prepare(root, enrolled):
    """Extend the bootstrap fixture: enroll the Scheduler platform peer + task delegation grants."""
    tls_root = root / 'bootstrap-tls'
    write_scheduler_certificate(tls_root)
    write_extra_runtime_certificate(tls_root, 'runtime-readonly', 'spiffe://kiarabot/runtime-readonly')
    installation = str(uuid.uuid4())
    # The frozen delegation claim has no installation/instance field; instance scopes are derived
    # from the delegated subject, so the fixture's owner instance is the delegated principal.
    instance = enrolled['administrator_id']
    task = str(uuid.uuid4())
    scheduler_principal, scheduler_credential = str(uuid.uuid4()), str(uuid.uuid4())
    enrolled['platform_peers'].append({
        'service': 'scheduler', 'principal_id': scheduler_principal,
        'credential_id': scheduler_credential,
        'certificate_chain_file': str(tls_root / 'scheduler.pem'), 'san': SAN,
        'assignments': [
            {'assignment_id': str(uuid.uuid4()), 'permissions': ['timer.accept'],
             'scope': [{'kind': 'RESOURCE_TASK', 'id': task}]},
            {'assignment_id': str(uuid.uuid4()),
             'permissions': ['identity.authorization.check'],
             'scope': [{'kind': 'RESOURCE_PRINCIPAL', 'id': scheduler_principal}]}]})
    enrolled['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()), 'permissions': ['task.schedule', 'task.read'],
        'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': installation},
                  {'kind': 'RESOURCE_INSTANCE', 'id': instance}]})
    enrolled['control_assignments'].append({
        'assignment_id': str(uuid.uuid4()), 'permissions': ['identity.assignment.manage'],
        'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': installation},
                  {'kind': 'RESOURCE_INSTANCE', 'id': instance}]})
    STATE.update(installation=installation, instance=instance, task=task,
                 scheduler_san=SAN, account=str(uuid.uuid4()))


def scenario(root, port, pg_bin, binaries):
    identity_binary, runtime_binary, scheduler_binary, initializer = binaries
    tls = root / 'bootstrap-tls'
    rt.identity_security_material(root)
    scheduler_db = scheduler_database(root, pg_bin, port, initializer)
    identity, identity_state = rt.daemon(identity_binary, root / 'identity.json')
    runtime = scheduler = None
    children = [identity]
    try:
        require(identity_state.get('ready') == 'true', 'identity readiness')
        identity_endpoint = '127.0.0.1:' + identity_state['grpc_port']
        runtime_config = {
            'schema_version': 1, 'service': 'runtime', 'probe_listen': '127.0.0.1:0',
            'grpc_listen': '127.0.0.1:0',
            'tls': {'certificate_file': str(tls / 'runtime.pem'),
                    'private_key_file': str(tls / 'runtime.key'),
                    'ca_file': str(tls / 'ca.pem')},
            'database': rt.runtime_database(root, pg_bin, port, initializer)[0],
            'identity_endpoint': identity_endpoint}
        runtime_path = root / 'runtime.json'
        runtime_path.write_text(json.dumps(runtime_config))
        runtime_path.chmod(0o600)
        runtime, runtime_state = rt.daemon(runtime_binary, runtime_path)
        children.append(runtime)
        require(rt.wait_ready(int(runtime_state['probe_port']), True), 'runtime readiness')
        runtime_endpoint = '127.0.0.1:' + runtime_state['grpc_port']
        # Restarts must reuse the same owner mTLS port so Scheduler and test channels reconnect.
        runtime_config['grpc_listen'] = runtime_endpoint
        runtime_path.write_text(json.dumps(runtime_config))

        scheduler_config = {
            'schema_version': 1, 'service': 'scheduler', 'probe_listen': '127.0.0.1:0',
            'grpc_listen': '127.0.0.1:0',
            'tls': {'certificate_file': str(tls / 'scheduler.pem'),
                    'private_key_file': str(tls / 'scheduler.key'),
                    'ca_file': str(tls / 'ca.pem')},
            'database': scheduler_db, 'runtime_endpoint': runtime_endpoint,
            'identity_endpoint': identity_endpoint,
            'runtime_peers': [
                {'certificate_sha256': certificate_sha256(tls / 'runtime.pem'),
                 'instance_id': STATE['instance'], 'installation_id': STATE['installation'],
                 'account_id': STATE['account'],
                 'permissions': ['task.read', 'task.schedule']},
                {'certificate_sha256': certificate_sha256(tls / 'runtime-readonly.pem'),
                 'instance_id': STATE['instance'], 'installation_id': STATE['installation'],
                 'account_id': STATE['account'], 'permissions': ['task.read']}]}
        scheduler_path = root / 'scheduler.json'
        scheduler_path.write_text(json.dumps(scheduler_config))
        scheduler_path.chmod(0o600)
        scheduler, scheduler_state = rt.daemon(scheduler_binary, scheduler_path)
        children.append(scheduler)
        probe_port = int(scheduler_state['probe_port'])
        require(rt.wait_ready(probe_port, True, 20.0), 'scheduler readiness')
        # Restarts must reuse the same Scheduler owner port so the existing test client channel and
        # probe target stay valid; probe_listen may stay ephemeral.
        scheduler_config['grpc_listen'] = '127.0.0.1:' + scheduler_state['grpc_port']
        scheduler_path.write_text(json.dumps(scheduler_config))

        import grpc
        from kiarabot.v1 import (common_pb2, identity_pb2, identity_pb2_grpc, platform_pb2,
                                 policy_pb2, runtime_pb2, runtime_pb2_grpc, scheduler_pb2,
                                 scheduler_pb2_grpc)
        identity_channel = rt.channel(identity_endpoint, tls)
        control_channel = rt.channel(runtime_endpoint, tls)
        scheduler_channel = rt.channel('127.0.0.1:' + scheduler_state['grpc_port'], tls)
        scheduler_peer_channel = rt.channel(runtime_endpoint, tls, certificate='scheduler.pem',
                                            key='scheduler.key')
        scheduler_endpoint = '127.0.0.1:' + scheduler_state['grpc_port']
        runtime_scheduler_channel = rt.channel(scheduler_endpoint, tls, certificate='runtime.pem',
                                               key='runtime.key')
        readonly_scheduler_channel = rt.channel(scheduler_endpoint, tls,
                                                certificate='runtime-readonly.pem',
                                                key='runtime-readonly.key')
        unregistered_scheduler_channel = rt.channel(scheduler_endpoint, tls)
        try:
            identity_stub = identity_pb2_grpc.IdentityServiceStub(identity_channel)
            scheduler_stub = scheduler_pb2_grpc.SchedulerServiceStub(scheduler_channel)
            runtime_peer_stub = runtime_pb2_grpc.RuntimeServiceStub(scheduler_peer_channel)
            password = (root / 'admin-password').read_text().strip()
            access = identity_stub.Login(identity_pb2.IdentityLoginRequest(
                context=rt.context(), username='administrator', password=password),
                timeout=10).tokens.access_token
            resources = [platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTALLATION,
                                                 id=STATE['installation']),
                         platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                 id=STATE['instance'])]
            proof = identity_stub.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
                context=rt.context(), subject_proof=access, audience='scheduler',
                scope=platform_pb2.ResourceScope(resources=resources),
                permissions=[policy_pb2.PERMISSION_TASK_SCHEDULE,
                             policy_pb2.PERMISSION_TASK_READ],
                client_certificate_sha256=rt.leaf_fingerprint(tls)), timeout=10
            ).delegation.proof
            require(bool(proof), 'scheduler delegation proof')

            def create_task():
                return scheduler_stub.CreateTask(scheduler_pb2.SchedulerCreateTaskRequest(
                    context=rt.context(delegation=proof), task_id=STATE['task'],
                    target=common_pb2.ConversationRef(id='conversation',
                                                        type=common_pb2.ConversationRef.PRIVATE),
                    payload=b'payload',
                    run_at=rt.timestamp(2)), timeout=10)

            created = create_task()
            require(created.task.state == common_pb2.SCHEDULED_TASK_PENDING,
                    'scheduler task pending after authorized create')
            occurrence = created.task.occurrence_id
            require(bool(occurrence), 'occurrence id fixed at creation')

            def get_task():
                return scheduler_stub.GetTask(scheduler_pb2.SchedulerGetTaskRequest(
                    context=rt.context(delegation=proof), task_id=STATE['task'],
                    scope=platform_pb2.ResourceScope(resources=[
                        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                 id=STATE['instance'])])), timeout=10)

            deadline = time.monotonic() + 25
            completed = False
            while time.monotonic() < deadline:
                try:
                    if get_task().task.state == common_pb2.SCHEDULED_TASK_COMPLETED:
                        completed = True
                        break
                except grpc.RpcError as error:
                    # A single Identity authorization deadline under fixture load must not fail the
                    # e2e; the task state is observed again by the next attempt.
                    if error.code() not in (grpc.StatusCode.DEADLINE_EXCEEDED,
                                            grpc.StatusCode.UNAVAILABLE):
                        raise
                time.sleep(0.25)
            require(completed, 'task must reach COMPLETED after Runtime acceptance')

            payload_hash = hashlib.sha256(b'payload').digest()
            operation_id = str(uuid.uuid4())
            handoff = platform_pb2.HandoffContext(
                source_service='Scheduler', message_id=occurrence,
                operation=platform_pb2.OperationRef(
                    owner_service='Scheduler', operation_id=operation_id,
                    resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_TASK,
                                                      id=STATE['task'])),
                resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_TASK,
                                                  id=STATE['task']),
                resource_version=1, payload_sha256=payload_hash,
                created_at=rt.timestamp(-2), valid_until=rt.timestamp(300))

            def accept(envelope, valid_until=None, occurrence_id=None):
                request = runtime_pb2.RuntimeAcceptTimerTriggerRequest(
                    context=rt.context(), handoff=handoff, occurrence_id=occurrence_id or occurrence,
                    task_id=STATE['task'], owner_instance_id=STATE['instance'],
                    schedule_version=1,
                    target=common_pb2.ConversationRef(id='conversation',
                                                        type=common_pb2.ConversationRef.PRIVATE),
                    payload=b'payload')
                if envelope is not None:
                    request.handoff.payload_sha256 = payload_hash
                return runtime_peer_stub.AcceptTimerTrigger(request, timeout=10)

            duplicate = accept(None)
            require(duplicate.receipt.event_id == occurrence and duplicate.receipt.duplicate,
                    'replayed same occurrence handoff is duplicate without second enqueue')
            changed = accept(None)
            require(changed.receipt.duplicate, 'duplicate replay remains duplicate')
            bad = runtime_pb2.RuntimeAcceptTimerTriggerRequest(
                context=rt.context(), handoff=handoff, occurrence_id=str(uuid.uuid4()),
                task_id=STATE['task'], owner_instance_id=STATE['instance'], schedule_version=1,
                target=common_pb2.ConversationRef(id='conversation',
                                                    type=common_pb2.ConversationRef.PRIVATE),
                payload=b'payload')
            bad.handoff.message_id = bad.occurrence_id
            bad.handoff.valid_until.CopyFrom(rt.timestamp(-5))
            timed_out = None
            try:
                runtime_peer_stub.AcceptTimerTrigger(bad, timeout=10)
            except grpc.RpcError as error:
                timed_out = error.code()
            require(timed_out == grpc.StatusCode.DEADLINE_EXCEEDED,
                    'expired handoff maps TIMEOUT')

            # Runtime event/target in the real Runtime database: one TimerTriggered event, one target.
            event_sql = (f"SELECT count(*) FROM kiarabot.events WHERE adapter='scheduler' AND "
                         f"event_id='{occurrence}';")
            target_sql = (f"SELECT count(*) FROM kiarabot.delivery_targets WHERE adapter='scheduler' "
                          f"AND event_id='{occurrence}';")
            environment = rt.postgres(root, pg_bin, port)
            environment = ({**environment[0], 'PGDATABASE': 'p03_runtime'}, environment[1])
            require(rt.run_sql(environment, event_sql) == '1', 'runtime persisted one timer event')
            require(rt.run_sql(environment, target_sql) == '1',
                    'runtime fanned out exactly one owner target')

            # ---- residual matrix: capacity, EXPIRED, crash recovery, lost confirm, revocation ----
            pg_environment = rt.postgres(root, pg_bin, port)
            runtime_env = ({**pg_environment[0], 'PGDATABASE': 'p03_runtime'},
                           pg_environment[1])
            scheduler_env = ({**pg_environment[0], 'PGDATABASE': 'p03_scheduler'},
                             pg_environment[1])

            def query(env, statement):
                return rt.run_sql(env, statement)

            def wait_query(env, statement, expected, timeout=20.0, label=''):
                end = time.monotonic() + timeout
                value = None
                while time.monotonic() < end:
                    value = query(env, statement)
                    if value == expected:
                        return
                    time.sleep(0.2)
                raise RuntimeError(f'p10 timer matrix timeout: {label} last={value!r}')

            def stop(child, label):
                child.send_signal(signal.SIGTERM)
                try:
                    code = child.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
                    raise RuntimeError('p10 timer e2e assertion: graceful stop timeout ' + label)
                if code != 0:
                    stdout, stderr = child.communicate(timeout=5)
                    sys.stderr.write('=== graceful stop failure ' + label + ' exit=' +
                                     str(code) + ' ===\n')
                    sys.stderr.write('stdout: ' + stdout.decode(errors='replace') + '\n')
                    sys.stderr.write('stderr: ' + stderr.decode(errors='replace') + '\n')
                    sys.stderr.flush()
                require(code == 0, 'graceful stop ' + label)

            def start_runtime():
                child, state = rt.daemon(runtime_binary, runtime_path)
                children.append(child)
                require(rt.wait_ready(int(state['probe_port']), True, 20.0),
                        'runtime restart readiness')
                return child

            def start_scheduler():
                child, state = rt.daemon(scheduler_binary, scheduler_path)
                children.append(child)
                require(rt.wait_ready(int(state['probe_port']), True, 20.0),
                        'scheduler restart readiness')
                return child

            def create_named(task_id, delay=2):
                return scheduler_stub.CreateTask(scheduler_pb2.SchedulerCreateTaskRequest(
                    context=rt.context(delegation=proof), task_id=task_id,
                    target=common_pb2.ConversationRef(id='conversation',
                                                       type=common_pb2.ConversationRef.PRIVATE),
                    payload=b'payload', run_at=rt.timestamp(delay)), timeout=10)

            def task_state(task_id):
                try:
                    return get_task_for(task_id).task.state
                except grpc.RpcError:
                    return None

            def get_task_for(task_id):
                return scheduler_stub.GetTask(scheduler_pb2.SchedulerGetTaskRequest(
                    context=rt.context(delegation=proof), task_id=task_id,
                    scope=platform_pb2.ResourceScope(resources=[
                        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                 id=STATE['instance'])])), timeout=10)

            def wait_completed(task_id, timeout=25.0):
                end = time.monotonic() + timeout
                while time.monotonic() < end:
                    if task_state(task_id) == common_pb2.SCHEDULED_TASK_COMPLETED:
                        return
                    time.sleep(0.25)
                raise RuntimeError('p10 timer matrix task never completed: ' + task_id)

            # P13h-1: Runtime platform peer path, restricted to ListTasks/CancelTask and one
            # deployment-authorized instance/installation/account. Other callers keep delegation.
            p13_stub = scheduler_pb2_grpc.SchedulerServiceStub(runtime_scheduler_channel)
            readonly_stub = scheduler_pb2_grpc.SchedulerServiceStub(readonly_scheduler_channel)
            unregistered_stub = scheduler_pb2_grpc.SchedulerServiceStub(
                unregistered_scheduler_channel)
            instance_scope = platform_pb2.ResourceScope(resources=[
                platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                         id=STATE['instance'])])
            platform_task = 'p13h1-platform'
            created_platform = scheduler_stub.CreateTask(
                scheduler_pb2.SchedulerCreateTaskRequest(
                    context=rt.context(delegation=proof), task_id=platform_task,
                    target=common_pb2.ConversationRef(id='conversation',
                                                       type=common_pb2.ConversationRef.PRIVATE),
                    payload=b'platform', run_at=rt.timestamp(60)), timeout=10)
            require(created_platform.task.state == common_pb2.SCHEDULED_TASK_PENDING,
                    'P13h-1 platform task pending')
            listed = p13_stub.ListTasks(scheduler_pb2.SchedulerListTasksRequest(
                context=rt.context(), scope=instance_scope), timeout=10)
            listed_ids = [item.task_id for item in listed.tasks]
            require(platform_task in listed_ids and
                    all(item.owner_instance_id == STATE['instance'] for item in listed.tasks),
                    'P13h-1 platform ListTasks returns target instance tasks only')
            cancelled_platform = p13_stub.CancelTask(scheduler_pb2.SchedulerCancelTaskRequest(
                context=rt.context(), task_id=platform_task, scope=instance_scope), timeout=10)
            require(cancelled_platform.task.state == common_pb2.SCHEDULED_TASK_CANCELLED,
                    'P13h-1 platform CancelTask allowed for owned task')
            for label, request in [
                    ('list cross-instance', lambda: p13_stub.ListTasks(
                        scheduler_pb2.SchedulerListTasksRequest(
                            context=rt.context(),
                            scope=platform_pb2.ResourceScope(resources=[
                                platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                         id=str(uuid.uuid4()))])), timeout=10)),
                    ('cancel cross-instance', lambda: p13_stub.CancelTask(
                        scheduler_pb2.SchedulerCancelTaskRequest(
                            context=rt.context(), task_id=platform_task,
                            scope=platform_pb2.ResourceScope(resources=[
                                platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                         id=str(uuid.uuid4()))])), timeout=10)),
                    ('missing instance scope', lambda: p13_stub.ListTasks(
                        scheduler_pb2.SchedulerListTasksRequest(
                            context=rt.context(),
                            scope=platform_pb2.ResourceScope()), timeout=10))]:
                denied = None
                try:
                    request()
                except grpc.RpcError as error:
                    denied = error.code()
                require(denied == grpc.StatusCode.PERMISSION_DENIED,
                        f'P13h-1 {label} must be PERMISSION_DENIED, got {denied}')
            # Cross-installation/cross-account task rows are owned by the allowed instance but must
            # never be listed or cancelled by this platform peer.
            other_installation = str(uuid.uuid4())
            other_account = str(uuid.uuid4())
            query(scheduler_env,
                  "INSERT INTO kiarabot.tasks(owner,task_id,installation,adapter,account,"
                  "conversation_id,conversation_kind,payload,run_at,state,version,occurrence_id,"
                  "cancel_requested,created_at,updated_at) VALUES "
                  f"('{STATE['instance']}','p13h1-cross-install','{other_installation}','','',"
                  "'conversation',1,''::bytea,9000000000000000000,0,1,'cross-occ-1',false,1,1),"
                  f"('{STATE['instance']}','p13h1-cross-account','{STATE['installation']}','','"
                  f"{other_account}','conversation',1,''::bytea,9000000000000000000,0,1,"
                  "'cross-occ-2',false,1,1);")
            listed = p13_stub.ListTasks(scheduler_pb2.SchedulerListTasksRequest(
                context=rt.context(), scope=instance_scope), timeout=10)
            listed_ids = [item.task_id for item in listed.tasks]
            require('p13h1-cross-install' not in listed_ids and
                    'p13h1-cross-account' not in listed_ids,
                    'P13h-1 ListTasks withholds cross-installation/account tasks')
            for task_id in ('p13h1-cross-install', 'p13h1-cross-account'):
                denied = None
                try:
                    p13_stub.CancelTask(scheduler_pb2.SchedulerCancelTaskRequest(
                        context=rt.context(), task_id=task_id, scope=instance_scope), timeout=10)
                except grpc.RpcError as error:
                    denied = error.code()
                require(denied == grpc.StatusCode.NOT_FOUND,
                        f'P13h-1 CancelTask {task_id} must be NOT_FOUND, got {denied}')
            # Read-only grant can list but cannot cancel. ListTasks only observes active tasks,
            # so keep one pending candidate after the previous cancel.
            create_named('p13h1-readonly', 60)
            readonly_page = readonly_stub.ListTasks(scheduler_pb2.SchedulerListTasksRequest(
                context=rt.context(), scope=instance_scope), timeout=10)
            require(any(item.task_id == 'p13h1-readonly' for item in readonly_page.tasks),
                    'P13h-1 read-only platform peer may list target instance tasks')
            denied = None
            try:
                readonly_stub.CancelTask(scheduler_pb2.SchedulerCancelTaskRequest(
                    context=rt.context(), task_id=STATE['task'], scope=instance_scope), timeout=10)
            except grpc.RpcError as error:
                denied = error.code()
            require(denied == grpc.StatusCode.PERMISSION_DENIED,
                    'P13h-1 missing task.schedule must deny CancelTask')
            # Unregistered certificate/direct legacy path without delegation fails closed; the
            # platform path is not a wildcard for other Scheduler methods either.
            for label, request in [
                    ('unregistered list', lambda: unregistered_stub.ListTasks(
                        scheduler_pb2.SchedulerListTasksRequest(
                            context=rt.context(), scope=instance_scope), timeout=10)),
                    ('runtime cert create', lambda: p13_stub.CreateTask(
                        scheduler_pb2.SchedulerCreateTaskRequest(
                            context=rt.context(), task_id='p13h1-forbidden',
                            target=common_pb2.ConversationRef(
                                id='conversation', type=common_pb2.ConversationRef.PRIVATE),
                            payload=b'x', run_at=rt.timestamp(60)), timeout=10))]:
                denied = None
                try:
                    request()
                except grpc.RpcError as error:
                    denied = error.code()
                require(denied == grpc.StatusCode.UNAUTHENTICATED,
                        f'P13h-1 {label} must be UNAUTHENTICATED, got {denied}')
            query(scheduler_env,
                  "DELETE FROM kiarabot.tasks WHERE task_id IN "
                  "('p13h1-cross-install','p13h1-cross-account');")

            # Capacity: instance 1024 then installation 8192 both reject without over-creation.
            query(scheduler_env,
                  "INSERT INTO kiarabot.tasks(owner,task_id,installation,adapter,account,"
                  "conversation_id,conversation_kind,payload,run_at,state,version,occurrence_id,"
                  "cancel_requested,created_at,updated_at) SELECT "
                  f"'{STATE['instance']}','filler-'||g,'{STATE['installation']}','','',"
                  "'conversation',1,''::bytea,9000000000000000000,0,1,'filler-occ-'||g,false,1,1 "
                  "FROM generate_series(1,1024) g;")
            capacity_denied = None
            try:
                create_named('capacity-instance')
            except grpc.RpcError as error:
                capacity_denied = error.code()
            require(capacity_denied == grpc.StatusCode.RESOURCE_EXHAUSTED,
                    'instance capacity must reject with RATE_LIMITED')
            query(scheduler_env, "DELETE FROM kiarabot.tasks WHERE task_id LIKE 'filler-%';")
            query(scheduler_env,
                  "INSERT INTO kiarabot.tasks(owner,task_id,installation,adapter,account,"
                  "conversation_id,conversation_kind,payload,run_at,state,version,occurrence_id,"
                  "cancel_requested,created_at,updated_at) SELECT "
                  f"'filler-owner-'||g,'inst-filler-'||g,'{STATE['installation']}','','',"
                  "'conversation',1,''::bytea,9000000000000000000,0,1,'inst-occ-'||g,false,1,1 "
                  "FROM generate_series(1,8192) g;")
            installation_denied = None
            try:
                create_named('capacity-installation')
            except grpc.RpcError as error:
                installation_denied = error.code()
            require(installation_denied == grpc.StatusCode.RESOURCE_EXHAUSTED,
                    'installation capacity must reject with RATE_LIMITED')
            query(scheduler_env,
                  "DELETE FROM kiarabot.tasks WHERE task_id LIKE 'inst-filler-%';")

            # EXPIRED: a due trigger whose handoff window is past must become FAILED(TIMEOUT),
            # never auto-replayed. Runtime is down so the first handoff attempt is retryable.
            stop(runtime, 'runtime before expiry')
            runtime = None
            require(create_named('expired', 2).task.state == common_pb2.SCHEDULED_TASK_PENDING,
                    'expiry task pending')
            wait_query(scheduler_env,
                       "SELECT count(*) FROM kiarabot.occurrences WHERE task_id='expired';", '1',
                       15.0, 'expired occurrence creation')
            query(scheduler_env,
                  "UPDATE kiarabot.occurrences SET created_at=0, valid_until=1 "
                  "WHERE task_id='expired';")
            wait_query(scheduler_env,
                       "SELECT state::text||':'||COALESCE(reason,-1)::text FROM kiarabot.tasks "
                       "WHERE task_id='expired';", '4:6', 20.0, 'expired task FAILED(TIMEOUT)')
            wait_query(scheduler_env,
                       "SELECT state::text||':'||COALESCE(last_error,-1)::text "
                       "FROM kiarabot.occurrences WHERE task_id='expired';", '6:6', 5.0,
                       'expired occurrence EXPIRED(TIMEOUT)')
            require(query(scheduler_env,
                          "SELECT count(*) FROM kiarabot.occurrences WHERE task_id='expired';") ==
                    '1', 'expired occurrence must not be replayed')
            runtime = start_runtime()
            require(rt.wait_ready(int(scheduler_state['probe_port']), True, 20.0),
                    'scheduler readiness after runtime restart')

            # Crash recovery: Scheduler is killed after the due sweep created the occurrence, then
            # restarted from the same database; the pending outbox converges to COMPLETED.
            require(scheduler.poll() is None and rt.probe_ready(int(scheduler_state['probe_port'])),
                    'scheduler alive/ready before crash task')
            require(create_named('crash', 2).task.state == common_pb2.SCHEDULED_TASK_PENDING,
                    'crash task pending')
            wait_query(scheduler_env,
                       "SELECT count(*) FROM kiarabot.occurrences WHERE task_id='crash';", '1',
                       15.0, 'crash occurrence creation')
            scheduler.kill()
            scheduler.wait(timeout=10)
            scheduler = start_scheduler()
            wait_completed('crash', 30.0)

            # Lost Scheduler confirmation: Runtime already persisted the occurrence, but Scheduler
            # never confirmed it. After its retry it must observe duplicate and confirm WITHOUT a
            # second event or fan-out target.
            stop(runtime, 'runtime before lost confirm')
            runtime = None
            require(create_named('lost-confirm', 2).task.state ==
                    common_pb2.SCHEDULED_TASK_PENDING, 'lost confirm task pending')
            wait_query(scheduler_env,
                       "SELECT count(*) FROM kiarabot.occurrences WHERE task_id='lost-confirm';",
                       '1', 15.0, 'lost confirm occurrence creation')
            runtime = start_runtime()
            row = query(scheduler_env,
                        "SELECT o.occurrence_id,o.owner,o.task_version,o.operation_id,"
                        "o.created_at,o.valid_until,encode(o.payload_hash,'hex'),"
                        "t.conversation_id,t.conversation_kind,encode(t.payload,'hex') "
                        "FROM kiarabot.occurrences o JOIN kiarabot.tasks t "
                        "ON t.owner=o.owner AND t.task_id=o.task_id WHERE o.task_id='lost-confirm';")
            (lost_occurrence, lost_owner, lost_version, lost_operation, lost_created, lost_valid,
             lost_hash, lost_conversation, lost_kind, lost_payload) = row.split('|')

            def ts_from_ns(value):
                from google.protobuf.timestamp_pb2 import Timestamp
                stamp = Timestamp()
                stamp.FromNanoseconds(int(value))
                return stamp

            lost_handoff = platform_pb2.HandoffContext(
                source_service='Scheduler', message_id=lost_occurrence,
                operation=platform_pb2.OperationRef(
                    owner_service='Scheduler', operation_id=lost_operation,
                    resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_TASK,
                                                      id='lost-confirm')),
                resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_TASK,
                                                  id='lost-confirm'),
                resource_version=int(lost_version), payload_sha256=bytes.fromhex(lost_hash),
                created_at=ts_from_ns(lost_created), valid_until=ts_from_ns(lost_valid))
            lost_request = runtime_pb2.RuntimeAcceptTimerTriggerRequest(
                context=rt.context(), handoff=lost_handoff, occurrence_id=lost_occurrence,
                task_id='lost-confirm', owner_instance_id=lost_owner,
                schedule_version=int(lost_version),
                target=common_pb2.ConversationRef(
                    id=lost_conversation, type=(common_pb2.ConversationRef.PRIVATE
                                                if lost_kind == '1'
                                                else common_pb2.ConversationRef.GROUP)),
                payload=bytes.fromhex(lost_payload))
            first_accept = runtime_peer_stub.AcceptTimerTrigger(lost_request, timeout=10)
            require(first_accept.receipt.event_id == lost_occurrence,
                    'lost-confirm handoff reaches Runtime')
            wait_completed('lost-confirm', 40.0)
            require(query(runtime_env,
                          f"SELECT count(*) FROM kiarabot.events WHERE adapter='scheduler' "
                          f"AND event_id='{lost_occurrence}';") == '1',
                    'lost confirm must not create a second event')
            require(query(runtime_env,
                          f"SELECT count(*) FROM kiarabot.delivery_targets WHERE "
                          f"adapter='scheduler' AND event_id='{lost_occurrence}';") == '1',
                    'lost confirm must not duplicate fan-out')

            # Revocation during maintenance: the task accepted before revocation must still
            # converge; new commands after revocation fail closed.
            assignment_id = None
            require(create_named('revoked-maintenance', 2).task.state ==
                    common_pb2.SCHEDULED_TASK_PENDING, 'revocation task pending')
            for assignment in json.loads((root / 'enrollment.json').read_text())[
                    'administrator_assignments']:
                if 'task.schedule' in assignment.get('permissions', []):
                    assignment_id = assignment['assignment_id']
            require(bool(assignment_id), 'task delegation assignment enrolled')
            identity_stub.RevokeAssignment(identity_pb2.IdentityRevokeAssignmentRequest(
                context=rt.context(), assignment_id=assignment_id, expected_version=1),
                timeout=10)
            # GetTask is itself revoked now, so observe maintenance convergence in the owned DB.
            wait_query(scheduler_env,
                       "SELECT state::text FROM kiarabot.tasks "
                       "WHERE task_id='revoked-maintenance';", '3', 30.0,
                       'revoked task maintenance completion')
            wait_query(scheduler_env,
                       "SELECT state::text FROM kiarabot.occurrences "
                       "WHERE task_id='revoked-maintenance';", '3', 5.0,
                       'revoked occurrence succeeded')
            denied = None
            try:
                create_task()
            except grpc.RpcError as error:
                denied = error.code()
            require(denied in (grpc.StatusCode.PERMISSION_DENIED,
                               grpc.StatusCode.UNAUTHENTICATED),
                    'revoked delegation must not create tasks')

            # P10m regression: repeated SIGTERM after the high-load task churn. Before the OpenSSL
            # atexit fix this intermittently SIGSEGVed inside a worker's RAND thread-local cleanup
            # (EVP_RAND_CTX_free) instead of exiting 0.
            for iteration in range(10):
                query(scheduler_env,
                      "INSERT INTO kiarabot.tasks(owner,task_id,installation,adapter,account,"
                      "conversation_id,conversation_kind,payload,run_at,state,version,occurrence_id,"
                      "cancel_requested,created_at,updated_at) SELECT "
                      f"'shutdown-loop-'||g,'loop-filler-'||g,'{STATE['installation']}','','',"
                      "'conversation',1,''::bytea,9000000000000000000,0,1,'loop-occ-'||g,false,1,1 "
                      "FROM generate_series(1,8192) g;")
                query(scheduler_env,
                      "DELETE FROM kiarabot.tasks WHERE task_id LIKE 'loop-filler-%';")
                stop(runtime, f'runtime shutdown loop {iteration}')
                runtime = start_runtime()
            require(runtime is not None, 'runtime alive after shutdown loop')

            for child, label in ((scheduler, 'scheduler graceful stop'),
                                 (runtime, 'runtime graceful stop')):
                child.send_signal(signal.SIGTERM)
                code = child.wait(timeout=10)
                if code != 0:
                    stdout, stderr = child.communicate(timeout=5)
                    sys.stderr.write('=== graceful stop failure ' + label + ' exit=' +
                                     str(code) + ' ===\n')
                    sys.stderr.write('stdout: ' + stdout.decode(errors='replace') + '\n')
                    sys.stderr.write('stderr: ' + stderr.decode(errors='replace') + '\n')
                    sys.stderr.flush()
                require(code == 0, label)
            identity.send_signal(signal.SIGTERM)
            # Teardown tolerance only: SIGTERM may race Identity's own shutdown checker after the
            # two dependent daemons are already gone; the lifecycle contract itself is covered by
            # architecture_p02_lifecycle.
            require(identity.wait(timeout=10) in (0, -signal.SIGTERM, 128 + signal.SIGTERM),
                    'identity teardown')
        finally:
            for channel in (identity_channel, control_channel, scheduler_channel,
                            scheduler_peer_channel):
                channel.close()
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.send_signal(signal.SIGTERM)
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
    print('Real Scheduler<->Runtime TimerTrigger handoff e2e passed')


def main():
    bootstrap_fixture.main(prepare=prepare, after_bootstrap=lambda root: scenario(
        root, int(sys.argv[2]), Path(sys.argv[6]),
        [str(Path(value).resolve()) for value in sys.argv[7:10]] + [sys.argv[3]]))


if __name__ == '__main__':
    main()
