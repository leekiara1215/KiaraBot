from __future__ import annotations

import asyncio
import logging
import random
import re
import time
from contextlib import suppress
from dataclasses import dataclass, field

import grpc
from kiarabot.v1 import adapters_pb2 as adapters, common_pb2 as common, services_pb2_grpc as services

from .mapper import AdapterError, identifier
from .outbox import Outbox

logger = logging.getLogger(__name__)

# Rejections that must keep the event pending even if Core omits retryable=true.
ALWAYS_RETRYABLE_REJECTIONS = frozenset({common.RATE_LIMITED})


@dataclass(frozen=True)
class ClientConfig:
    endpoint: str
    token: str = field(repr=False)
    adapter_id: str = 'nonebot'
    account_id: str = ''
    timeout: float = 5.0
    retry_base: float = 1.0
    idle_interval: float = 1.0

    def __post_init__(self):
        match = re.fullmatch(r'127\.0\.0\.1:(\d+)', self.endpoint)
        if (not match or not 1 <= int(match[1]) <= 65535 or not 32 <= len(self.token) <= 4096
                or not all(33 <= ord(ch) <= 126 for ch in self.token)
                or self.timeout <= 0 or self.retry_base <= 0 or self.idle_interval <= 0):
            raise AdapterError(common.INVALID_ARGUMENT)
        identifier(self.adapter_id)
        identifier(self.account_id)


class RetryConnection(Exception):
    def __init__(self, code=common.UPSTREAM_UNAVAILABLE, delay=0.0):
        self.code, self.delay = code, delay


class CoreClient:
    def __init__(self, config: ClientConfig, outbox: Outbox, executor=None, status=None):
        self.config, self.outbox = config, outbox
        self.executor, self.status = executor, status
        self.instance = None
        self.heartbeat_interval = 30.0

    def report_status(self, state: int, reason: str = '') -> None:
        if self.status is None:
            return
        try:
            self.status.report(state, reason)
        except AdapterError as failure:
            logger.warning('operation=status_report error_code=%s', common.ErrorCode.Name(failure.code))

    def registration(self):
        request = adapters.RegisterAdapterRequest(adapter_id=self.config.adapter_id,
            implementation='kiarabot-adapter-nonebot', implementation_version='0.1.0',
            protocol_version='kiarabot.v1')
        request.accounts.add(account_id=self.config.account_id, enabled=True)
        request.capabilities.add(name='event.message_received', version='1', supported=True)
        if self.executor:
            request.capabilities.add(name='action.send_message.text.v1', version='1', supported=True)
            request.capabilities.add(name='action.react_message.v1', version='1', supported=True)
            request.capabilities.add(name='action.delete_message.v1', version='1', supported=True)
            request.capabilities.add(name='action.mute_member.v1', version='1', supported=True)
            request.capabilities.add(name='action.kick_member.v1', version='1', supported=True)
        return adapters.AdapterToCore(register=request)

    async def write(self, call, request):
        await asyncio.wait_for(call.write(request), timeout=self.config.timeout)

    async def next_response(self, inbox):
        response = await asyncio.wait_for(inbox.get(), timeout=self.config.timeout)
        if response is None:
            raise RetryConnection()
        return response

    async def next_control(self, call, inbox, instance):
        """Return the next non-Action response, executing Actions as they arrive."""
        while True:
            response = await self.next_response(inbox)
            if response.WhichOneof('body') != 'action':
                return response
            if not self.executor:
                raise RetryConnection(common.CAPABILITY_NOT_SUPPORTED)
            result = await self.executor.execute(response.action, instance)
            await self.write(call, adapters.AdapterToCore(action_result=result))

    async def connection(self, stop: asyncio.Event):
        async with grpc.aio.insecure_channel(self.config.endpoint, options=[
            ('grpc.max_receive_message_length', 1024 * 1024),
            ('grpc.max_send_message_length', 1024 * 1024),
        ]) as channel:
            call = services.AdapterBridgeStub(channel).Connect(
                metadata=(('authorization', 'Bearer ' + self.config.token),))
            inbox = asyncio.Queue()
            reader = None

            async def pump():
                try:
                    while True:
                        response = await call.read()
                        if response is grpc.aio.EOF:
                            await inbox.put(None)
                            return
                        await inbox.put(response)
                except asyncio.CancelledError:
                    raise
                except Exception:
                    await inbox.put(None)

            try:
                reader = asyncio.create_task(pump())
                await self.write(call, self.registration())
                registered = await self.next_response(inbox)
                if registered.WhichOneof('body') != 'register_result':
                    raise RetryConnection(common.INTERNAL)
                result = registered.register_result
                if not result.accepted:
                    raise RetryConnection(result.error.code or common.INTERNAL)
                if result.required_protocol_version != 'kiarabot.v1' or not result.assigned_instance_id:
                    raise RetryConnection(common.INVALID_ARGUMENT)
                instance = result.assigned_instance_id
                self.instance = instance
                # The server owns the heartbeat cadence; fall back to 30s when unset.
                self.heartbeat_interval = float(result.heartbeat_interval_seconds or 30)
                self.report_status(common.HEALTHY, 'core_connected')
                last_heartbeat = time.monotonic()
                while not stop.is_set():
                    event = self.outbox.next_pending()
                    if event is not None:
                        event.source.adapter_instance_id = instance
                        await self.write(call, adapters.AdapterToCore(event=event))
                        response = await self.next_control(call, inbox, instance)
                        if (response.WhichOneof('body') != 'event_ack'
                                or response.event_ack.event_id != event.event_id
                                or response.event_ack.trace_id != event.trace_id):
                            raise RetryConnection(common.INTERNAL)
                        self.handle_event_ack(event, response.event_ack)
                        continue
                    now = time.monotonic()
                    if now - last_heartbeat >= self.heartbeat_interval:
                        await self.write(call, adapters.AdapterToCore(
                            heartbeat=adapters.Heartbeat(
                                instance_id=instance, sent_at_unix_ms=int(time.time() * 1000))))
                        last_heartbeat = now
                        continue
                    # Keep reading while idle: Core pushes Actions asynchronously and
                    # they must not wait for the next heartbeat.
                    try:
                        response = await asyncio.wait_for(
                            inbox.get(), min(self.config.idle_interval,
                                             max(0.01, self.heartbeat_interval - (now - last_heartbeat))))
                    except asyncio.TimeoutError:
                        continue
                    if response is None:
                        raise RetryConnection()
                    body = response.WhichOneof('body')
                    if body == 'action':
                        if not self.executor:
                            raise RetryConnection(common.CAPABILITY_NOT_SUPPORTED)
                        result = await self.executor.execute(response.action, instance)
                        await self.write(call, adapters.AdapterToCore(action_result=result))
                    elif body == 'heartbeat_ack':
                        last_heartbeat = time.monotonic()
                    else:
                        raise RetryConnection(common.INTERNAL)
            finally:
                self.instance = None
                if reader is not None:
                    reader.cancel()
                    with suppress(asyncio.CancelledError):
                        await reader
                call.cancel()

    def handle_event_ack(self, event, ack) -> None:
        if ack.status in (adapters.AdapterEventAck.PERSISTED, adapters.AdapterEventAck.DUPLICATE):
            if ack.HasField('error'):
                raise RetryConnection(common.INTERNAL)
            self.outbox.finish(event.event_id)
            return
        if ack.status == adapters.AdapterEventAck.REJECTED and ack.error.code:
            if ack.error.code not in common.ErrorCode.values():
                raise RetryConnection(common.INTERNAL)
            # Ingress backpressure is always retryable: Core currently returns queue-full
            # RATE_LIMITED with retryable=false, but resending the same event_id is
            # idempotent and dropping it would lose the platform message (OneBot has no
            # consumer ACK for replay).
            retryable = ack.error.retryable or ack.error.code in ALWAYS_RETRYABLE_REJECTIONS
            if retryable:
                delay = min(30.0, ack.retry_after_ms / 1000) if ack.retry_after_ms else 0.0
                raise RetryConnection(ack.error.code, delay)
            self.outbox.finish(event.event_id, ack.error.code)
            return
        raise RetryConnection(common.INTERNAL)

    async def run(self, stop: asyncio.Event):
        failures = 0
        while not stop.is_set():
            delay = 0.0
            try:
                await self.connection(stop)
                failures = 0
            except asyncio.CancelledError:
                raise
            except RetryConnection as failure:
                logger.warning('operation=core_connect error_code=%s', common.ErrorCode.Name(failure.code)
                               if failure.code in common.ErrorCode.values() else 'INTERNAL')
                delay = failure.delay
            except (grpc.RpcError, asyncio.TimeoutError):
                logger.warning('operation=core_connect error_code=UPSTREAM_UNAVAILABLE')
            except AdapterError as failure:
                logger.warning('operation=outbox error_code=%s', common.ErrorCode.Name(failure.code))
            except Exception:
                logger.error('operation=core_worker error_code=INTERNAL')
            if asyncio.current_task().cancelling():
                raise asyncio.CancelledError()
            if not stop.is_set():
                failures = min(failures + 1, 6)
                delay = max(delay, min(30.0, self.config.retry_base * 2 ** (failures - 1)) * random.uniform(0.8, 1.0))
                await wait_or_stop(stop, delay)


async def wait_or_stop(stop: asyncio.Event, delay: float):
    try:
        await asyncio.wait_for(stop.wait(), timeout=delay)
    except asyncio.TimeoutError:
        pass
