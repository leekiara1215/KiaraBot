#!/usr/bin/env python3
"""v1.2 Runtime/Scheduler owner-feed real-process e2e (owned PostgreSQL).

Starts the real Identity, Runtime and Scheduler binaries against a disposable PG16.4 fixture with
the offline bootstrap enrollment and temporary mTLS material, then drives the frozen v1.2 feeds:
  * Runtime.ListLogChanges (RESOURCE_CATALOG/control): a real Runtime management mutation appends
    the audit row + redacted log journal row in the same owner transaction, then the cursor page
    returns it; empty continuation pages and typed cursor conflicts are asserted.
  * Runtime.ListMessageMetadataChanges (exact RESOURCE_ACCOUNT): a real AdapterBridge stream
    (bootstrap credential from the compiled seed, real server-verified TLS) registers an adapter
    and accepts one real MessageReceived event; the feed returns only safe received metadata with
    derived canonical ids (never platform raw ids), and a replayed acceptance is a DUPLICATE that
    appends no second row.
  * Scheduler.ListTaskChanges (exact RESOURCE_TASK): real CreateTask/CancelTask owner RPCs stage
    pending/cancelled facts in the same scheduler transaction; multi-page cursor reads and typed
    gap conflicts are asserted.
Every owner RPC is called over real mTLS with the Control client certificate and an Identity-issued
delegation for the exact owner/audience/permission/scope. Refusals (absent optional scope, wrong
owner scope/audience, missing permission) are asserted as stable gRPC codes, never fake empty pages.
This is non-fixture evidence: real service processes + real PG journals + real mTLS.
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
import p06_bridge_acceptance as p06  # noqa: E402
import p14_native_single_host as p14  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402

import grpc  # noqa: E402
from google.protobuf.timestamp_pb2 import Timestamp  # noqa: E402
from kiarabot.v1 import (common_pb2, identity_pb2, identity_pb2_grpc, observations_pb2,  # noqa: E402
                         platform_pb2, policy_pb2, runtime_pb2, runtime_pb2_grpc,
                         scheduler_pb2, scheduler_pb2_grpc)

STATE = {}
ACCOUNT = p06.ACCOUNT
UNBOUND_ACCOUNT = p06.UNBOUND_ACCOUNT
ADAPTER_INSTANCE = p06.ADAPTER_INSTANCE
ADAPTER_ID = p06.ADAPTER_ID
INSTALLATION = p06.INSTALLATION


def stage(name):
    STATE['stage'] = name


def require(condition, label):
    if not condition:
        raise RuntimeError(f'v1.2 owner feed e2e ({STATE.get("stage", "?")}): {label}')


def timestamp_in(seconds=5):
    value = Timestamp()
    value.seconds = int(time.time()) + seconds
    return value


def delegated_context(proof, idempotency=True):
    context = request_context(idempotency)
    context.delegation = proof
    return context


def request_context(idempotency=True):
    return platform_pb2.RequestContext(request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()),
                                       deadline=timestamp_in(),
                                       idempotency_key=str(uuid.uuid4()) if idempotency else '')


def catalog_scope():
    return platform_pb2.ResourceScope(resources=[
        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG, id='control')])


def account_scope(account=ACCOUNT):
    return platform_pb2.ResourceScope(resources=[
        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ACCOUNT, id=account)])


def task_scope(task_ids):
    return platform_pb2.ResourceScope(resources=[
        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_TASK, id=task_id)
        for task_id in task_ids])


def leaf_fingerprint(pem_path):
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem_path.read_text())).digest()


def grant(permissions, scopes):
    return {'assignment_id': str(uuid.uuid4()), 'permissions': permissions,
            'scope': scopes}


def prepare(root, enrolled):
    p14.prepare(root, enrolled)
    STATE['tasks'] = [str(uuid.uuid4()) for _ in range(3)]
    grants = [
        (['log.read'], [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]),
        (['message.read'], [{'kind': 'RESOURCE_ACCOUNT', 'id': ACCOUNT}]),
        (['account.manage'], [{'kind': 'RESOURCE_ACCOUNT', 'id': ACCOUNT}]),
        (['task.schedule'], [{'kind': 'RESOURCE_INSTANCE', 'id': ADAPTER_INSTANCE}]),
        (['task.read'], [{'kind': 'RESOURCE_INSTANCE', 'id': ADAPTER_INSTANCE}]),
        (['task.schedule'], [{'kind': 'RESOURCE_INSTALLATION', 'id': INSTALLATION}]),
        (['task.read'], [{'kind': 'RESOURCE_INSTALLATION', 'id': INSTALLATION}]),
        (['task.schedule'], [{'kind': 'RESOURCE_TASK', 'id': task_id}
                             for task_id in STATE['tasks']]),
        (['task.read'], [{'kind': 'RESOURCE_TASK', 'id': task_id} for task_id in STATE['tasks']]),
    ]
    for permissions, scope in grants:
        for key in ('administrator_assignments', 'control_assignments'):
            enrolled[key].append(grant(permissions, scope))


def sql(database, statement):
    environment, pg_bin = STATE['pg']
    return rt.run_sql(({**environment, 'PGDATABASE': database}, pg_bin), statement)


def start_identity(root, binaries):
    rt.identity_security_material(root)
    config = json.loads((root / 'identity.json').read_text())
    config['probe_listen'] = p14.free_endpoint()
    config['grpc_listen'] = p14.free_endpoint()
    path = root / 'feed-identity.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    child, state = p14.daemon(binaries['identity'], path)
    STATE['children'].append(child)
    require(state.get('ready') == 'true', 'identity readiness')
    require(rt.wait_ready(int(state['probe_port']), True, 20.0), 'identity probe')
    return config['grpc_listen']


def start_service(name, binary, config, children):
    path = STATE['root'] / f'feed-{name}.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    child, state = p14.daemon(binary, path)
    children.append(child)
    require('probe_port' in state and 'grpc_port' in state,
            f'{name} startup line incomplete: {state}')
    require(rt.wait_ready(int(state['probe_port']), True, 20.0), f'{name} probe readiness')
    return state


def service_channel(endpoint, leaf, key, ca):
    credentials = grpc.ssl_channel_credentials(
        root_certificates=ca.read_bytes(), private_key=key.read_bytes(),
        certificate_chain=leaf.read_bytes())
    return grpc.secure_channel(endpoint, credentials)


def issue_delegation(identity, access, audience, scope, permissions, client_certificate,
                     label=''):
    response = identity.IssueDelegation(identity_pb2.IdentityIssueDelegationRequest(
        context=request_context(), subject_proof=access, audience=audience, scope=scope,
        permissions=permissions, client_certificate_sha256=client_certificate), timeout=10)
    require(response.delegation.proof != '', f'{audience} delegation issued')
    return response.delegation.proof


def expect_code(call, codes, label):
    try:
        call()
    except grpc.RpcError as error:
        require(error.code() in codes,
                f'{label}: expected {[code.name for code in codes]}, got {error.code().name}')
        return
    raise RuntimeError(f'{label}: expected a refusal, call succeeded')


def feed_request(scope, delegation, cursor=None, page_size=8, include_scope=True):
    request = observations_pb2.ObservationChangesRequest(context=request_context(),
                                                        page_size=page_size)
    request.context.delegation = delegation
    if include_scope:
        request.scope.CopyFrom(scope)
    if cursor is not None:
        request.cursor.CopyFrom(cursor)
    return request


def page_header(response, expected_scope, owner, page_size):
    require(response.checked_at.seconds > 0, 'checked_at present')
    require(response.next_cursor.source_service == owner, 'next cursor owner')
    require(response.next_cursor.epoch != '', 'next cursor epoch')
    require(len(response.changes) <= page_size, 'page bound')
    actual = sorted((item.kind, item.id) for item in response.next_cursor.scope.resources)
    wanted = sorted((item.kind, item.id) for item in expected_scope.resources)
    require(actual == wanted, 'next cursor canonical scope match')


def log_changes(runtime, delegation, cursor=None, page_size=8, scope=None,
                include_scope=True):
    return runtime.ListLogChanges(feed_request(scope or catalog_scope(), delegation, cursor,
                                              page_size, include_scope), timeout=10)


def message_changes(runtime, delegation, cursor=None, page_size=8, scope=None,
                    include_scope=True):
    return runtime.ListMessageMetadataChanges(feed_request(scope or account_scope(), delegation,
                                                          cursor, page_size, include_scope),
                                             timeout=10)


def task_changes(scheduler, delegation, cursor=None, page_size=8, scope=None,
                 include_scope=True):
    return scheduler.ListTaskChanges(feed_request(scope or task_scope(STATE['tasks']),
                                                  delegation, cursor, page_size, include_scope),
                                    timeout=10)


def check_log_metadata(log):
    require(log.owner_service == 'runtime', 'runtime log owner')
    require(log.severity in ('DEBUG', 'INFO', 'WARN', 'ERROR'), 'closed severity')
    require(log.component == 'runtime', 'runtime component')
    require(log.event in ('request.recorded', 'request.rejected'), 'closed event')
    require(log.method != '' and log.entry_id != '' and log.occurred_at.seconds > 0,
            'log identity/time')
    for value in (log.method, log.component, log.entry_id):
        for forbidden in ('password', 'secret', 'bearer', 'private key', '-----begin'):
            require(forbidden not in value.lower(), 'log redacted projection')


def check_message_metadata(message):
    require(message.owner_service == 'runtime', 'runtime message owner')
    for value in (message.message_id, message.conversation_id, message.account_id):
        require(len(value) == 36 and uuid.UUID(value), 'derived canonical id')
    require(message.message_id != 'message-1' and message.conversation_id != 'group-1',
            'platform raw ids are not projected')
    require(message.account_id == ACCOUNT, 'exact account id')
    require(message.received_at.seconds > 0, 'received_at present')


async def bridge_flow(runtime, msg_delegation, ca, bridge_port):
    (_, adapters, _, events, _, _, _) = p06.proto_modules(REPO)
    grpc_module = p06._grpc()
    channel = grpc_module.aio.secure_channel(
        f'localhost:{bridge_port}',
        grpc_module.ssl_channel_credentials(root_certificates=ca.read_bytes()))
    async with channel:
        call = channel.stream_stream(
            '/kiarabot.v1.AdapterBridge/Connect',
            request_serializer=adapters.AdapterToCore.SerializeToString,
            response_deserializer=adapters.CoreToAdapter.FromString)
        stream = call(metadata=(('authorization', 'Bearer ' + p06.ADAPTER_BOOTSTRAP),))
        await stream.write(adapters.AdapterToCore(
            register=adapters.RegisterAdapterRequest(
                adapter_id=ADAPTER_ID, implementation='feed-e2e',
                implementation_version='1.0.0', protocol_version='kiarabot.v1',
                adapter_instance_id=ADAPTER_INSTANCE,
                accounts=[adapters.Account(account_id=ACCOUNT, enabled=True)],
                capabilities=[common_pb2.Capability(name='action.send_message.text.v1',
                                                    version='1', supported=True)],
                context=request_context())))
        registered = await p06.read_or_fail(stream, 'adapter registration')
        require(registered.WhichOneof('body') == 'register_result' and
                registered.register_result.accepted, 'adapter registration accepted')
        fence = p06.fence_fields(registered.register_result.fence)
        proof = registered.register_result.authorization.proof

        stage('message feed head before acceptance')
        head = message_changes(runtime, msg_delegation)
        page_header(head, account_scope(), 'runtime', 8)
        require(not head.changes, 'message head returns no changes')

        event_id = str(uuid.uuid4())
        envelope = events.EventEnvelope(
            event_id=event_id, trace_id=event_id, schema_version='kiarabot.v1',
            occurred_at=p06.timestamp(time.time()),
            source=common_pb2.SourceRef(adapter_id=ADAPTER_ID,
                                        adapter_instance_id=ADAPTER_INSTANCE,
                                        account_id=ACCOUNT),
            subject=common_pb2.Subject(user_id='10001', conversation_id='group-1',
                                       group_id='group-1'),
            message_received=events.MessageReceived(
                message_id='platform-message-raw',
                sender_id='10001',
                conversation=common_pb2.ConversationRef(id='platform-conversation-raw',
                                                        type=common_pb2.ConversationRef.GROUP,
                                                        account_id=ACCOUNT)))
        frame = adapters.AdapterToCore(fence=fence, connection_proof=proof, event=envelope)
        await stream.write(frame)
        ack = await p06.read_or_fail(stream, 'adapter event ack')
        require(ack.WhichOneof('body') == 'event_ack' and
                ack.event_ack.status == adapters.AdapterEventAck.PERSISTED,
                'first message acceptance persisted')
        stage('message feed page after one acceptance')
        page = message_changes(runtime, msg_delegation, cursor=head.next_cursor)
        page_header(page, account_scope(), 'runtime', 8)
        require(len(page.changes) == 1, 'one accepted message appears once')
        check_message_metadata(page.changes[0].message)
        require(page.changes[0].cursor.source_service == 'runtime', 'change cursor owner')
        STATE['message_cursor'] = platform_pb2.StreamCursor()
        STATE['message_cursor'].CopyFrom(page.next_cursor)
        STATE['message_epoch'] = page.next_cursor.epoch

        stage('duplicate acceptance appends nothing')
        await stream.write(frame)
        duplicate = await p06.read_or_fail(stream, 'duplicate adapter event ack')
        require(duplicate.event_ack.status == adapters.AdapterEventAck.DUPLICATE,
                'replayed acceptance is DUPLICATE')
        replay = message_changes(runtime, msg_delegation, cursor=page.next_cursor)
        require(not replay.changes, 'duplicate acceptance appends no second feed row')
        require(sql('p03_runtime', 'SELECT count(*) FROM kiarabot.observation_feed_changes '
                                   'WHERE family=2;') == '1', 'exactly one message journal row')
        stage('message feed refusals')
        expect_code(lambda: message_changes(runtime, msg_delegation, include_scope=False),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'message feed absent scope')
        expect_code(lambda: message_changes(runtime, msg_delegation,
                                            scope=catalog_scope()),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'message feed wrong owner scope')
        expect_code(lambda: message_changes(runtime, msg_delegation, cursor=head.next_cursor,
                                            scope=account_scope(UNBOUND_ACCOUNT)),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'message feed cursor scope mismatch')


async def run_owner_feeds(root, ca, control_cert, control_key, identity_endpoint,
                          runtime_endpoint, scheduler_endpoint, bridge_port):
    identity_channel = service_channel(identity_endpoint, control_cert, control_key, ca)
    runtime_channel = service_channel(runtime_endpoint, control_cert, control_key, ca)
    scheduler_channel = service_channel(scheduler_endpoint, control_cert, control_key, ca)
    runtime_only_channel = service_channel(runtime_endpoint, STATE['tls'] / 'runtime.pem',
                                           STATE['tls'] / 'runtime.key', ca)
    try:
        for channel in (identity_channel, runtime_channel, scheduler_channel,
                        runtime_only_channel):
            grpc.channel_ready_future(channel).result(timeout=15)
        identity = identity_pb2_grpc.IdentityServiceStub(identity_channel)
        runtime = runtime_pb2_grpc.RuntimeServiceStub(runtime_channel)
        scheduler = scheduler_pb2_grpc.SchedulerServiceStub(scheduler_channel)
        runtime_only = runtime_pb2_grpc.RuntimeServiceStub(runtime_only_channel)
        fingerprint = leaf_fingerprint(control_cert)

        stage('administrator login')
        password = (root / 'admin-password').read_text().strip()
        login = identity.Login(identity_pb2.IdentityLoginRequest(
            context=request_context(), username='administrator', password=password), timeout=10)
        access = login.tokens.access_token

        stage('delegations')
        log_delegation = issue_delegation(
            identity, access, 'runtime', catalog_scope(), [policy_pb2.PERMISSION_LOG_READ],
            fingerprint, 'runtime-log')
        message_delegation = issue_delegation(
            identity, access, 'runtime', account_scope(),
            [policy_pb2.PERMISSION_MESSAGE_READ], fingerprint, 'runtime-message')
        account_delegation = issue_delegation(
            identity, access, 'runtime', account_scope(ACCOUNT),
            [policy_pb2.PERMISSION_ACCOUNT_MANAGE], fingerprint, 'runtime-account')
        schedule_delegation = issue_delegation(
            identity, access, 'scheduler',
            platform_pb2.ResourceScope(resources=[
                platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                         id=ADAPTER_INSTANCE),
                platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTALLATION,
                                         id=INSTALLATION)] +
                list(task_scope(STATE['tasks']).resources)),
            [policy_pb2.PERMISSION_TASK_SCHEDULE], fingerprint, 'scheduler-task')
        task_read_delegation = issue_delegation(
            identity, access, 'scheduler', task_scope(STATE['tasks']),
            [policy_pb2.PERMISSION_TASK_READ], fingerprint, 'scheduler-read')

        stage('bridge message acceptance and message feed')
        await bridge_flow(runtime, message_delegation, ca, bridge_port)

        stage('runtime log feed head')
        log_head = log_changes(runtime, log_delegation)
        page_header(log_head, catalog_scope(), 'runtime', 8)
        require(not log_head.changes, 'runtime log head returns no changes')
        stage('real Runtime management mutation appends audit/log fact')
        updated = runtime.UpdateAccount(runtime_pb2.RuntimeUpdateAccountRequest(
            context=delegated_context(account_delegation), account_id=ACCOUNT,
            display_name='feed-e2e', expected_version=1), timeout=10)
        require(updated.account.account_id == ACCOUNT and
                updated.account.display_name == 'feed-e2e', 'runtime update account audit')
        log_page = log_changes(runtime, log_delegation, cursor=log_head.next_cursor)
        page_header(log_page, catalog_scope(), 'runtime', 8)
        require(log_page.changes, 'real runtime audit row appears after head')
        require(log_page.next_cursor.epoch != STATE['message_epoch'],
                'log and message families keep independent durable epochs')
        for change in log_page.changes:
            check_log_metadata(change.log)
        log_empty = log_changes(runtime, log_delegation, cursor=log_page.next_cursor)
        page_header(log_empty, catalog_scope(), 'runtime', 8)
        require(not log_empty.changes, 'owner-confirmed empty log page is legal')
        stage('runtime log feed typed conflicts and refusals')
        stale = platform_pb2.StreamCursor()
        stale.CopyFrom(log_page.next_cursor)
        stale.epoch = 'stale-epoch'
        expect_code(lambda: log_changes(runtime, log_delegation, cursor=stale),
                    (grpc.StatusCode.ABORTED,), 'log stale epoch typed conflict')
        future = platform_pb2.StreamCursor()
        future.CopyFrom(log_page.next_cursor)
        future.sequence += 1000000
        expect_code(lambda: log_changes(runtime, log_delegation, cursor=future),
                    (grpc.StatusCode.ABORTED,), 'log future sequence typed conflict')
        foreign = platform_pb2.StreamCursor()
        foreign.CopyFrom(log_page.next_cursor)
        foreign.source_service = 'scheduler'
        expect_code(lambda: log_changes(runtime, log_delegation, cursor=foreign),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'log foreign owner cursor')
        expect_code(lambda: log_changes(runtime, log_delegation, include_scope=False),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'log absent scope deployment refusal')
        expect_code(lambda: log_changes(runtime, log_delegation, scope=account_scope()),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'log wrong owner scope')
        expect_code(lambda: log_changes(runtime, message_delegation),
                    (grpc.StatusCode.PERMISSION_DENIED,), 'log requires log.read')
        expect_code(lambda: log_changes(runtime, log_delegation,
                                        cursor=STATE['message_cursor']),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'message-family cursor cannot cross the log feed')
        expect_code(lambda: log_changes(runtime_only, log_delegation),
                    (grpc.StatusCode.UNAUTHENTICATED,), 'log requires Control peer')

        stage('scheduler task feed multi-page')
        task_head = task_changes(scheduler, task_read_delegation, page_size=8)
        page_header(task_head, task_scope(STATE['tasks']), 'scheduler', 8)
        require(not task_head.changes, 'task head returns no changes')
        for index, task_id in enumerate(STATE['tasks']):
            created = scheduler.CreateTask(scheduler_pb2.SchedulerCreateTaskRequest(
                context=delegated_context(schedule_delegation), task_id=task_id,
                target=common_pb2.ConversationRef(id=f'feed-conversation-{index}',
                                                  type=common_pb2.ConversationRef.GROUP,
                                                  account_id=ACCOUNT),
                payload=b'{}', run_at=timestamp_in(3600)), timeout=10)
            require(created.task.task_id == task_id, 'real task creation')
        cancelled = scheduler.CancelTask(scheduler_pb2.SchedulerCancelTaskRequest(
            context=delegated_context(schedule_delegation), task_id=STATE['tasks'][1],
            scope=task_scope([STATE['tasks'][1]])), timeout=10)
        require(cancelled.task.cancel_requested, 'real task cancellation')
        first = task_changes(scheduler, task_read_delegation, cursor=task_head.next_cursor,
                             page_size=2)
        page_header(first, task_scope(STATE['tasks']), 'scheduler', 2)
        require(len(first.changes) == 2 and first.has_more, 'first task page bounded + has_more')
        second = task_changes(scheduler, task_read_delegation, cursor=first.next_cursor,
                              page_size=2)
        page_header(second, task_scope(STATE['tasks']), 'scheduler', 2)
        collect = list(first.changes) + list(second.changes)
        while second.has_more:
            second = task_changes(scheduler, task_read_delegation, cursor=second.next_cursor,
                                  page_size=2)
            page_header(second, task_scope(STATE['tasks']), 'scheduler', 2)
            collect += list(second.changes)
        states = [change.task.state for change in collect]
        require(len(collect) == 4, 'four real task transition rows')
        require(common_pb2.SCHEDULED_TASK_PENDING in states, 'pending task observable')
        require(common_pb2.SCHEDULED_TASK_CANCELLED in states, 'terminal cancelled observable')
        cancelled_rows = [change.task for change in collect
                          if change.task.state == common_pb2.SCHEDULED_TASK_CANCELLED]
        require(cancelled_rows and cancelled_rows[0].cancel_requested, 'cancel requested projected')
        for change in collect:
            require(change.task.owner_service == 'scheduler', 'task owner')
            require(change.task.owner_instance_id != '' and change.task.version > 0,
                    'task metadata identity/version')
            require(change.cursor.source_service == 'scheduler', 'task cursor owner')
        stage('scheduler task feed typed gaps and refusals')
        stale_task = platform_pb2.StreamCursor()
        stale_task.CopyFrom(task_head.next_cursor)
        stale_task.epoch = 'stale-epoch'
        expect_code(lambda: task_changes(scheduler, task_read_delegation, cursor=stale_task),
                    (grpc.StatusCode.ABORTED,), 'task stale epoch typed conflict')
        expect_code(lambda: task_changes(scheduler, task_read_delegation, include_scope=False),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'task absent scope deployment refusal')
        expect_code(lambda: task_changes(scheduler, task_read_delegation, scope=account_scope()),
                    (grpc.StatusCode.INVALID_ARGUMENT,), 'task wrong owner scope')
        expect_code(lambda: task_changes(scheduler, schedule_delegation),
                    (grpc.StatusCode.PERMISSION_DENIED,), 'task feed requires task.read')
        expect_code(lambda: task_changes(scheduler, message_delegation),
                    (grpc.StatusCode.UNAUTHENTICATED,), 'task feed wrong delegation audience')
    finally:
        identity_channel.close()
        runtime_channel.close()
        scheduler_channel.close()
        runtime_only_channel.close()


def build_configs(root, identity_endpoint, runtime_database, scheduler_database):
    tls = STATE['tls']
    ca = str(tls / 'ca.pem')
    endpoints = {name: p14.free_endpoint()
                 for name in ('runtime_grpc', 'runtime_probe', 'runtime_bridge',
                              'scheduler_grpc', 'scheduler_probe')}
    runtime_config = {
        'schema_version': 1, 'service': 'runtime',
        'probe_listen': endpoints['runtime_probe'], 'grpc_listen': endpoints['runtime_grpc'],
        'bridge_listen': endpoints['runtime_bridge'],
        'tls': {'certificate_file': str(tls / 'runtime.pem'),
                'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
        'database': runtime_database, 'identity_endpoint': identity_endpoint,
    }
    scheduler_config = {
        'schema_version': 1, 'service': 'scheduler',
        'probe_listen': endpoints['scheduler_probe'],
        'grpc_listen': endpoints['scheduler_grpc'],
        'tls': {'certificate_file': str(tls / 'scheduler.pem'),
                'private_key_file': str(tls / 'scheduler.key'), 'ca_file': ca},
        'database': scheduler_database, 'identity_endpoint': identity_endpoint,
        'runtime_endpoint': endpoints['runtime_grpc'],
    }
    return endpoints, runtime_config, scheduler_config


def after_bootstrap(root):
    STATE['root'] = root
    STATE['tls'] = root / 'bootstrap-tls'
    STATE['children'] = []
    binaries = STATE['binaries']
    port = int(sys.argv[2])
    stage('databases')
    runtime_database = p14.create_database(root, binaries['pg_bin'], port,
                                           binaries['initializer'], 'runtime')
    scheduler_database = p14.create_database(root, binaries['pg_bin'], port,
                                             binaries['initializer'], 'scheduler')
    stage('runtime seed')
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('PG', 'KIARABOT_'))}
    seeded = subprocess.run(
        [str(binaries['seed']), str(root), str(port), ADAPTER_INSTANCE, ACCOUNT,
         p06.PLUGIN_INSTANCE, p06.ADAPTER_BOOTSTRAP, p06.PLUGIN_BOOTSTRAP, INSTALLATION,
         p06.SHUTDOWN_INSTANCE, p06.SHUTDOWN_BOOTSTRAP],
        capture_output=True, text=True, env=environment, timeout=120)
    require(seeded.returncode == 0 and 'SEEDED' in seeded.stdout,
            f'runtime seed rc={seeded.returncode} err={seeded.stderr.strip()[:120]}')
    STATE['pg'] = rt.postgres(root, binaries['pg_bin'], port)
    try:
        stage('identity startup')
        identity_endpoint = start_identity(root, binaries)
        endpoints, runtime_config, scheduler_config = build_configs(
            root, identity_endpoint, runtime_database, scheduler_database)
        stage('runtime/scheduler startup')
        start_service('runtime', binaries['runtime'], runtime_config, STATE['children'])
        start_service('scheduler', binaries['scheduler'], scheduler_config, STATE['children'])
        control_cert = STATE['tls'] / 'control.pem'
        control_key = STATE['tls'] / 'control.key'
        ca = STATE['tls'] / 'ca.pem'
        stage('bridge message acceptance + owner feeds')
        asyncio.run(run_owner_feeds(
            root, ca, control_cert, control_key, identity_endpoint,
            endpoints['runtime_grpc'], endpoints['scheduler_grpc'],
            int(endpoints['runtime_bridge'].rsplit(':', 1)[1])))
        print('v1.2 Runtime/Scheduler owner feed real-process e2e passed', flush=True)
    finally:
        for child in STATE['children']:
            try:
                if child.poll() is None:
                    p14.stop(child)
            except Exception:  # noqa: BLE001 - bounded cleanup
                pass


def main():
    root = Path(sys.argv[1]).resolve()
    if len(sys.argv) < 12:
        raise RuntimeError('fixture backend directory mismatch')
    port = int(sys.argv[2])
    require(root.name.startswith('kiarabot-p03-') and root.stat().st_uid == os.getuid(),
            'owned fixture root')
    require((root / 'p03-owned-marker').is_file() and not root.is_symlink(),
            'owned fixture marker')
    p14.verify_backend(root, port, sys.argv[6])
    STATE['binaries'] = {
        'initializer': Path(sys.argv[3]).resolve(),
        'identity_bootstrap': Path(sys.argv[5]).resolve(),
        'pg_bin': Path(sys.argv[6]).resolve(),
        'identity': Path(sys.argv[7]).resolve(),
        'runtime': Path(sys.argv[8]).resolve(),
        'scheduler': Path(sys.argv[9]).resolve(),
        'seed': Path(sys.argv[10]).resolve(),
        'openssl': Path(sys.argv[11]).resolve(),
    }
    bootstrap_fixture.main(control_server_certificate=True, prepare=prepare,
                           after_bootstrap=after_bootstrap)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:  # noqa: BLE001 - bounded redacted diagnostic below
        message = str(error)[:600]
        if 'fixture backend directory mismatch' not in message:
            message = f'v1.2 owner feed e2e ({STATE.get("stage", "bootstrap")}): ' + message
        print(message, file=sys.stderr)
        sys.exit(1)
