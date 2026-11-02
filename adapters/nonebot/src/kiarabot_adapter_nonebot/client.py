"""Target Runtime bridge client for the NoneBot adapter.

The target contract (docs 01/04, frozen 2026-11-02) replaces the reference unary bridge with a
single bidirectional ``AdapterBridge.Connect`` stream over server-verified TLS: the adapter
authenticates the Runtime certificate against the deployment CA, registers with a one-time
bootstrap credential supplied as gRPC metadata, receives a ``Fence`` plus a short-lived
``ConnectionAuthorization``, and then fences every frame with the current fence and opaque
connection proof. The reference insecure loopback/token transport and its compatibility switch
are intentionally gone.
"""
from __future__ import annotations

import asyncio
import logging
import random
import time
from contextlib import suppress
from dataclasses import dataclass

import grpc
from kiarabot.v1 import adapters_pb2 as adapters, common_pb2 as common, services_pb2_grpc as services

from .actions import ACTION_CAPABILITIES
from .bridge import (BridgeConfig, BridgeFailure, ConnectionAuthorization, FATAL_REGISTRATION_CODES,
                     Registration, authorization_result, check_response_fence, error_code, frame,
                     shutdown_notice)
from .mapper import AdapterError
from .outbox import Outbox

logger = logging.getLogger(__name__)

EVENT_CAPABILITY = 'event.message_received'
# Rejections that must keep the event pending even if the Runtime omits retryable=true.
ALWAYS_RETRYABLE_REJECTIONS = frozenset({common.RATE_LIMITED})


@dataclass
class RetryConnection(Exception):
    code: int = common.UPSTREAM_UNAVAILABLE
    delay: float = 0.0


def _grpc_failure(error: grpc.aio.AioRpcError) -> BridgeFailure:
    """Map stream transport status: authentication/permission are terminal, rest is retryable."""
    status = error.code()
    if status in (grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED):
        return BridgeFailure(common.UNAUTHENTICATED if status == grpc.StatusCode.UNAUTHENTICATED
                             else common.PERMISSION_DENIED, fatal=True)
    if status == grpc.StatusCode.INVALID_ARGUMENT:
        return BridgeFailure(common.INVALID_ARGUMENT, fatal=True)
    if status == grpc.StatusCode.DEADLINE_EXCEEDED:
        return BridgeFailure(common.TIMEOUT)
    return BridgeFailure(common.UPSTREAM_UNAVAILABLE)


class BridgeClient:
    """Single event-loop owner of the target bridge connection and durable event outbox."""

    def __init__(self, config: BridgeConfig, outbox: Outbox, executor=None, status=None,
                 on_fatal=None, now=time.time, monotonic=time.monotonic, randomizer=random.uniform):
        self.config, self.outbox = config, outbox
        self.executor, self.status = executor, status
        self._on_fatal = on_fatal
        self._now, self._monotonic, self._random = now, monotonic, randomizer
        self.fatal = 0
        self.shutdown_seen = False
        self.heartbeat_interval = 5.0
        self._authorization: ConnectionAuthorization | None = None
        self._fence: Fence | None = None
        self._instance: str | None = None
        self._effective_capabilities: frozenset = frozenset()
        self._connected = False
        self._draining = False
        self._refresh_at = float('inf')
        self._refresh_pending: str | None = None
        self._refresh_deadline = float('inf')
        self._drain_deadline = float('inf')
        self._deferred_action_results = []
        self._last_heartbeat = 0.0

    # ---- state exposed to health/status -------------------------------------------------
    @property
    def authorization(self):
        return self._authorization

    @property
    def instance(self):
        return self._instance

    @property
    def effective_capabilities(self):
        return self._effective_capabilities

    @property
    def authorized(self) -> bool:
        """Current authorization (fence + unexpired proof); cleared on every disconnect."""
        return (self._authorization is not None and self._instance is not None
                and self._authorization.valid(self._now()))

    def report_status(self, state: int, reason: str = '') -> None:
        if self.status is None:
            return
        try:
            self.status.report(state, reason)
        except AdapterError as failure:
            logger.warning('operation=status_report error_code=%s', common.ErrorCode.Name(failure.code))

    def declared_capabilities(self):
        capabilities = [EVENT_CAPABILITY]
        if self.executor:
            capabilities.extend(ACTION_CAPABILITIES)
        return tuple(capabilities)

    def registration_request(self):
        return self.config.registration_request(self.declared_capabilities())

    # ---- transport helpers -------------------------------------------------------------
    async def _write(self, call, request) -> None:
        try:
            await asyncio.wait_for(call.write(request), timeout=self.config.timeout)
        except asyncio.TimeoutError:
            raise RetryConnection(common.TIMEOUT) from None
        except grpc.aio.AioRpcError as error:
            raise _grpc_failure(error) from None
        except (ConnectionError, OSError):
            raise RetryConnection(common.UPSTREAM_UNAVAILABLE) from None

    async def _pump(self, call, inbox) -> None:
        try:
            while True:
                response = await call.read()
                if response is grpc.aio.EOF:
                    await inbox.put(None)
                    return
                await inbox.put(response)
        except asyncio.CancelledError:
            raise
        except grpc.aio.AioRpcError as error:
            await inbox.put(error)
        except Exception:
            await inbox.put(None)

    async def _read(self, inbox, timeout=None):
        try:
            response = await asyncio.wait_for(inbox.get(), timeout or self.config.timeout)
        except asyncio.TimeoutError:
            raise RetryConnection(common.TIMEOUT) from None
        if response is None:
            raise RetryConnection(common.UPSTREAM_UNAVAILABLE)
        if isinstance(response, grpc.aio.AioRpcError):
            raise _grpc_failure(response)
        return response

    def _frame(self, **body):
        assert self._authorization is not None and self._fence is not None
        return frame(self._fence, self._authorization.proof, **body)

    # ---- registration and authorization lifecycle --------------------------------------
    def _apply_registration(self, registration: Registration) -> None:
        if EVENT_CAPABILITY not in registration.effective_capabilities:
            raise BridgeFailure(common.CAPABILITY_NOT_SUPPORTED, fatal=True)
        self._authorization = registration.authorization
        self._fence = registration.fence
        self._instance = registration.instance_id
        self._effective_capabilities = registration.effective_capabilities
        self.heartbeat_interval = registration.heartbeat_interval
        self._draining = False
        self._drain_deadline = float('inf')
        self._refresh_pending = None
        self._schedule_refresh(registration.authorization)
        if self.executor is not None:
            self.executor.configure(registration.instance_id, registration.effective_capabilities)

    def _schedule_refresh(self, authorization: ConnectionAuthorization) -> None:
        remaining = authorization.expires_at - self._now()
        self._refresh_at = self._monotonic() + max(0.0, remaining - authorization.refresh_lead)
        self._refresh_deadline = float('inf')
        self._refresh_pending = None

    async def _start_refresh(self, call) -> None:
        assert self._authorization is not None
        request = self._authorization.refresh_request()
        await self._write(call, self._frame(refresh_authorization=request))
        self._refresh_pending = request.request_id
        self._refresh_deadline = self._monotonic() + self.config.timeout

    async def _apply_refresh(self, call, request_id: str, authorization) -> None:
        if request_id != self._refresh_pending or self._authorization is None:
            raise RetryConnection(common.INTERNAL)
        if self._fence is None or not authorization.HasField('fence'):
            raise BridgeFailure(common.UNAUTHENTICATED, fatal=True)
        refreshed = ConnectionAuthorization.from_proto(
            authorization, expected_fence=self._fence)
        if (refreshed.principal_id != self._authorization.principal_id
                or refreshed.credential_version != self._authorization.credential_version
                or refreshed.authorization_version != self._authorization.authorization_version
                or refreshed.refresh_generation <= self._authorization.refresh_generation
                or refreshed.proof == self._authorization.proof
                or not refreshed.valid(self._now())):
            raise BridgeFailure(common.UNAUTHENTICATED, fatal=True)
        self._authorization = refreshed
        self._schedule_refresh(refreshed)
        # Frames generated while the rotation was in flight must use the new proof: the Runtime
        # invalidates the old material atomically when it accepts the refresh request.
        deferred, self._deferred_action_results = self._deferred_action_results, []
        for result in deferred:
            await self._write(call, self._frame(action_result=result))

    def _refresh_failure(self, request_id: str, code: int) -> None:
        if request_id != self._refresh_pending:
            raise RetryConnection(common.INTERNAL)
        # No old-proof fallback: an unknown or lost rotation always reconnects with the still
        # valid bootstrap credential and receives a new connection_generation.
        self._refresh_pending = None
        if code in (common.UNAUTHENTICATED, common.PERMISSION_DENIED):
            raise BridgeFailure(code, fatal=True)
        raise RetryConnection(code)

    # ---- frame processing --------------------------------------------------------------
    async def _process(self, call, response, *, in_flight: bool) -> None:
        body = response.WhichOneof('body')
        if body == 'shutdown':
            check_response_fence(response, self._fence)
            self.shutdown_seen = True
            drain_ms = shutdown_notice(response)
            self._draining = True
            self._drain_deadline = self._monotonic() + drain_ms / 1000.0
            if not in_flight:
                raise RetryConnection(common.UPSTREAM_UNAVAILABLE, delay=drain_ms / 1000.0)
            return
        if body in ('event_ack', 'heartbeat_ack', 'action', 'action_receipt'):
            check_response_fence(response, self._fence)
        if body == 'heartbeat_ack':
            self._last_heartbeat = self._monotonic()
            return
        if body == 'action':
            if not self.executor:
                raise BridgeFailure(common.CAPABILITY_NOT_SUPPORTED)
            result = await self.executor.execute(response.action, self._fence.instance_id)
            if self._refresh_pending is not None:
                self._deferred_action_results.append(result)
            else:
                await self._write(call, self._frame(action_result=result))
            return
        if body == 'action_receipt':
            # P07 export confirmation is not consumed in the P06 adapter phase.
            return
        if body == 'authorization_result':
            request_id, authorization, code = authorization_result(response)
            if code is not None:
                self._refresh_failure(request_id, code)
                return
            await self._apply_refresh(call, request_id, authorization)
            return
        raise RetryConnection(common.INTERNAL)

    def handle_event_ack(self, event, ack) -> None:
        if ack.status in (adapters.AdapterEventAck.PERSISTED, adapters.AdapterEventAck.DUPLICATE):
            if ack.HasField('error'):
                raise RetryConnection(common.INTERNAL)
            self.outbox.finish(event.event_id)
            return
        if ack.status == adapters.AdapterEventAck.REJECTED and ack.error.code:
            if ack.error.code not in common.ErrorCode.values():
                raise RetryConnection(common.INTERNAL)
            # Target backpressure is retryable and closes the stream; the adapter keeps the
            # unconfirmed event and resends it after re-registering. Never drop it.
            retryable = ack.error.retryable or ack.error.code in ALWAYS_RETRYABLE_REJECTIONS
            if retryable:
                delay = min(30.0, ack.retry_after_ms / 1000) if ack.retry_after_ms else 0.0
                raise RetryConnection(ack.error.code, delay)
            self.outbox.finish(event.event_id, ack.error.code)
            return
        raise RetryConnection(common.INTERNAL)

    async def _send_event(self, call, inbox, event) -> None:
        assert self._fence is not None
        event.source.adapter_instance_id = self._fence.instance_id
        await self._write(call, self._frame(event=event))
        while True:
            response = await self._read(inbox)
            body = response.WhichOneof('body')
            if body == 'event_ack':
                check_response_fence(response, self._fence)
                ack = response.event_ack
                if ack.event_id != event.event_id or ack.trace_id != event.trace_id:
                    raise RetryConnection(common.INTERNAL)
                if self._draining:
                    self.handle_event_ack(event, ack)
                    raise RetryConnection(common.UPSTREAM_UNAVAILABLE)
                self.handle_event_ack(event, ack)
                return
            await self._process(call, response, in_flight=True)
            # Shutdown noticed while an event is unconfirmed: finish this confirmation inside
            # the advertised drain window, then reconnect with the event still pending.
            if self._draining and self._monotonic() >= self._drain_deadline:
                raise RetryConnection(common.UPSTREAM_UNAVAILABLE)

    async def _loop(self, call, inbox, stop) -> None:
        self._last_heartbeat = self._monotonic()
        while not stop.is_set():
            if self._draining:
                raise RetryConnection(common.UPSTREAM_UNAVAILABLE)
            now_wall, now_mono = self._now(), self._monotonic()
            if self._authorization is None or not self._authorization.valid(now_wall):
                raise RetryConnection(common.UNAUTHENTICATED)
            if self._refresh_pending is not None and now_mono > self._refresh_deadline:
                raise RetryConnection(common.TIMEOUT)
            if self._refresh_pending is None and now_mono >= self._refresh_at:
                await self._start_refresh(call)
                continue
            if self._refresh_pending is not None:
                # Single-flight rotation: the Runtime invalidates the old proof when it accepts
                # the refresh request, so no other frame uses the old material until the current
                # authorization result arrives.
                timeout = max(0.005, self._refresh_deadline - now_mono)
                try:
                    response = await self._read(inbox, min(self.config.idle_interval, timeout))
                except RetryConnection as failure:
                    if failure.code == common.TIMEOUT:
                        continue
                    raise
                await self._process(call, response, in_flight=False)
                continue
            event = self.outbox.next_pending()
            if event is not None:
                await self._send_event(call, inbox, event)
                continue
            if now_mono - self._last_heartbeat >= self.heartbeat_interval:
                await self._write(call, self._frame(heartbeat=adapters.Heartbeat(
                    instance_id=self._fence.instance_id,
                    sent_at_unix_ms=int(self._now() * 1000))))
                self._last_heartbeat = now_mono
                continue
            timeout = self.config.idle_interval
            if self._refresh_pending is None:
                timeout = min(timeout, max(0.005, self._refresh_at - now_mono))
            try:
                response = await self._read(inbox, timeout)
            except RetryConnection as failure:
                if failure.code == common.TIMEOUT:
                    continue
                raise
            await self._process(call, response, in_flight=False)

    async def connection(self, stop: asyncio.Event) -> None:
        """One bootstrap + registration + framed session; failure returns to ``run`` backoff."""
        channel = grpc.aio.secure_channel(self.config.endpoint, self.config.tls_credentials(),
                                          options=self.config.channel_options())
        async with channel:
            call = services.AdapterBridgeStub(channel).Connect(metadata=self.config.metadata())
            inbox: asyncio.Queue = asyncio.Queue()
            reader = asyncio.create_task(self._pump(call, inbox))
            try:
                await self._write(call, adapters.AdapterToCore(register=self.registration_request()))
                response = await self._read(inbox)
                if response.WhichOneof('body') != 'register_result':
                    raise RetryConnection(common.INTERNAL)
                result = response.register_result
                if not result.accepted:
                    code = error_code(result.error) if result.HasField('error') else common.INTERNAL
                    raise BridgeFailure(code, fatal=code in FATAL_REGISTRATION_CODES)
                self._apply_registration(Registration.from_response(
                    result, expected_instance=self.config.adapter_instance_id))
                self._connected = True
                self.report_status(common.HEALTHY, 'runtime_connected')
                await self._loop(call, inbox, stop)
            finally:
                self._connected = False
                self._authorization = None
                self._fence = None
                self._instance = None
                self._effective_capabilities = frozenset()
                self._refresh_at = float('inf')
                self._refresh_pending = None
                reader.cancel()
                with suppress(asyncio.CancelledError):
                    await reader
                call.cancel()
                if not self.shutdown_seen:
                    self.report_status(common.DEGRADED, 'runtime_disconnected')

    async def run(self, stop: asyncio.Event) -> None:
        failures = 0
        while not stop.is_set():
            delay = 0.0
            try:
                await self.connection(stop)
                failures = 0
            except asyncio.CancelledError:
                raise
            except BridgeFailure as failure:
                if failure.fatal:
                    self.fatal = failure.code
                    logger.warning('operation=bridge_fatal error_code=%s',
                                   common.ErrorCode.Name(failure.code))
                    stop.set()
                    if self._on_fatal is not None:
                        self._on_fatal()
                    return
                logger.warning('operation=bridge_connect error_code=%s',
                               common.ErrorCode.Name(failure.code))
                delay = failure.retry_after
            except RetryConnection as failure:
                logger.warning('operation=bridge_connect error_code=%s',
                               common.ErrorCode.Name(failure.code))
                delay = failure.delay
            except grpc.aio.AioRpcError as failure:
                mapped = _grpc_failure(failure)
                if mapped.fatal:
                    self.fatal = mapped.code
                    logger.warning('operation=bridge_fatal error_code=%s',
                                   common.ErrorCode.Name(mapped.code))
                    stop.set()
                    if self._on_fatal is not None:
                        self._on_fatal()
                    return
                logger.warning('operation=bridge_connect error_code=%s',
                               common.ErrorCode.Name(mapped.code))
            except (asyncio.TimeoutError, ConnectionError, OSError):
                logger.warning('operation=bridge_connect error_code=UPSTREAM_UNAVAILABLE')
            except AdapterError as failure:
                logger.warning('operation=outbox error_code=%s', common.ErrorCode.Name(failure.code))
            except Exception:
                logger.error('operation=bridge_worker error_code=INTERNAL')
            if asyncio.current_task().cancelling():
                raise asyncio.CancelledError()
            if stop.is_set():
                break
            failures = min(failures + 1, 6)
            delay = max(delay, min(30.0, self.config.retry_base * 2 ** (failures - 1))
                        * self._random(0.8, 1.0))
            await wait_or_stop(stop, delay)


async def wait_or_stop(stop: asyncio.Event, delay: float):
    try:
        await asyncio.wait_for(stop.wait(), timeout=delay)
    except asyncio.TimeoutError:
        pass
