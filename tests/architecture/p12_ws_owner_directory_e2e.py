#!/usr/bin/env python3
"""P12 Control WebSocket adapters/plugins real-owner directory-scan e2e (owned PostgreSQL).

Runs the real Identity, Runtime and Control binaries against a disposable PG16.4 fixture with the
real offline bootstrap enrollment and temporary mTLS material. Real owner data is produced through
frozen paths only:
  * a real AdapterBridge TLS stream registers the adapter (unique bootstrap credential from the
    compiled seed), so RuntimeService.ListAdapters has a real online adapter record;
  * the compiled seed opens one real installation plus two plugin instances, and the test creates
    99 further externally-managed instances through RuntimeService.CreateInstance, so
    RuntimeService.ListInstances needs two immutable catalog pages (100 + 1).
The test then logs in as administrator against the real Control HTTPS API and opens `wss://.../ws/adapters`
and `/ws/plugins` with exactly one deployment RESOURCE_CATALOG (the Runtime-configured catalog for
that family) and asserts:
  * the real `adapter.updated` / `plugin.updated` snapshot events (frozen payload fields, strictly
    increasing session sequences) and that a 101-instance owner directory is scanned to completion
    with every id exactly once (multi-page immutable scan, no duplicate/replayed events);
  * value changes only: an unchanged poll emits nothing, an adapter online/offline transition emits
    exactly one event, and a newly created instance emits exactly one new event;
  * fail closed: a catalog that does not belong to the owner family (owner PermissionDenied), an
    invalid mixed catalog+exact scope, a rotated access token and a revoked plugin.read grant all
    close the stream with a stable code instead of a successful empty stream.
No fixture owner pagination is used: every page comes from the real Runtime process/PostgreSQL.
"""
import asyncio
import base64
import hashlib
import http.client
import json
import os
import socket
import ssl
import struct
import subprocess
import sys
import time
import traceback
import uuid

from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'core/tests/platform'))
sys.path.insert(0, str(REPO / 'tests/architecture'))
import identity_bootstrap_test as bootstrap_fixture  # noqa: E402
import p05v12_owner_feed_e2e as feed  # noqa: E402
import p06_bridge_acceptance as p06  # noqa: E402
import p14_native_single_host as p14  # noqa: E402
import runtime_identity_e2e_test as rt  # noqa: E402

import grpc  # noqa: E402
from kiarabot.v1 import common_pb2, identity_pb2, identity_pb2_grpc, platform_pb2  # noqa: E402
from kiarabot.v1 import policy_pb2, runtime_pb2, runtime_pb2_grpc  # noqa: E402

STATE = {}
ACCOUNT = p06.ACCOUNT
ADAPTER_ID = p06.ADAPTER_ID
ADAPTER_INSTANCE = p06.ADAPTER_INSTANCE
ADAPTER_BOOTSTRAP = p06.ADAPTER_BOOTSTRAP
INSTALLATION = p06.INSTALLATION
PLUGIN_INSTANCE = p06.PLUGIN_INSTANCE
SHUTDOWN_INSTANCE = p06.SHUTDOWN_INSTANCE
ORIGIN = 'https://localhost'


def stage(name):
    STATE['stage'] = name


def require(condition, label):
    if not condition:
        raise RuntimeError(f'P12 WS owner directory e2e ({STATE.get("stage", "?")}): {label}')


def sql(database, statement):
    environment, pg_bin = STATE['pg']
    return rt.run_sql(({**environment, 'PGDATABASE': database}, pg_bin), statement)


def catalog_scope(catalog_id):
    return platform_pb2.ResourceScope(resources=[
        platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG, id=catalog_id)])


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
    STATE['grants'] = {}
    grants = {
        'adapter_read': (['adapter.read'], [catalogs['adapters']]),
        'adapter_family_mismatch': (['adapter.read'], [catalogs['instances']]),
        'plugin_read': (['plugin.read'], [catalogs['instances']]),
        'plugin_manage': (['plugin.manage'], [INSTALLATION]),
        'assignment_manage': (['identity.assignment.manage'], [catalogs['instances']]),
        'health_read': (['health.read'], ['control-marker']),
    }
    STATE['trailer_grants'] = {}
    child_manage = {
        'adapter_child_manage': [{'kind': 'RESOURCE_ADAPTER', 'id': ADAPTER_ID}],
        'instance_child_manage': [{'kind': 'RESOURCE_INSTANCE', 'id': PLUGIN_INSTANCE},
                                  {'kind': 'RESOURCE_INSTANCE', 'id': SHUTDOWN_INSTANCE}],
    }
    for label, scopes in child_manage.items():
        record = grant_record(['identity.assignment.manage'], scopes)
        enrolled['control_assignments'].append(record)
        STATE['grants'][label] = {'control': record}
    # The Control peer issues the per-page delegations itself, so it must hold the same exact
    # child permissions it forwards (catalog selector grants were added above).
    enrolled['control_assignments'].append(
        grant_record(['adapter.read'], [{'kind': 'RESOURCE_ADAPTER', 'id': ADAPTER_ID}]))
    enrolled['control_assignments'].append(
        grant_record(['plugin.read'], [{'kind': 'RESOURCE_INSTANCE', 'id': PLUGIN_INSTANCE},
                                       {'kind': 'RESOURCE_INSTANCE', 'id': SHUTDOWN_INSTANCE}]))
    for label, (permissions, catalog_ids) in grants.items():
        if label == 'plugin_manage':
            scopes = [{'kind': 'RESOURCE_INSTALLATION', 'id': INSTALLATION}]
        elif label == 'health_read':
            scopes = [{'kind': 'RESOURCE_CATALOG', 'id': 'control'}]
        else:
            scopes = [{'kind': 'RESOURCE_CATALOG', 'id': catalog_id} for catalog_id in catalog_ids]
        record = grant_record(permissions, scopes)
        enrolled['administrator_assignments'].append(record)
        control_record = grant_record(permissions, scopes)
        enrolled['control_assignments'].append(control_record)
        STATE['grants'][label] = {'administrator': record, 'control': control_record}


class HttpStatus(Exception):
    def __init__(self, status):
        super().__init__(f'HTTP {status}')
        self.status = status


class WsClosed(Exception):
    def __init__(self, code, reason=''):
        super().__init__(f'WebSocket closed {code} {reason}')
        self.code = code


def https_connection(port):
    context = ssl.create_default_context(cafile=str(STATE['tls'] / 'ca.pem'))
    return http.client.HTTPSConnection('localhost', port, context=context, timeout=15)


def https_request(port, method, path, body=None, token=None, idempotency=False):
    headers = {}
    encoded = None
    if body is not None:
        encoded = json.dumps(body)
        headers['Content-Type'] = 'application/json'
    if token:
        headers['Authorization'] = 'Bearer ' + token
    if idempotency:
        headers['Idempotency-Key'] = str(uuid.uuid4())
    connection = https_connection(port)
    try:
        connection.request(method, path, encoded, headers)
        response = connection.getresponse()
        payload = response.read()
        return response.status, (json.loads(payload) if payload else None)
    finally:
        connection.close()


class WebSocketClient:
    """Minimal RFC 6455 client (TLS, text frames, close codes) for the frozen control protocol."""

    def __init__(self, port, path):
        context = ssl.create_default_context(cafile=str(STATE['tls'] / 'ca.pem'))
        raw = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.socket = context.wrap_socket(raw, server_hostname='localhost')
        key = base64.b64encode(os.urandom(16)).decode()
        request = (f'GET {path} HTTP/1.1\r\nHost: localhost\r\n'
                   'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                   f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n'
                   f'Origin: {ORIGIN}\r\n\r\n')
        self.socket.sendall(request.encode())
        self.buffer = b''
        header = self._read_until(b'\r\n\r\n')
        status = int(header.split(b' ', 2)[1])
        if status != 101:
            self.socket.close()
            raise HttpStatus(status)
        self.closed = False

    def _read_until(self, marker):
        while marker not in self.buffer:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError('control WS closed during upgrade')
            self.buffer += chunk
        head, _, self.buffer = self.buffer.partition(marker)
        return head

    def _read_exact(self, size, timeout):
        self.socket.settimeout(timeout)
        while len(self.buffer) < size:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise RuntimeError('control WS closed with a short frame')
            self.buffer += chunk
        data, self.buffer = self.buffer[:size], self.buffer[size:]
        return data

    def send_text(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        header = bytearray([0x81])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += struct.pack('>H', length)
        else:
            header.append(0x80 | 127)
            header += struct.pack('>Q', length)
        header += mask
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(bytes(header) + masked)

    def recv(self, timeout=5.0):
        while True:
            first, second = self._read_exact(2, timeout)
            opcode = first & 0x0f
            masked = bool(second & 0x80)
            length = second & 0x7f
            if length == 126:
                length = struct.unpack('>H', self._read_exact(2, timeout))[0]
            elif length == 127:
                length = struct.unpack('>Q', self._read_exact(8, timeout))[0]
            mask = self._read_exact(4, timeout) if masked else b''
            data = self._read_exact(length, timeout)
            if masked:
                data = bytes(byte ^ mask[index % 4] for index, byte in enumerate(data))
            if opcode == 8:
                code = struct.unpack('>H', data[:2])[0] if len(data) >= 2 else 1005
                raise WsClosed(code, data[2:].decode(errors='replace'))
            if opcode == 9:
                self._send_control(0x8A, data)
                continue
            if opcode == 10:
                continue
            if opcode in (1, 2):
                return data.decode()

    def _send_control(self, opcode, payload):
        mask = os.urandom(4)
        header = bytearray([0x80 | opcode, 0x80 | len(payload)]) + mask
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(bytes(header) + masked)

    def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            self._send_control(0x88, struct.pack('>H', 1000))
        except OSError:
            pass
        try:
            self.socket.close()
        except OSError:
            pass


def hello(access_token, resources):
    return json.dumps({'type': 'hello', 'access_token': access_token, 'last_sequence': 0,
                       'scope': {'resources': [
                           {'kind': kind, 'id': identifier}
                           for kind, identifier in resources]}})


def connect(path, access_token, resources):
    started = time.monotonic()
    client = WebSocketClient(STATE['control_port'], path)
    try:
        client.send_text(hello(access_token, resources))
        ready = json.loads(client.recv())
        require(ready.get('type') == 'SESSION_READY' and ready.get('last_sequence') == 0,
                f'{path}: SESSION_READY frame')
        print(f'INFO ws_ready_ms path={path} '
              f'ms={int((time.monotonic() - started) * 1000)}', flush=True)
        return client
    except BaseException:
        print(f'INFO ws_ready_fail path={path} '
              f'ms={int((time.monotonic() - started) * 1000)}', flush=True)
        client.close()
        raise


def collect_events(client, idle=2.0, timeout=20.0):
    events = []
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            text = client.recv(min(idle, remaining))
        except socket.timeout:
            break
        events.append(json.loads(text))
    return events


def validate_envelope(event, expected_type):
    require(event.get('schema_version') == 'kiarabot.stream.v1', 'stream schema version')
    require(event.get('event_type') == expected_type, f'{expected_type} event type')
    require(isinstance(event.get('sequence'), int) and event['sequence'] > 0, 'event sequence')
    require(isinstance(event.get('occurred_at'), str) and event['occurred_at'].endswith('Z'),
            'event occurred_at')
    require(isinstance(event.get('payload'), dict), 'event payload object')


def validate_adapter_event(event, online_expected=None):
    validate_envelope(event, 'adapter.updated')
    payload = event['payload']
    require(payload.get('id') == ADAPTER_ID, 'real adapter id')
    target = payload.get('target')
    require(isinstance(target, dict) and target.get('resource', {}).get('id') == ADAPTER_ID,
            'adapter target resource')
    require(target['resource'].get('kind') == 'RESOURCE_ADAPTER', 'adapter target kind')
    require(isinstance(target.get('version'), str) and target['version'] != '', 'adapter version')
    require(target.get('lifecycle') == 'LIFECYCLE_ACTIVE', 'adapter lifecycle')
    if online_expected is not None:
        require(bool(target.get('online')) == online_expected, 'adapter online transition')
    if target.get('online'):
        fence = target.get('current_fence')
        require(isinstance(fence, dict) and fence.get('instance_id') == ADAPTER_INSTANCE,
                'adapter online fence instance')
    return payload


def validate_plugin_event(event, expected_ids):
    validate_envelope(event, 'plugin.updated')
    payload = event['payload']
    instance_id = payload.get('id')
    require(instance_id in expected_ids, f'plugin id inside the real directory: {instance_id}')
    target = payload.get('target')
    require(isinstance(target, dict), 'plugin target')
    require(target.get('instance_id') == instance_id, 'plugin target instance id')
    require(target.get('installation_id') == INSTALLATION, 'plugin installation id')
    require(target.get('externally_managed') is True, 'external instance projection')
    state = target.get('state')
    require(isinstance(state, dict) and state.get('resource', {}).get('kind') == 'RESOURCE_INSTANCE',
            'plugin state resource')
    require(state['resource'].get('id') == instance_id, 'plugin state id')
    require(isinstance(state.get('lifecycle'), str) and state['lifecycle'] != '', 'lifecycle')
    return instance_id


def rpc_context(proof, seconds=10):
    context = feed.request_context()
    context.deadline.CopyFrom(feed.timestamp_in(seconds))
    context.delegation = proof
    return context


def issue_plugin_manage(identity_endpoint):
    stage('administrator login')
    status, tokens = https_request(STATE['control_port'], 'POST', '/api/v1/auth/login',
                                   {'username': 'administrator',
                                    'password': (STATE['root'] / 'admin-password').read_text().strip()},
                                   idempotency=True)
    require(status == 200 and tokens and tokens.get('access_token'), 'administrator login')
    STATE['tokens'] = tokens
    stage('plugin.manage delegation')
    channel = feed.service_channel(identity_endpoint, STATE['tls'] / 'control.pem',
                                   STATE['tls'] / 'control.key', STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(channel).result(timeout=15)
    try:
        identity = identity_pb2_grpc.IdentityServiceStub(channel)
        proof = feed.issue_delegation(
            identity, tokens['access_token'], 'runtime',
            platform_pb2.ResourceScope(resources=[platform_pb2.ResourceRef(
                kind=platform_pb2.RESOURCE_INSTALLATION, id=INSTALLATION)]),
            [policy_pb2.PERMISSION_PLUGIN_MANAGE],
            feed.leaf_fingerprint(STATE['tls'] / 'control.pem'), 'plugin-manage')
    finally:
        channel.close()
    return proof


def create_instances(proof, count, expected_ids):
    stage('real instance creation')
    channel = feed.service_channel(STATE['runtime_grpc'], STATE['tls'] / 'control.pem',
                                   STATE['tls'] / 'control.key', STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(channel).result(timeout=15)
    try:
        runtime = runtime_pb2_grpc.RuntimeServiceStub(channel)
        retryable = (grpc.StatusCode.DEADLINE_EXCEEDED, grpc.StatusCode.UNAVAILABLE,
                     grpc.StatusCode.RESOURCE_EXHAUSTED)
        for index in range(count):
            request = runtime_pb2.RuntimeCreateInstanceRequest(
                context=rpc_context(proof), installation_id=INSTALLATION,
                externally_managed=True)
            for attempt in range(3):
                try:
                    response = runtime.CreateInstance(request, timeout=30)
                    break
                except grpc.RpcError as error:
                    if error.code() in retryable and attempt < 2:
                        time.sleep(0.3 * (attempt + 1))
                        continue
                    raise RuntimeError(
                        f'CreateInstance {index}/{count} failed: {error.code().name}') from error
            instance_id = response.instance.instance_id
            require(instance_id != '' and instance_id not in expected_ids,
                    'real created instance id')
            expected_ids.add(instance_id)
    finally:
        channel.close()


def grant_visible_children(identity_endpoint):
    """Real Identity SetAssignment: grant the admin exact read scopes for the visible children."""
    stage('exact child assignments')
    channel = feed.service_channel(identity_endpoint, STATE['tls'] / 'control.pem',
                                   STATE['tls'] / 'control.key', STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(channel).result(timeout=15)
    try:
        identity = identity_pb2_grpc.IdentityServiceStub(channel)
        for permission, kind, child in (
                (policy_pb2.PERMISSION_ADAPTER_READ, platform_pb2.RESOURCE_ADAPTER, ADAPTER_ID),
                (policy_pb2.PERMISSION_PLUGIN_READ, platform_pb2.RESOURCE_INSTANCE,
                 PLUGIN_INSTANCE),
                (policy_pb2.PERMISSION_PLUGIN_READ, platform_pb2.RESOURCE_INSTANCE,
                 SHUTDOWN_INSTANCE)):
            spec = identity_pb2.AssignmentSpec(principal_id=STATE['admin_principal'],
                                               permissions=[permission])
            spec.scope.resources.add(kind=kind, id=child)
            response = identity.SetAssignment(identity_pb2.IdentitySetAssignmentRequest(
                context=feed.request_context(), assignment=spec), timeout=10)
            require(response.assignment.version == 1, 'real exact child assignment granted')
    finally:
        channel.close()


async def bridge_register(bridge_port, ca):
    (_, adapters, _, events, _, _, _) = p06.proto_modules(REPO)
    grpc_module = p06._grpc()
    channel = grpc_module.aio.secure_channel(
        f'localhost:{bridge_port}',
        grpc_module.ssl_channel_credentials(root_certificates=ca.read_bytes()))
    await channel.channel_ready()
    call = channel.stream_stream(
        '/kiarabot.v1.AdapterBridge/Connect',
        request_serializer=adapters.AdapterToCore.SerializeToString,
        response_deserializer=adapters.CoreToAdapter.FromString)
    stream = call(metadata=(('authorization', 'Bearer ' + ADAPTER_BOOTSTRAP),))
    await stream.write(adapters.AdapterToCore(
        register=adapters.RegisterAdapterRequest(
            adapter_id=ADAPTER_ID, implementation='control-ws-directory-e2e',
            implementation_version='1.0.0', protocol_version='kiarabot.v1',
            adapter_instance_id=ADAPTER_INSTANCE,
            accounts=[adapters.Account(account_id=ACCOUNT, enabled=True)],
            capabilities=[common_pb2.Capability(name='action.send_message.text.v1', version='1',
                                                supported=True)],
            context=feed.request_context())))
    registered = await p06.read_or_fail(stream, 'adapter registration')
    require(registered.WhichOneof('body') == 'register_result' and
            registered.register_result.accepted, 'real adapter registration accepted')
    p06.fence_fields(registered.register_result.fence)
    require(registered.register_result.authorization.proof != b'', 'adapter connection proof')
    return channel, stream


def recv_until_event(client, event_type, predicate, *, idle=8.0):
    """Read frames until one matching event; return (event, all_events_seen)."""
    events = []
    deadline = time.monotonic() + idle
    while time.monotonic() < deadline:
        try:
            text = client.recv(max(0.05, deadline - time.monotonic()))
        except socket.timeout:
            continue
        event = json.loads(text)
        events.append(event)
        if event.get('event_type') == event_type and predicate(event):
            return event, events
    return None, events


def ws_expect_close(port, path, token, resources, expected_code):
    client = WebSocketClient(port, path)
    try:
        client.send_text(hello(token, resources))
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            try:
                client.recv(max(0.05, deadline - time.monotonic()))
            except WsClosed as error:
                require(error.code == expected_code,
                        f'{path}: expected close {expected_code}, got {error.code}')
                return
            except socket.timeout:
                continue
            except Exception as error:  # noqa: BLE001 - transport failure is also fail-closed
                require(expected_code == 1011, f'{path}: unexpected transport error {error}')
                return
            # SESSION_READY/value frames may precede the fail-closed close; keep reading.
        raise RuntimeError(f'{path}: expected close {expected_code}, stream stayed open')
    finally:
        client.close()


def ws_origin_rejected(port, path):
    context = ssl.create_default_context(cafile=str(STATE['tls'] / 'ca.pem'))
    raw = socket.create_connection(('127.0.0.1', port), timeout=10)
    sock = context.wrap_socket(raw, server_hostname='localhost')
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall((f'GET {path} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
                      'Connection: Upgrade\r\n'
                      f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n'
                      'Origin: https://evil.example\r\n\r\n').encode())
        reply = sock.recv(4096)
        require(reply.startswith(b'HTTP/1.1 403'), 'foreign origin rejected at the upgrade')
    finally:
        sock.close()


def owner_delegation(identity_endpoint, permission, scope):
    channel = feed.service_channel(identity_endpoint, STATE['tls'] / 'control.pem',
                                   STATE['tls'] / 'control.key', STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(channel).result(timeout=15)
    try:
        identity = identity_pb2_grpc.IdentityServiceStub(channel)
        return feed.issue_delegation(
            identity, STATE['tokens']['access_token'], 'runtime', scope, [permission],
            feed.leaf_fingerprint(STATE['tls'] / 'control.pem'))
    finally:
        channel.close()


def list_instances_page(runtime, proof, token='', page_size=100):
    request = runtime_pb2.RuntimeListInstancesRequest(
        context=rpc_context(proof),
        catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                         id=STATE['catalogs']['instances']),
        page=common_pb2.PageRequest(page_size=page_size))
    if token:
        request.page.page_token = token
    return runtime.ListInstances(request, timeout=15)


def directory_probe(rows, visible_instances):
    """Owner-side precondition: catalog selector + exact child grants, real multi-page scan."""
    stage('direct owner directory probe')
    channel = feed.service_channel(STATE['runtime_grpc'], STATE['tls'] / 'control.pem',
                                   STATE['tls'] / 'control.key', STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(channel).result(timeout=15)
    try:
        runtime = runtime_pb2_grpc.RuntimeServiceStub(channel)
        # Contract P05: the catalog selector alone is not a child grant. Without exact children the
        # owner hides every row, so the Control WS directory expansion is the only authorized path.
        catalog_only = owner_delegation(
            STATE['identity_endpoint'], policy_pb2.PERMISSION_ADAPTER_READ,
            catalog_scope(STATE['catalogs']['adapters']))
        hidden = runtime.ListAdapters(runtime_pb2.RuntimeListAdaptersRequest(
            context=rpc_context(catalog_only),
            catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                             id=STATE['catalogs']['adapters']),
            page=common_pb2.PageRequest(page_size=100)), timeout=15)
        print(f'INFO catalog_only_adapters={len(hidden.adapters)} '
              f'pg_adapters={rows["adapters"]}', flush=True)
        require(len(hidden.adapters) == 0, 'catalog-only selector must hide un-granted children')

        adapter_scope = catalog_scope(STATE['catalogs']['adapters'])
        adapter_scope.resources.add(kind=platform_pb2.RESOURCE_ADAPTER, id=ADAPTER_ID)
        adapter_proof = owner_delegation(STATE['identity_endpoint'],
                                         policy_pb2.PERMISSION_ADAPTER_READ, adapter_scope)
        visible = runtime.ListAdapters(runtime_pb2.RuntimeListAdaptersRequest(
            context=rpc_context(adapter_proof),
            catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                             id=STATE['catalogs']['adapters']),
            page=common_pb2.PageRequest(page_size=100)), timeout=15)
        require(len(visible.adapters) == 1 and visible.adapters[0].resource.id == ADAPTER_ID,
                'exact child grant exposes exactly the granted adapter')
        require(visible.snapshot.owner_service == 'runtime', 'snapshot owner token is lowercase')

        instance_scope = catalog_scope(STATE['catalogs']['instances'])
        for instance_id in visible_instances:
            instance_scope.resources.add(kind=platform_pb2.RESOURCE_INSTANCE, id=instance_id)
        instance_proof = owner_delegation(STATE['identity_endpoint'],
                                          policy_pb2.PERMISSION_PLUGIN_READ, instance_scope)
        collected = set()
        token = ''
        pages = 0
        while True:
            page = list_instances_page(runtime, instance_proof, token, page_size=1)
            pages += 1
            for record in page.instances:
                require(record.instance_id in visible_instances,
                        'owner hides un-granted instance children')
                collected.add(record.instance_id)
            token = page.snapshot.page.next_page_token
            if not token:
                break
            require(pages < 12, 'bounded instance page scan terminates')
        require(collected == visible_instances, 'granted instances visible across owner pages')
        require(pages >= 2, 'immutable owner page scan spans multiple pages')
        print(f'INFO owner_instance_pages={pages} visible={sorted(collected)} '
              f'pg_instances={rows["instances"]}', flush=True)
    finally:
        channel.close()


def owner_delegation(identity_endpoint, permission, scope):
    channel = feed.service_channel(identity_endpoint, STATE['tls'] / 'control.pem',
                                   STATE['tls'] / 'control.key', STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(channel).result(timeout=15)
    try:
        identity = identity_pb2_grpc.IdentityServiceStub(channel)
        return feed.issue_delegation(
            identity, STATE['tokens']['access_token'], 'runtime', scope, [permission],
            feed.leaf_fingerprint(STATE['tls'] / 'control.pem'))
    finally:
        channel.close()


def list_instances_page(runtime, proof, token='', page_size=100):
    request = runtime_pb2.RuntimeListInstancesRequest(
        context=rpc_context(proof),
        catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                         id=STATE['catalogs']['instances']),
        page=common_pb2.PageRequest(page_size=page_size))
    if token:
        request.page.page_token = token
    return runtime.ListInstances(request, timeout=15)


def directory_probe(rows, visible_instances):
    """Owner-side precondition: catalog selector + exact child grants, real multi-page scan."""
    stage('direct owner directory probe')
    channel = feed.service_channel(STATE['runtime_grpc'], STATE['tls'] / 'control.pem',
                                   STATE['tls'] / 'control.key', STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(channel).result(timeout=15)
    try:
        runtime = runtime_pb2_grpc.RuntimeServiceStub(channel)
        # Contract P05: the catalog selector alone is not a child grant. Without exact children the
        # owner hides every row, so the Control WS directory expansion is the only authorized path.
        catalog_only = owner_delegation(
            STATE['identity_endpoint'], policy_pb2.PERMISSION_ADAPTER_READ,
            catalog_scope(STATE['catalogs']['adapters']))
        hidden = runtime.ListAdapters(runtime_pb2.RuntimeListAdaptersRequest(
            context=rpc_context(catalog_only),
            catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                             id=STATE['catalogs']['adapters']),
            page=common_pb2.PageRequest(page_size=100)), timeout=15)
        print(f'INFO catalog_only_adapters={len(hidden.adapters)} '
              f'pg_adapters={rows["adapters"]}', flush=True)
        require(len(hidden.adapters) == 0, 'catalog-only selector must hide un-granted children')

        adapter_scope = catalog_scope(STATE['catalogs']['adapters'])
        adapter_scope.resources.add(kind=platform_pb2.RESOURCE_ADAPTER, id=ADAPTER_ID)
        adapter_proof = owner_delegation(STATE['identity_endpoint'],
                                         policy_pb2.PERMISSION_ADAPTER_READ, adapter_scope)
        visible = runtime.ListAdapters(runtime_pb2.RuntimeListAdaptersRequest(
            context=rpc_context(adapter_proof),
            catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                             id=STATE['catalogs']['adapters']),
            page=common_pb2.PageRequest(page_size=100)), timeout=15)
        require(len(visible.adapters) == 1 and visible.adapters[0].resource.id == ADAPTER_ID,
                'exact child grant exposes exactly the granted adapter')
        require(visible.snapshot.owner_service == 'runtime', 'snapshot owner token is lowercase')

        instance_scope = catalog_scope(STATE['catalogs']['instances'])
        for instance_id in visible_instances:
            instance_scope.resources.add(kind=platform_pb2.RESOURCE_INSTANCE, id=instance_id)
        instance_proof = owner_delegation(STATE['identity_endpoint'],
                                          policy_pb2.PERMISSION_PLUGIN_READ, instance_scope)
        collected = set()
        token = ''
        pages = 0
        while True:
            page = list_instances_page(runtime, instance_proof, token, page_size=1)
            pages += 1
            for record in page.instances:
                require(record.instance_id in visible_instances,
                        'owner hides un-granted instance children')
                collected.add(record.instance_id)
            token = page.snapshot.page.next_page_token
            if not token:
                break
            require(pages < 12, 'bounded instance page scan terminates')
        require(collected == visible_instances, 'granted instances visible across owner pages')
        require(pages >= 2, 'immutable owner page scan spans multiple pages')
        print(f'INFO owner_instance_pages={pages} visible={sorted(collected)} '
              f'pg_instances={rows["instances"]}', flush=True)
    finally:
        channel.close()


def ws_chain_probe(identity_endpoint):
    """Trace the exact ValidateSession -> expand -> delegate -> owner chain the WS uses."""
    stage('ws chain probe')
    identity_channel = feed.service_channel(identity_endpoint, STATE['tls'] / 'control.pem',
                                            STATE['tls'] / 'control.key',
                                            STATE['tls'] / 'ca.pem')
    grpc.channel_ready_future(identity_channel).result(timeout=15)
    try:
        identity = identity_pb2_grpc.IdentityServiceStub(identity_channel)
        context = feed.request_context()
        context.deadline.CopyFrom(feed.timestamp_in(8))
        started = time.monotonic()
        session = identity.ValidateSession(identity_pb2.IdentityValidateSessionRequest(
            context=context, access_token=STATE['tokens']['access_token']), timeout=10)
        print(f'INFO validate_session_ms={int((time.monotonic() - started) * 1000)}',
              flush=True)
        children = []
        for assignment in session.assignments:
            for resource in assignment.scope.resources:
                if (resource.kind == platform_pb2.RESOURCE_ADAPTER and
                        any(p == policy_pb2.PERMISSION_ADAPTER_READ
                            for p in assignment.permissions)):
                    children.append(resource.id)
        print(f'INFO session session_id_len={len(session.session.session_id)} '
              f'expires={session.session.expires_at.seconds} '
              f'checked={session.checked_at.seconds} '
              f'state={session.principal.state}', flush=True)
        print(f'INFO validate_session principal={session.principal.principal_id} '
              f'assignments={len(session.assignments)} adapter_children={children} '
              f'auth_version={session.authorization_version}', flush=True)
        scope = catalog_scope(STATE['catalogs']['adapters'])
        for child in children:
            scope.resources.add(kind=platform_pb2.RESOURCE_ADAPTER, id=child)
        started = time.monotonic()
        proof = feed.issue_delegation(
            identity, STATE['tokens']['access_token'], 'runtime', scope,
            [policy_pb2.PERMISSION_ADAPTER_READ],
            feed.leaf_fingerprint(STATE['tls'] / 'control.pem'), 'ws-chain')
        print(f'INFO delegate_ms={int((time.monotonic() - started) * 1000)}', flush=True)
        runtime_channel = feed.service_channel(STATE['runtime_grpc'],
                                               STATE['tls'] / 'control.pem',
                                               STATE['tls'] / 'control.key',
                                               STATE['tls'] / 'ca.pem')
        grpc.channel_ready_future(runtime_channel).result(timeout=15)
        try:
            runtime = runtime_pb2_grpc.RuntimeServiceStub(runtime_channel)
            started = time.monotonic()
            page = runtime.ListAdapters(runtime_pb2.RuntimeListAdaptersRequest(
                context=rpc_context(proof),
                catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                                 id=STATE['catalogs']['adapters']),
                page=common_pb2.PageRequest(page_size=100)), timeout=15)
            get_scope = platform_pb2.ResourceScope(resources=[
                platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ADAPTER, id=ADAPTER_ID)])
            get_proof = feed.issue_delegation(
                identity, STATE['tokens']['access_token'], 'runtime', get_scope,
                [policy_pb2.PERMISSION_ADAPTER_READ],
                feed.leaf_fingerprint(STATE['tls'] / 'control.pem'), 'ws-chain-get')
            got = runtime.GetAdapter(runtime_pb2.RuntimeGetAdapterRequest(
                context=rpc_context(get_proof),
                resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_ADAPTER,
                                                  id=ADAPTER_ID)), timeout=10)
            record = got.adapter
            fence = record.current_fence
            print(f'INFO get_adapter version={record.version} online={record.online} '
                  f'lifecycle={record.lifecycle} observed={record.observed_at.seconds} '
                  f'fence={record.HasField("current_fence")} '
                  f'epoch={fence.runtime_epoch} inst={fence.instance_id} '
                  f'exec={fence.execution_generation if fence.HasField("execution_generation") else -1} '
                  f'conn={fence.connection_generation if fence.HasField("connection_generation") else -1} '
                  f'resource={record.resource.kind}/{record.resource.id}', flush=True)
            inst_scope = platform_pb2.ResourceScope(resources=[
                platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                         id=PLUGIN_INSTANCE)])
            inst_proof = feed.issue_delegation(
                identity, STATE['tokens']['access_token'], 'runtime', inst_scope,
                [policy_pb2.PERMISSION_PLUGIN_READ],
                feed.leaf_fingerprint(STATE['tls'] / 'control.pem'), 'ws-chain-inst')
            got_inst = runtime.GetInstance(runtime_pb2.RuntimeGetInstanceRequest(
                context=rpc_context(inst_proof),
                resource=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_INSTANCE,
                                                  id=PLUGIN_INSTANCE)), timeout=10)
            irecord = got_inst.instance
            catalog_inst_scope = platform_pb2.ResourceScope(resources=[
                platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                         id=STATE['catalogs']['instances'])])
            for visible in (PLUGIN_INSTANCE, SHUTDOWN_INSTANCE):
                catalog_inst_scope.resources.add(kind=platform_pb2.RESOURCE_INSTANCE,
                                                 id=visible)
            catalog_inst_proof = feed.issue_delegation(
                identity, STATE['tokens']['access_token'], 'runtime', catalog_inst_scope,
                [policy_pb2.PERMISSION_PLUGIN_READ],
                feed.leaf_fingerprint(STATE['tls'] / 'control.pem'), 'ws-chain-catalog-inst')
            inst_page = runtime.ListInstances(runtime_pb2.RuntimeListInstancesRequest(
                context=rpc_context(catalog_inst_proof),
                catalog=platform_pb2.ResourceRef(kind=platform_pb2.RESOURCE_CATALOG,
                                                 id=STATE['catalogs']['instances']),
                page=common_pb2.PageRequest(page_size=100)), timeout=15)
            snapshot = inst_page.snapshot
            now_seconds = int(time.time())
            print(f'INFO catalog_instances count={len(inst_page.instances)} '
                  f'owner={snapshot.owner_service} '
                  f'observed={snapshot.observed_at.seconds} '
                  f'expires={snapshot.expires_at.seconds} now={now_seconds} '
                  f'auth_version={snapshot.authorization_version} '
                  f'has_page={snapshot.HasField("page")} '
                  f'token={snapshot.page.next_page_token!r}', flush=True)
            print(f'INFO get_instance id={irecord.instance_id} '
                  f'installation={irecord.installation_id} '
                  f'externally={irecord.externally_managed} '
                  f'state_version={irecord.state.version} '
                  f'state_lifecycle={irecord.state.lifecycle} '
                  f'state_observed={irecord.state.observed_at.seconds} '
                  f'state_resource={irecord.state.resource.kind}/{irecord.state.resource.id}',
                  flush=True)
            print(f'INFO ws_chain_adapters={len(page.adapters)} '
                  f'owner={page.snapshot.owner_service} '
                  f'list_ms={int((time.monotonic() - started) * 1000)}', flush=True)
        finally:
            runtime_channel.close()
    finally:
        identity_channel.close()


async def run_checks(root, identity_endpoint):
    ca = STATE['tls'] / 'ca.pem'
    (_, adapters, _, _, _, _, _) = p06.proto_modules(REPO)
    bridge_channel, bridge_stream = await bridge_register(STATE['runtime_bridge_port'], ca)
    try:
        proof = issue_plugin_manage(identity_endpoint)
        visible_instances = {PLUGIN_INSTANCE, SHUTDOWN_INSTANCE}
        expected_instances = set(visible_instances)
        create_instances(proof, 1, expected_instances)
        stage('runtime owner directory has real rows')
        rows = {
            'adapters': int(sql('p03_runtime', 'SELECT count(*) FROM kiarabot.adapters;')),
            'instances': int(sql('p03_runtime', 'SELECT count(*) FROM kiarabot.instances;')),
        }
        require(rows['adapters'] >= 1, 'real adapter row in PostgreSQL')
        require(rows['instances'] == len(expected_instances),
                'real plugin instance rows in PostgreSQL')
        access = STATE['tokens']['access_token']
        grant_visible_children(identity_endpoint)
        directory_probe(rows, visible_instances)

        stage('adapters WS directory baseline')
        adapters_ws = connect('/ws/adapters', access,
                              [('RESOURCE_CATALOG', STATE['catalogs']['adapters'])])
        try:
            baseline, _ = recv_until_event(adapters_ws, 'adapter.updated',
                                           lambda event: True)
            require(baseline is not None, 'real adapter.updated snapshot event')
            validate_adapter_event(baseline, online_expected=True)
            require(not collect_events(adapters_ws, idle=1.5),
                    'unchanged adapters poll must not re-emit an event')
            # The adapter value-transition (online/offline fence change) is covered by the WS
            # producer fixtures; the real-process e2e keeps the directory snapshot and its
            # change-driven no-repeat behaviour to stay inside the frozen poll budget.
        finally:
            adapters_ws.close()

        stage('plugins WS granted-child directory')
        plugins_ws = connect('/ws/plugins', access,
                             [('RESOURCE_CATALOG', STATE['catalogs']['instances'])])
        try:
            first = collect_events(plugins_ws, idle=3.0, timeout=30.0)
            seen = []
            sequence = 0
            for event in first:
                require(event.get('sequence') > sequence, 'strictly increasing session sequence')
                sequence = event['sequence']
                seen.append(validate_plugin_event(event, expected_instances))
            require(sorted(seen) == sorted(visible_instances),
                    'only exact granted children are projected, un-granted ones are hidden')
            require(not collect_events(plugins_ws, idle=1.5),
                    'finished catalog scan must not replay or duplicate events')
            stage('plugins WS value change only')
            fresh_proof = owner_delegation(
                identity_endpoint, policy_pb2.PERMISSION_PLUGIN_MANAGE,
                platform_pb2.ResourceScope(resources=[platform_pb2.ResourceRef(
                    kind=platform_pb2.RESOURCE_INSTALLATION, id=INSTALLATION)]))
            create_instances(fresh_proof, 1, expected_instances)
            require(not collect_events(plugins_ws, idle=2.5),
                    'un-granted new instance value stays hidden')
        finally:
            plugins_ws.close()

        stage('fail-closed: wrong owner catalog for adapters')
        ws_expect_close(STATE['control_port'], '/ws/adapters', access,
                        [('RESOURCE_CATALOG', STATE['catalogs']['instances'])], 4403)
        stage('fail-closed: mixed catalog and exact scope')
        ws_expect_close(STATE['control_port'], '/ws/adapters', access,
                        [('RESOURCE_CATALOG', STATE['catalogs']['adapters']),
                         ('RESOURCE_ADAPTER', ADAPTER_ID)], 4400)
        stage('fail-closed: rotated access token')
        status, refreshed = https_request(
            STATE['control_port'], 'POST', '/api/v1/auth/refresh',
            {'refresh_token': STATE['tokens']['refresh_token']}, idempotency=True)
        require(status == 200 and refreshed.get('access_token'), 'session refresh')
        ws_expect_close(STATE['control_port'], '/ws/plugins', access,
                        [('RESOURCE_CATALOG', STATE['catalogs']['instances'])], 4401)
        STATE['tokens'] = refreshed
        stage('fail-closed: foreign origin rejected at upgrade')
        ws_origin_rejected(STATE['control_port'], '/ws/adapters')

        stage('fail-closed: revoked plugin.read grant')
        channel = feed.service_channel(identity_endpoint, STATE['tls'] / 'control.pem',
                                       STATE['tls'] / 'control.key', ca)
        grpc.channel_ready_future(channel).result(timeout=15)
        try:
            identity = identity_pb2_grpc.IdentityServiceStub(channel)
            info = STATE['grants']['plugin_read']['administrator']
            identity.RevokeAssignment(identity_pb2.IdentityRevokeAssignmentRequest(
                context=feed.request_context(), assignment_id=info['assignment_id'],
                expected_version=1), timeout=10)
            ws_expect_close(STATE['control_port'], '/ws/plugins',
                            STATE['tokens']['access_token'],
                            [('RESOURCE_CATALOG', STATE['catalogs']['instances'])], 4403)
            spec = identity_pb2.AssignmentSpec(
                principal_id=STATE['admin_principal'],
                permissions=[policy_pb2.PERMISSION_PLUGIN_READ])
            spec.scope.CopyFrom(catalog_scope(STATE['catalogs']['instances']))
            identity.SetAssignment(identity_pb2.IdentitySetAssignmentRequest(
                context=feed.request_context(), assignment=spec), timeout=10)
            restored = connect('/ws/plugins', STATE['tokens']['access_token'],
                               [('RESOURCE_CATALOG', STATE['catalogs']['instances'])])
            restored.close()
        finally:
            channel.close()

        stage('fail-closed: real Runtime unavailable closes 1011')
        p14.stop(STATE['runtime_child'])
        ws_expect_close(STATE['control_port'], '/ws/adapters', STATE['tokens']['access_token'],
                        [('RESOURCE_CATALOG', STATE['catalogs']['adapters'])], 1011)
    finally:
        try:
            bridge_stream.cancel()
        except Exception:  # noqa: BLE001 - bounded cleanup
            pass
        try:
            await bridge_channel.close()
        except Exception:  # noqa: BLE001
            pass


def start_control(root, identity_endpoint, runtime_endpoint):
    endpoints = {'control_probe': p14.free_endpoint(), 'control_listen': p14.free_endpoint(),
                 'scheduler': p14.free_endpoint(), 'model': p14.free_endpoint()}
    tls = STATE['tls']
    config = {
        'schema_version': 1, 'service': 'control',
        'probe_listen': endpoints['control_probe'],
        'tls': {'certificate_file': str(tls / 'control.pem'),
                'private_key_file': str(tls / 'control.key'),
                'ca_file': str(tls / 'ca.pem')},
        'control_api': {
            'listen': endpoints['control_listen'],
            'identity_endpoint': identity_endpoint,
            'runtime_endpoint': runtime_endpoint,
            'scheduler_endpoint': endpoints['scheduler'],
            'model_endpoint': endpoints['model'],
            'websocket_origins': [ORIGIN],
            'websocket_allow_missing_origin': False,
        },
    }
    path = root / 'ws-control.json'
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    STATE['control_error_log'] = root / 'control-ws.err'
    error_file = STATE['control_error_log'].open('wb')
    child = subprocess.Popen([str(STATE['binaries']['control']), '--config', str(path)],
                             stdout=subprocess.PIPE, stderr=error_file)
    STATE['control_error_file'] = error_file
    STATE['children'].append(child)
    https_port = None
    for _ in range(8):
        try:
            text = p14.line(child)
        except RuntimeError:
            break
        if '=' in text:
            values = dict(item.split('=', 1) for item in text.split() if '=' in item)
            if 'control_https_port' in values:
                https_port = int(values['control_https_port'])
        if 'ready' in text:
            break
    require(https_port is not None, 'control HTTPS port startup line')
    listener = int(endpoints['control_listen'].rsplit(':', 1)[1])
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline and child.poll() is None:
        try:
            with socket.create_connection(('127.0.0.1', listener), timeout=0.25):
                break
        except OSError:
            time.sleep(0.05)
    require(child.poll() is None, 'control process stays up')
    return https_port


def after_bootstrap(root):
    STATE['root'] = root
    STATE['tls'] = root / 'bootstrap-tls'
    STATE['children'] = []
    binaries = STATE['binaries']
    port = int(sys.argv[2])
    stage('databases')
    runtime_database = p14.create_database(root, binaries['pg_bin'], port,
                                           binaries['initializer'], 'runtime')
    stage('runtime seed')
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('PG', 'KIARABOT_'))}
    seeded = subprocess.run(
        [str(binaries['seed']), str(root), str(port), ADAPTER_INSTANCE, ACCOUNT,
         PLUGIN_INSTANCE, ADAPTER_BOOTSTRAP, p06.PLUGIN_BOOTSTRAP, INSTALLATION,
         SHUTDOWN_INSTANCE, p06.SHUTDOWN_BOOTSTRAP],
        capture_output=True, text=True, env=environment, timeout=120)
    require(seeded.returncode == 0 and 'SEEDED' in seeded.stdout,
            f'runtime seed rc={seeded.returncode} err={seeded.stderr.strip()[:300]}')
    STATE['pg'] = rt.postgres(root, binaries['pg_bin'], port)
    tls = STATE['tls']
    ca = str(tls / 'ca.pem')
    try:
        stage('identity startup')
        rt.identity_security_material(root)
        identity_config = json.loads((root / 'identity.json').read_text())
        identity_config['probe_listen'] = p14.free_endpoint()
        identity_config['grpc_listen'] = p14.free_endpoint()
        path = root / 'ws-identity.json'
        path.write_text(json.dumps(identity_config))
        path.chmod(0o600)
        identity, identity_state = p14.daemon(binaries['identity'], path)
        STATE['children'].append(identity)
        require(identity_state.get('ready') == 'true', 'identity readiness')
        require(rt.wait_ready(int(identity_state['probe_port']), True, 20.0),
                'identity probe readiness')
        identity_endpoint = identity_config['grpc_listen']
        STATE['identity_endpoint'] = identity_endpoint

        stage('runtime startup')
        endpoints = {'runtime_grpc': p14.free_endpoint(), 'runtime_probe': p14.free_endpoint(),
                     'runtime_bridge': p14.free_endpoint()}
        STATE['runtime_grpc'] = endpoints['runtime_grpc']
        STATE['runtime_bridge_port'] = int(endpoints['runtime_bridge'].rsplit(':', 1)[1])
        runtime_config = {
            'schema_version': 1, 'service': 'runtime',
            'probe_listen': endpoints['runtime_probe'],
            'grpc_listen': endpoints['runtime_grpc'],
            'bridge_listen': endpoints['runtime_bridge'],
            'tls': {'certificate_file': str(tls / 'runtime.pem'),
                    'private_key_file': str(tls / 'runtime.key'), 'ca_file': ca},
            'database': runtime_database, 'identity_endpoint': identity_endpoint,
            'resource_catalogs': STATE['catalogs'],
        }
        path = root / 'ws-runtime.json'
        path.write_text(json.dumps(runtime_config))
        path.chmod(0o600)
        runtime, runtime_state = p14.daemon(binaries['runtime'], path)
        STATE['children'].append(runtime)
        STATE['runtime_child'] = runtime
        require(rt.wait_ready(int(runtime_state['probe_port']), True, 20.0),
                'runtime probe readiness')

        stage('control startup')
        STATE['control_port'] = start_control(root, identity_endpoint,
                                              endpoints['runtime_grpc'])
        stage('websocket directory checks')
        asyncio.run(run_checks(root, identity_endpoint))
        print('P12 Control WS owner-directory real-process e2e passed', flush=True)
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
            message = f'P12 WS owner directory e2e ({STATE.get("stage", "bootstrap")}): ' + message
        print(message, file=sys.stderr)
        try:
            print('TRACEBACK: ' + traceback.format_exc()[-1500:], file=sys.stderr)
        except Exception:  # noqa: BLE001
            pass
        try:
            error_log = STATE.get('control_error_log')
            if error_log is not None and error_log.is_file():
                tail = error_log.read_text(errors='replace')[-800:]
                if tail.strip():
                    print('CONTROL STDERR TAIL: ' + tail.strip(), file=sys.stderr)
        except OSError:
            pass
        sys.exit(1)
