"""Loopback target Runtime bridge fixture with the frozen P06 wire semantics.

The fixture is a real gRPC server over server-verified TLS (disposable test certificate). It
validates the bootstrap metadata, the registration frame, the mandatory per-frame ``Fence`` and
``connection_proof``, rotates connection authorizations, and lets tests inject ack/refresh/close
faults through plain callables. It is a protocol fixture, never a substitute for the P06a domain
and PostgreSQL implementation.
"""
from __future__ import annotations

import asyncio
import hashlib
import re
import time
import uuid
from dataclasses import dataclass, field

import grpc
from kiarabot.v1 import adapters_pb2 as adapters, common_pb2 as common, platform_pb2 as platform
from kiarabot.v1 import services_pb2_grpc as services
from kiarabot_adapter_nonebot.bridge import (PROTOCOL_VERSION, BridgeConfig, Fence, timestamp)

UUID4 = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z')
ADAPTER_EVENT_CAPABILITY = 'event.message_received'


class _Close:
    pass


@dataclass
class _Abort:
    code: grpc.StatusCode


_CLOSE = _Close()


def _digest(label: str, generation: int) -> bytes:
    return hashlib.sha256(f'{label}:{generation}'.encode()).digest()


@dataclass
class BootstrapEntry:
    credential: str
    instance_id: str
    execution_generation: int = 1
    revoked: bool = False
    connection_generation: int = 0
    accepted_events: set = field(default_factory=set)


@dataclass
class Connection:
    entry: BootstrapEntry
    connection_generation: int
    fence: Fence
    proof: bytes
    refresh_credential: bytes
    refresh_generation: int
    authorization_expires: float
    registration: object
    heartbeats: list = field(default_factory=list)
    events: list = field(default_factory=list)
    refreshes: list = field(default_factory=list)
    action_results: list = field(default_factory=list)
    closed: bool = False


class FakeTargetRuntime(services.AdapterBridgeServicer):
    """Real gRPC target bridge with injectable clock-free faults (coroutine-safe loopback)."""

    def __init__(self, certificate: str, private_key: str, *, now=time.time,
                 heartbeat_interval: int = 5, authorization_lifetime: float = 56.0,
                 runtime_epoch: str = 'runtime-epoch-fixture', io_timeout: float = 5.0):
        if not 1 <= heartbeat_interval <= 300:
            raise ValueError('invalid heartbeat interval')
        if not 0.2 <= authorization_lifetime <= 60:
            raise ValueError('invalid authorization lifetime')
        self._now, self._io_timeout = now, io_timeout
        self.heartbeat_interval = heartbeat_interval
        self.authorization_lifetime = authorization_lifetime
        self.runtime_epoch = runtime_epoch
        self.entries: dict[str, BootstrapEntry] = {}
        self.connections: list[Connection] = []
        self.auth_failures: list[grpc.StatusCode] = []
        self.protocol_violations: list[str] = []
        self.connect_policy = None       # callable(entry) -> None | grpc.StatusCode
        self.registration_policy = None  # callable(registration) -> None | ('reject', code) | code
        self.ack_policy = None           # callable(connection, event, index) -> None/dict/'hold'/'close'
        self.refresh_policy = None       # callable(connection, request) -> 'ok'/('error',code)/'drop'/('abort',code)
        self.effective_capability_filter = None  # callable(iterable[str]) -> iterable[str]
        self.mutate_ack_fence = False
        self.effective_capabilities = None
        self._server = grpc.aio.server()
        services.add_AdapterBridgeServicer_to_server(self, self._server)
        with open(certificate, 'rb') as certificate_file:
            certificate_bytes = certificate_file.read()
        with open(private_key, 'rb') as key_file:
            key_bytes = key_file.read()
        self.certificate = certificate
        self._port = self._server.add_secure_port(
            '127.0.0.1:0', grpc.ssl_server_credentials([(key_bytes, certificate_bytes)]))
        self.endpoint = f'127.0.0.1:{self._port}'
        self._active: Connection | None = None
        self._outbox: asyncio.Queue | None = None

    async def __aenter__(self):
        await self._server.start()
        return self

    async def __aexit__(self, *_):
        await self.stop()

    async def stop(self):
        await self._server.stop(0)

    # ---- test setup helpers ------------------------------------------------------------
    @staticmethod
    def credential(label: str = 'adapter') -> str:
        return hashlib.sha256(f'credential:{label}:{uuid.uuid4()}'.encode()).hexdigest()

    def add_bootstrap(self, credential: str, instance_id: str | None = None, **kwargs) -> BootstrapEntry:
        entry = BootstrapEntry(credential, instance_id or str(uuid.uuid4()), **kwargs)
        self.entries[credential] = entry
        return entry

    def revoke(self, entry: BootstrapEntry) -> None:
        entry.revoked = True

    @property
    def active(self) -> Connection | None:
        return self._active

    # ---- server -> client pushes -------------------------------------------------------
    def _frame(self, connection: Connection, **body):
        return adapters.CoreToAdapter(fence=connection.fence.to_proto(), **body)

    async def _put(self, item) -> None:
        if self._outbox is None:
            raise AssertionError('no active connection')
        await self._outbox.put(item)

    async def push_action(self, action) -> None:
        await self._put(self._frame(self._active, action=action))

    async def push_shutdown(self, *, reason: str = 'drain', drain_timeout_ms: int = 0) -> None:
        await self._put(self._frame(self._active, shutdown=common.ShutdownNotice(
            reason=reason, drain_timeout_ms=drain_timeout_ms)))

    async def push_empty_frame(self) -> None:
        await self._put(self._frame(self._active))

    async def close_stream(self) -> None:
        await self._put(_CLOSE)

    async def abort_stream(self, code=grpc.StatusCode.UNAVAILABLE) -> None:
        await self._put(_Abort(code))

    # ---- servicer ----------------------------------------------------------------------
    def _credential(self, context) -> BootstrapEntry | None:
        values = [value for key, value in context.invocation_metadata() if key == 'authorization']
        if len(values) != 1 or not isinstance(values[0], str) or not values[0].startswith('Bearer '):
            return None
        token = values[0][len('Bearer '):]
        return self.entries.get(token)

    async def _read_first(self, requests):
        try:
            return await asyncio.wait_for(requests.__anext__(), self._io_timeout)
        except (StopAsyncIteration, asyncio.TimeoutError):
            return None

    def _registration_valid(self, registration) -> str | None:
        """Return a stable violation label, never free-form payload text."""
        context = registration.context
        if (registration.WhichOneof is None
                or not UUID4.fullmatch(context.request_id) or not UUID4.fullmatch(context.trace_id)
                or not context.HasField('deadline') or context.deadline.seconds <= 0
                or not registration.adapter_id or len(registration.adapter_id) > 128
                or not registration.accounts or len(registration.accounts) > 64):
            return 'invalid_registration'
        for account in registration.accounts:
            if not account.account_id or not account.enabled:
                return 'invalid_account'
        names = [item.name for item in registration.capabilities]
        if len(names) != len(set(names)) or ADAPTER_EVENT_CAPABILITY not in names:
            return 'invalid_capability'
        return None

    async def Connect(self, requests, context):  # noqa: N802 - grpc servicer name
        entry = self._credential(context)
        if entry is None:
            self.auth_failures.append(grpc.StatusCode.UNAUTHENTICATED)
            await context.abort(grpc.StatusCode.UNAUTHENTICATED, 'UNAUTHENTICATED')
        if entry.revoked:
            self.auth_failures.append(grpc.StatusCode.PERMISSION_DENIED)
            await context.abort(grpc.StatusCode.PERMISSION_DENIED, 'PERMISSION_DENIED')
        refusal = self.connect_policy(entry) if self.connect_policy else None
        if refusal is not None:
            self.auth_failures.append(refusal)
            await context.abort(refusal, refusal.name)
        first = await self._read_first(requests)
        if first is None or first.WhichOneof('body') != 'register':
            self.protocol_violations.append('first_frame_not_register')
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, 'INVALID_ARGUMENT')
        registration = first.register
        violation = self._registration_valid(registration)
        if violation is not None:
            self.protocol_violations.append(violation)
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, 'INVALID_ARGUMENT')
        if registration.adapter_instance_id != entry.instance_id:
            self.protocol_violations.append('bootstrap_resource_mismatch')
            await context.abort(grpc.StatusCode.PERMISSION_DENIED, 'PERMISSION_DENIED')
        policy = self.registration_policy(registration) if self.registration_policy else None
        if isinstance(policy, tuple) and policy[0] == 'reject':
            yield adapters.CoreToAdapter(register_result=adapters.RegisterAdapterResponse(
                accepted=False, error=common.Error(code=policy[1],
                                                  message=common.ErrorCode.Name(policy[1]))))
            return
        if isinstance(policy, grpc.StatusCode):
            await context.abort(policy, policy.name)
        entry.connection_generation += 1
        generation = entry.connection_generation
        fence = Fence(entry.instance_id, entry.execution_generation, generation,
                      self.runtime_epoch)
        now = self._now()
        connection = Connection(entry, generation, fence, _digest('proof', generation),
                                _digest('refresh', generation), 1,
                                now + self.authorization_lifetime, registration)
        self.connections.append(connection)
        self._active = connection
        self._outbox = asyncio.Queue()
        effective = tuple(item.name for item in registration.capabilities)
        if self.effective_capability_filter is not None:
            effective = tuple(self.effective_capability_filter(effective))
        self.effective_capabilities = effective
        response = adapters.RegisterAdapterResponse(
            accepted=True, assigned_instance_id=entry.instance_id,
            required_protocol_version=PROTOCOL_VERSION,
            heartbeat_interval_seconds=self.heartbeat_interval, fence=fence.to_proto(),
            authorization=self._authorization(connection),
            effective_capabilities=[common.Capability(name=name, version='1', supported=True)
                                    for name in effective])
        reader = asyncio.create_task(self._read_loop(requests, connection))
        try:
            yield adapters.CoreToAdapter(register_result=response)
            while True:
                item = await self._outbox.get()
                if item is _CLOSE:
                    return
                if isinstance(item, _Abort):
                    await context.abort(item.code, item.code.name)
                yield item
        finally:
            connection.closed = True
            if self._active is connection:
                self._active = None
                self._outbox = None
            reader.cancel()
            try:
                await reader
            except asyncio.CancelledError:
                pass

    def _authorization(self, connection: Connection):
        now = self._now()
        connection.authorization_expires = now + self.authorization_lifetime
        return platform.ConnectionAuthorization(
            fence=connection.fence.to_proto(), proof=connection.proof,
            issued_at=timestamp(now), expires_at=timestamp(connection.authorization_expires),
            refresh_credential=connection.refresh_credential,
            refresh_generation=connection.refresh_generation,
            principal_id=connection.entry.instance_id, credential_version=1,
            authorization_version=1)

    async def _fence_ok(self, frame, connection: Connection) -> bool:
        if not frame.HasField('fence') or frame.connection_proof != connection.proof:
            self.protocol_violations.append('missing_or_stale_fence')
            await self._put(_Abort(grpc.StatusCode.UNAUTHENTICATED))
            return False
        if Fence.from_proto(frame.fence) != connection.fence:
            self.protocol_violations.append('fence_mismatch')
            await self._put(_Abort(grpc.StatusCode.UNAUTHENTICATED))
            return False
        return True

    async def _read_loop(self, requests, connection: Connection) -> None:
        try:
            async for frame in requests:
                if not await self._fence_ok(frame, connection):
                    return
                body = frame.WhichOneof('body')
                if body == 'event':
                    if not await self._handle_event(connection, frame.event):
                        return
                elif body == 'heartbeat':
                    connection.heartbeats.append(frame.heartbeat)
                    await self._put(self._frame(connection, heartbeat_ack=adapters.HeartbeatAck(
                        received_at_unix_ms=int(self._now() * 1000))))
                elif body == 'refresh_authorization':
                    if not await self._handle_refresh(connection, frame.refresh_authorization):
                        return
                elif body == 'action_result':
                    connection.action_results.append(frame.action_result)
                else:
                    self.protocol_violations.append('unexpected_client_body')
                    await self._put(_Abort(grpc.StatusCode.INVALID_ARGUMENT))
                    return
        except Exception:
            pass
        finally:
            outbox = self._outbox
            if outbox is not None:
                await outbox.put(_CLOSE)

    async def _handle_event(self, connection: Connection, event) -> bool:
        if (not event.event_id or not event.trace_id
                or event.source.adapter_id != connection.registration.adapter_id
                or event.HasField('adapter_status_changed') and
                event.adapter_status_changed.adapter_id != connection.registration.adapter_id):
            self.protocol_violations.append('invalid_event')
            await self._put(_Abort(grpc.StatusCode.INVALID_ARGUMENT))
            return False
        connection.events.append(event)
        index = len(connection.events)
        action = self.ack_policy(connection, event, index) if self.ack_policy else None
        if action == 'hold':
            return True
        if action == 'close' or action == 'reject_close':
            ack = adapters.AdapterEventAck(event_id=event.event_id, trace_id=event.trace_id,
                                           status=adapters.AdapterEventAck.PERSISTED)
            await self._put(self._frame(connection, event_ack=ack))
            await self._put(_CLOSE)
            return False
        if isinstance(action, dict) and action.get('status') is not None:
            ack = adapters.AdapterEventAck(event_id=event.event_id, trace_id=event.trace_id,
                                           status=action['status'])
            if action.get('code'):
                ack.error.code = action['code']
                ack.error.retryable = bool(action.get('retryable'))
                ack.error.message = common.ErrorCode.Name(action['code'])
            ack.retry_after_ms = int(action.get('retry_after_ms', 0))
            if action.get('wrong_event_id'):
                ack.event_id = 'not-the-event'
            await self._put(self._ack_frame(connection, ack))
            if action.get('after_ack') == 'close':
                await self._put(_CLOSE)
                return False
            if action.get('after_ack') == 'abort':
                await self._put(_Abort(grpc.StatusCode.UNAVAILABLE))
                return False
            return True
        # Persistence fixture: first successful event is PERSISTED, redelivery is DUPLICATE.
        status = (adapters.AdapterEventAck.DUPLICATE
                  if event.event_id in connection.entry.accepted_events
                  else adapters.AdapterEventAck.PERSISTED)
        connection.entry.accepted_events.add(event.event_id)
        await self._put(self._ack_frame(connection, adapters.AdapterEventAck(
            event_id=event.event_id, trace_id=event.trace_id, status=status)))
        return True

    def _ack_frame(self, connection: Connection, ack):
        frame = self._frame(connection, event_ack=ack)
        if self.mutate_ack_fence:
            self.mutate_ack_fence = False
            frame.fence.CopyFrom(platform.Fence(
                instance_id=connection.fence.instance_id,
                execution_generation=connection.fence.execution_generation,
                connection_generation=connection.fence.connection_generation + 1,
                runtime_epoch=connection.fence.runtime_epoch))
        return frame

    async def _handle_refresh(self, connection: Connection, request) -> bool:
        connection.refreshes.append(request)
        action = self.refresh_policy(connection, request) if self.refresh_policy else 'ok'
        if action == 'drop':
            return True
        if action == 'abort' or (isinstance(action, tuple) and action[0] == 'abort'):
            code = action[1] if isinstance(action, tuple) else grpc.StatusCode.UNAUTHENTICATED
            await self._put(_Abort(code))
            return False
        if isinstance(action, tuple) and action[0] == 'error':
            await self._put(self._frame(connection, authorization_result=(
                platform.ConnectionAuthorizationResult(
                    request_id=request.request_id,
                    error=common.Error(code=action[1], message=common.ErrorCode.Name(action[1]),
                                       retryable=True)))))
            return True
        if (Fence.from_proto(request.fence) != connection.fence
                or request.proof != connection.proof
                or request.refresh_credential != connection.refresh_credential
                or request.refresh_generation != connection.refresh_generation):
            self.protocol_violations.append('invalid_refresh_material')
            await self._put(_Abort(grpc.StatusCode.UNAUTHENTICATED))
            return False
        connection.refresh_generation += 1
        connection.proof = _digest('proof', connection.connection_generation * 1000
                                   + connection.refresh_generation)
        connection.refresh_credential = _digest('refresh', connection.connection_generation * 1000
                                                + connection.refresh_generation)
        authorization = self._authorization(connection)
        await self._put(self._frame(connection, authorization_result=(
            platform.ConnectionAuthorizationResult(request_id=request.request_id,
                                                  authorization=authorization))))
        return True


async def until(predicate, timeout=5.0, interval=0.005):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(interval)


def make_bridge_config(tmp_path, runtime: FakeTargetRuntime, entry: BootstrapEntry, *,
                       account_id='123', server_name=None, **kwargs):
    """Deployment-shaped BridgeConfig: CA trust file plus an owner-only bootstrap secret file."""
    from tls_fixture import write_bootstrap_secret, write_trust_file
    trust = write_trust_file(tmp_path, runtime.certificate)
    secret = write_bootstrap_secret(tmp_path, entry.credential)
    return BridgeConfig(endpoint=runtime.endpoint, ca_file=str(trust),
                        bootstrap_file=str(secret), adapter_instance_id=entry.instance_id,
                        account_id=account_id, server_name=server_name, **kwargs)
