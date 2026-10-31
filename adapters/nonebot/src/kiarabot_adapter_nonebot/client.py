from __future__ import annotations

import asyncio
import logging
import random
import re
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
    def __init__(self, config: ClientConfig, outbox: Outbox, executor=None):
        self.config, self.outbox = config, outbox
        self.executor = executor
        self.instance = None

    def registration(self):
        request = adapters.RegisterAdapterRequest(adapter_id=self.config.adapter_id,
            implementation='kiarabot-adapter-nonebot', implementation_version='0.1.0',
            protocol_version='kiarabot.v1')
        request.accounts.add(account_id=self.config.account_id, enabled=True)
        request.capabilities.add(name='event.message_received', version='1', supported=True)
        if self.executor:
            request.capabilities.add(name='action.send_message.text.v1', version='1', supported=True)
        return adapters.AdapterToCore(register=request)

    async def read(self, call):
        while True:
            response = await asyncio.wait_for(call.read(), timeout=self.config.timeout)
            if response is grpc.aio.EOF:
                raise RetryConnection()
            if response.WhichOneof('body') != 'action':
                return response
            if not self.executor or not self.instance:
                raise RetryConnection(common.CAPABILITY_NOT_SUPPORTED)
            result = await self.executor.execute(response.action, self.instance)
            await self.write(call, adapters.AdapterToCore(action_result=result))

    async def write(self, call, request):
        await asyncio.wait_for(call.write(request), timeout=self.config.timeout)

    async def connection(self, stop: asyncio.Event):
        async with grpc.aio.insecure_channel(self.config.endpoint, options=[
            ('grpc.max_receive_message_length', 1024 * 1024),
            ('grpc.max_send_message_length', 1024 * 1024),
        ]) as channel:
            call = services.AdapterBridgeStub(channel).Connect(
                metadata=(('authorization', 'Bearer ' + self.config.token),))
            try:
                await self.write(call, self.registration())
                registered = await self.read(call)
                if registered.WhichOneof('body') != 'register_result':
                    raise RetryConnection(common.INTERNAL)
                result = registered.register_result
                if not result.accepted:
                    raise RetryConnection(result.error.code or common.INTERNAL)
                if result.required_protocol_version != 'kiarabot.v1' or not result.assigned_instance_id:
                    raise RetryConnection(common.INVALID_ARGUMENT)
                instance = result.assigned_instance_id
                self.instance = instance
                while not stop.is_set():
                    event = self.outbox.next_pending()
                    if event is None:
                        await self.write(call, adapters.AdapterToCore(
                            heartbeat=adapters.Heartbeat(instance_id=instance)))
                        heartbeat = await self.read(call)
                        if heartbeat.WhichOneof('body') != 'heartbeat_ack':
                            raise RetryConnection(common.INTERNAL)
                        await wait_or_stop(stop, self.config.idle_interval)
                        continue
                    event.source.adapter_instance_id = instance
                    await self.write(call, adapters.AdapterToCore(event=event))
                    response = await self.read(call)
                    if (response.WhichOneof('body') != 'event_ack'
                            or response.event_ack.event_id != event.event_id
                            or response.event_ack.trace_id != event.trace_id):
                        raise RetryConnection(common.INTERNAL)
                    ack = response.event_ack
                    if ack.status in (adapters.AdapterEventAck.PERSISTED, adapters.AdapterEventAck.DUPLICATE):
                        if ack.HasField('error'):
                            raise RetryConnection(common.INTERNAL)
                        self.outbox.finish(event.event_id)
                    elif ack.status == adapters.AdapterEventAck.REJECTED and ack.error.code:
                        if ack.error.code not in common.ErrorCode.values():
                            raise RetryConnection(common.INTERNAL)
                        # Ingress backpressure is always retryable: Core currently returns
                        # queue-full RATE_LIMITED with retryable=false, but resending the
                        # same event_id is idempotent and dropping it would lose the
                        # platform message (OneBot has no consumer ACK for replay).
                        retryable = (ack.error.retryable
                                     or ack.error.code in ALWAYS_RETRYABLE_REJECTIONS)
                        if retryable:
                            delay = min(30.0, ack.retry_after_ms / 1000) if ack.retry_after_ms else 0.0
                            raise RetryConnection(ack.error.code, delay)
                        self.outbox.finish(event.event_id, ack.error.code)
                    else:
                        raise RetryConnection(common.INTERNAL)
            finally:
                self.instance = None
                call.cancel()

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
