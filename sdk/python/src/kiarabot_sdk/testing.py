"""Target PluginBridge test double: TLS, bootstrap, fence/proof and delivery leases.

This fake is a protocol fixture for SDK tests, never a substitute for Core
authorization.  It implements the frozen P06 target behaviour closely enough to
exercise the SDK: server-verified TLS, per-instance bootstrap credential,
``ConnectionAuthorization`` + single-flight refresh, per-attempt delivery leases
(30 s, renew after 10 s, 300 s maximum), ACK/Renew receipts with dispositions,
config delivery binding a fence/config_version and graceful ``ShutdownNotice``.
"""

from __future__ import annotations

import asyncio
import os
import subprocess
import tempfile
import time
import uuid
from contextlib import suppress
from pathlib import Path

import grpc
from google.protobuf.timestamp_pb2 import Timestamp
from kiarabot.v1 import common_pb2 as common, events_pb2 as events, platform_pb2 as platform
from kiarabot.v1 import plugins_pb2 as plugins, services_pb2_grpc as services

from .errors import PluginError

TLS_ROOT: bytes | None = None
TLS_CERT: bytes | None = None
TLS_KEY: bytes | None = None


def generate_test_certificates(directory: Path) -> tuple[bytes, bytes]:
    """Create a short-lived self-signed server certificate for localhost."""
    directory.mkdir(parents=True, exist_ok=True)
    certificate, key = directory / 'server.crt', directory / 'server.key'
    subprocess.run([
        'openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
        '-keyout', str(key), '-out', str(certificate), '-days', '2',
        '-subj', '/CN=localhost',
        '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1',
    ], check=True, capture_output=True)
    return certificate.read_bytes(), key.read_bytes()


def _tls_material() -> tuple[bytes, bytes, bytes]:
    global TLS_ROOT, TLS_CERT, TLS_KEY
    if TLS_ROOT is None:
        with tempfile.TemporaryDirectory(prefix='kiarabot-sdk-tls-') as temporary:
            TLS_CERT, TLS_KEY = generate_test_certificates(Path(temporary))
        TLS_ROOT = TLS_CERT
    assert TLS_ROOT is not None and TLS_CERT is not None and TLS_KEY is not None
    return TLS_ROOT, TLS_CERT, TLS_KEY


def _timestamp(seconds: float) -> Timestamp:
    value = Timestamp()
    value.FromSeconds(int(seconds))
    value.nanos = int((seconds - int(seconds)) * 1_000_000_000)
    return value


class FakeTargetRuntime(services.PluginBridgeServicer):
    def __init__(self, token='t' * 32, *, grants=None, features=(), heartbeat_acks=True,
                 authorization_lifetime=56.0, refusal=None):
        root, certificate, key = _tls_material()
        self.root_cert = root
        self.token = token
        self.grants = grants
        self.features = list(features)
        self.heartbeat_acks = heartbeat_acks
        self.authorization_lifetime = authorization_lifetime
        self.refusal = refusal
        self.registrations = asyncio.Queue()
        self.acknowledgements = asyncio.Queue()
        self.config_acknowledgements = asyncio.Queue()
        self.receipts = asyncio.Queue()
        self.renewals = asyncio.Queue()
        self.refreshes = asyncio.Queue()
        self.heartbeats = asyncio.Queue()
        self.host_requests = asyncio.Queue()
        self.actions = asyncio.Queue()
        self.outgoing = asyncio.Queue()
        self.connection_generation = 0
        self.runtime_epoch = 'epoch-' + uuid.uuid4().hex[:8]
        self.execution_generation = 1
        self.authorization_version = 0
        self.refresh_generation = 0
        self.last_disposition = platform.DELIVERY_DISPOSITION_UNSPECIFIED
        self.attempts: dict[str, platform.DeliveryAttempt] = {}
        self.attempt_expired: set[str] = set()
        self.acked: set[str] = set()
        self._clock_offset = 0.0
        self.refresh_failure = None  # None | 'error_result' | 'abort'
        self.fence: platform.Fence | None = None
        self.proof = b''
        self.refresh_credential = b''
        self._connection_active = False
        self._server = grpc.aio.server()
        services.add_PluginBridgeServicer_to_server(self, self._server)
        credentials = grpc.ssl_server_credentials([(key, certificate)])
        port = self._server.add_secure_port('127.0.0.1:0', credentials)
        self.endpoint = f'127.0.0.1:{port}'

    async def __aenter__(self):
        await self._server.start()
        return self

    async def __aexit__(self, *_):
        await self._server.stop(0)

    # -- clock helpers -----------------------------------------------------
    def _now(self) -> float:
        return time.time() + self._clock_offset

    def advance(self, seconds: float) -> None:
        self._clock_offset += seconds

    def expire_attempt(self, event_id: str) -> None:
        self.attempt_expired.add(event_id)

    async def disconnect(self) -> None:
        await self.outgoing.put(None)

    # -- server frame helpers ---------------------------------------------
    def _authorization(self) -> platform.ConnectionAuthorization:
        assert self.fence is not None
        now = self._now()
        return platform.ConnectionAuthorization(
            fence=self.fence,
            proof=self.proof,
            issued_at=_timestamp(now),
            expires_at=_timestamp(now + self.authorization_lifetime),
            refresh_credential=self.refresh_credential,
            refresh_generation=self.refresh_generation,
            principal_id=self.fence.instance_id,
            credential_version=1,
            authorization_version=self.authorization_version)

    def _new_attempt(self, event_id: str) -> platform.DeliveryAttempt:
        assert self.fence is not None
        now = self._now()
        attempt = platform.DeliveryAttempt(
            event_id=event_id,
            attempt_id=str(uuid.uuid4()),
            fence=self.fence,
            started_at=_timestamp(now),
            lease_until=_timestamp(now + 30.0),
            attempt_number=1)
        self.attempts[event_id] = attempt
        self.attempt_expired.discard(event_id)
        return attempt

    def _receipt(self, attempt: platform.DeliveryAttempt,
                 disposition: int) -> platform.DeliveryReceipt:
        return platform.DeliveryReceipt(attempt=attempt, disposition=disposition)

    async def send_event(self, event: events.EventEnvelope) -> platform.DeliveryAttempt:
        attempt = self._new_attempt(event.event_id)
        await self.outgoing.put(plugins.CoreToPlugin(event=event, delivery_attempt=attempt))
        return attempt

    async def send_config(self, version: str, values=None, *, config_version=1,
                          delivery_id=None, apply_timeout_ms=5000) -> plugins.PluginConfig:
        assert self.fence is not None
        config = plugins.PluginConfig(version=version,
                                      delivery_id=delivery_id or 'delivery-' + version,
                                      apply_timeout_ms=apply_timeout_ms,
                                      config_version=config_version)
        config.values.update(values or {})
        config.fence.CopyFrom(self.fence)
        await self.outgoing.put(plugins.CoreToPlugin(config=config))
        return config

    async def send_shutdown(self, reason='shutdown', drain_timeout_ms=1000) -> None:
        await self.outgoing.put(plugins.CoreToPlugin(
            shutdown=common.ShutdownNotice(reason=reason, drain_timeout_ms=drain_timeout_ms)))

    async def send_frame(self, frame: plugins.CoreToPlugin) -> None:
        await self.outgoing.put(frame)

    # -- connection --------------------------------------------------------
    async def Connect(self, requests, context):
        metadata = dict(context.invocation_metadata())
        if metadata.get('authorization') != 'Bearer ' + self.token:
            await context.abort(grpc.StatusCode.UNAUTHENTICATED, 'UNAUTHENTICATED')
        try:
            first = await anext(requests)
        except (StopAsyncIteration, grpc.aio.EOF):
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, 'INVALID_ARGUMENT')
        if first.WhichOneof('body') != 'register':
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, 'INVALID_ARGUMENT')
        self.registrations.put_nowait(first.register)
        if self.refusal is not None:
            yield plugins.CoreToPlugin(register_result=plugins.RegisterPluginResponse(
                error=self.refusal))
            return
        registration = first.register
        self.connection_generation += 1
        self.authorization_version += 1
        self.refresh_generation = 1
        self.proof = os.urandom(32)
        self.refresh_credential = os.urandom(32)
        self.fence = platform.Fence(
            instance_id=registration.instance_id or 'instance',
            execution_generation=self.execution_generation,
            connection_generation=self.connection_generation,
            runtime_epoch=self.runtime_epoch)
        granted = list(registration.manifest.permissions) if self.grants is None else list(self.grants)
        yield plugins.CoreToPlugin(register_result=plugins.RegisterPluginResponse(
            accepted=True, required_api_version='kiarabot.plugin.v1',
            granted_permissions=granted, enabled_features=self.features,
            fence=self.fence, desired_config_version=0,
            authorization=self._authorization()))
        self._connection_active = True
        responses: asyncio.Queue = asyncio.Queue()

        async def read():
            async for request in requests:
                if not await self._validate_frame(request, context):
                    return
                body = request.WhichOneof('body')
                if body == 'heartbeat':
                    self.heartbeats.put_nowait(request.heartbeat)
                    if self.heartbeat_acks:
                        responses.put_nowait(plugins.CoreToPlugin(
                            heartbeat_ack=plugins.PluginHeartbeatAck(
                                received_at_unix_ms=int(self._now() * 1000))))
                elif body == 'event_ack':
                    await self._handle_ack(request.event_ack, responses)
                elif body == 'renew_delivery':
                    await self._handle_renew(request.renew_delivery, responses)
                elif body == 'config_ack':
                    self.config_acknowledgements.put_nowait(request.config_ack)
                elif body == 'refresh_authorization':
                    if not await self._handle_refresh(request.refresh_authorization,
                                                      responses, context):
                        return
                elif body == 'host_request':
                    self.host_requests.put_nowait(request.host_request)
                elif body in ('action', 'action_query', 'action_cancel'):
                    self.actions.put_nowait(request)

        async def forward():
            while True:
                frame = await self.outgoing.get()
                if frame is None:
                    await responses.put(None)
                    return
                await responses.put(frame)

        reader, sender = asyncio.create_task(read()), asyncio.create_task(forward())
        try:
            while True:
                frame = await responses.get()
                if frame is None:
                    return
                self._attach_envelope(frame)
                yield frame
        finally:
            self._connection_active = False
            for task in (reader, sender):
                task.cancel()
                with suppress(asyncio.CancelledError, Exception):
                    await task

    async def _validate_frame(self, request: plugins.PluginToCore, context) -> bool:
        if (not request.HasField('fence') or self.fence is None
                or request.fence != self.fence or request.connection_proof != self.proof):
            await context.abort(grpc.StatusCode.UNAUTHENTICATED, 'UNAUTHENTICATED')
            return False
        return True

    def _attach_envelope(self, frame: plugins.CoreToPlugin) -> None:
        if self.fence is not None:
            frame.fence.CopyFrom(self.fence)
        if frame.WhichOneof('body') == 'event' and not frame.HasField('delivery_attempt'):
            frame.delivery_attempt.CopyFrom(self.attempts.get(frame.event.event_id)
                                            or self._new_attempt(frame.event.event_id))
        if frame.WhichOneof('body') == 'config' and not frame.config.HasField('fence'):
            if self.fence is not None:
                frame.config.fence.CopyFrom(self.fence)

    async def _handle_ack(self, ack: plugins.PluginEventAck,
                          responses: asyncio.Queue) -> None:
        if not ack.HasField('attempt') or ack.attempt.event_id != ack.event_id:
            responses.put_nowait(plugins.CoreToPlugin(error=common.Error(
                code=common.INVALID_ARGUMENT, message='INVALID_ARGUMENT')))
            return
        current = self.attempts.get(ack.event_id)
        if (current is None or ack.attempt.attempt_id != current.attempt_id
                or ack.event_id in self.attempt_expired):
            disposition = platform.DELIVERY_STALE
        elif ack.event_id in self.acked:
            disposition = platform.DELIVERY_DUPLICATE
        else:
            self.acked.add(ack.event_id)
            disposition = (platform.DELIVERY_ACKNOWLEDGED if ack.accepted
                           else platform.DELIVERY_REJECTED)
        self.last_disposition = disposition
        self.acknowledgements.put_nowait(ack)
        responses.put_nowait(plugins.CoreToPlugin(
            delivery_receipt=self._receipt(ack.attempt, disposition)))

    async def _handle_renew(self, attempt: platform.DeliveryAttempt,
                            responses: asyncio.Queue) -> None:
        current = self.attempts.get(attempt.event_id)
        if (current is not None and current.attempt_id == attempt.attempt_id
                and attempt.event_id not in self.attempt_expired):
            started = current.started_at.seconds
            now = self._now()
            current.lease_until.CopyFrom(_timestamp(min(now + 30.0, started + 300.0)))
            disposition = platform.DELIVERY_RENEWED
        else:
            disposition = platform.DELIVERY_STALE
        self.last_disposition = disposition
        self.renewals.put_nowait((attempt, disposition))
        responses.put_nowait(plugins.CoreToPlugin(
            delivery_receipt=self._receipt(attempt, disposition)))

    async def _handle_refresh(self, request: platform.RefreshConnectionAuthorization,
                              responses: asyncio.Queue, context) -> bool:
        self.refreshes.put_nowait(request)
        if self.refresh_failure == 'abort':
            await context.abort(grpc.StatusCode.UNAUTHENTICATED, 'UNAUTHENTICATED')
            return False
        if self.refresh_failure == 'error_result':
            responses.put_nowait(plugins.CoreToPlugin(
                authorization_result=platform.ConnectionAuthorizationResult(
                    request_id=request.request_id,
                    error=common.Error(code=common.UNAUTHENTICATED,
                                       message='UNAUTHENTICATED'))))
            return True
        # Reject old/rotated material: proof, refresh and generation must be current.
        if (self.fence is None or request.fence != self.fence
                or request.proof != self.proof
                or request.refresh_credential != self.refresh_credential
                or request.refresh_generation != self.refresh_generation):
            await context.abort(grpc.StatusCode.UNAUTHENTICATED, 'UNAUTHENTICATED')
            return False
        self.refresh_generation += 1
        self.authorization_version += 1
        self.proof = os.urandom(32)
        self.refresh_credential = os.urandom(32)
        responses.put_nowait(plugins.CoreToPlugin(
            authorization_result=platform.ConnectionAuthorizationResult(
                request_id=request.request_id, authorization=self._authorization())))
        return True
