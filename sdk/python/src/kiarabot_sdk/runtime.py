from __future__ import annotations

import asyncio
import logging
import re
import uuid
from contextlib import suppress
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path

import grpc
from google.protobuf.json_format import Parse, ParseError
from kiarabot.v1 import common_pb2 as common, plugins_pb2 as plugins, services_pb2_grpc as services

logger = logging.getLogger(__name__)


class PluginError(Exception):
    def __init__(self, code: int, retryable: bool = False):
        self.code = code if code in common.ErrorCode.values() and code else common.INTERNAL
        self.retryable = retryable
        super().__init__(common.ErrorCode.Name(self.code))


class UncooperativeHook(PluginError):
    def __init__(self):
        super().__init__(common.TIMEOUT)


class Context:
    def __init__(self):
        self._permissions = frozenset()
        self._rpc = None
        self._features = frozenset()

    @property
    def granted_permissions(self) -> frozenset[str]:
        return self._permissions

    async def _call(self, request, timeout, feature="action.result.v1"):
        if timeout <= 0:
            raise PluginError(common.INVALID_ARGUMENT)
        if not self._rpc:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        if feature not in self._features:
            raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
        return await self._rpc(request, timeout)

    async def submit_action(self, action, timeout=5.0):
        return await self._call(plugins.PluginToCore(action=plugins.PluginActionRequest(
            request_id=str(uuid.uuid4()), action=action)), timeout)

    async def query_action(self, *, action_id=None, idempotency_key=None, timeout=5.0):
        if (action_id is None) == (idempotency_key is None) or timeout <= 0:
            raise PluginError(common.INVALID_ARGUMENT)
        request = plugins.PluginActionQuery(request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()))
        request.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=timeout))
        if action_id is not None:
            request.action_id = action_id
        else:
            request.idempotency_key = idempotency_key
        return await self._call(plugins.PluginToCore(action_query=request), timeout)

    async def cancel_action(self, action_id, timeout=5.0):
        if timeout <= 0:
            raise PluginError(common.INVALID_ARGUMENT)
        request = plugins.PluginActionCancel(request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()), action_id=action_id)
        request.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=timeout))
        return await self._call(plugins.PluginToCore(action_cancel=request), timeout)

    async def _host(self, operation, timeout, idempotency_key='', **kwargs):
        if not 0 < timeout <= 300:
            raise PluginError(common.INVALID_ARGUMENT)
        request = plugins.HostRequest(request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()),
                                      idempotency_key=idempotency_key, **{operation: kwargs})
        request.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=timeout))
        feature = 'host.model.v1' if operation == 'model_chat' else ('host.tasks.v1' if operation.startswith('task_') else 'host.storage.v1')
        try:
            response = await self._call(plugins.PluginToCore(host_request=request), timeout, feature)
        except PluginError as error:
            if operation == 'model_chat' and error.code != common.RATE_LIMITED:
                raise PluginError(error.code, False) from None
            raise
        return getattr(response, operation)

    async def storage_get(self, key, timeout=5.0):
        return (await self._host('storage_get', timeout, key=key)).value

    async def model_chat(self, model, messages, timeout=5.0):
        return await self._host('model_chat', timeout, model=model, messages=messages)

    async def task_create(self, task_id, target_conversation, payload, idempotency_key,
                          *, run_at=None, local_time=None, timeout=5.0):
        if (run_at is None) == (local_time is None):
            raise PluginError(common.INVALID_ARGUMENT)
        operation = dict(task_id=task_id, target_conversation=target_conversation, payload=payload)
        if run_at is not None:
            from google.protobuf.timestamp_pb2 import Timestamp
            if run_at.tzinfo is None or run_at.utcoffset() != timedelta(0):
                raise PluginError(common.INVALID_ARGUMENT)
            date = Timestamp()
            date.FromDatetime(run_at)
            operation['run_at'] = date
        else:
            operation['local_time'] = local_time
        return await self._host('task_create', timeout, idempotency_key, **operation)

    async def task_get(self, task_id, timeout=5.0):
        return await self._host('task_get', timeout, task_id=task_id)

    async def task_cancel(self, task_id, idempotency_key, timeout=5.0):
        return await self._host('task_cancel', timeout, idempotency_key, task_id=task_id)

    async def storage_set(self, key, value, idempotency_key, *, expires_at=None, timeout=5.0):
        operation = dict(key=key, value=value)
        if expires_at is not None:
            from google.protobuf.timestamp_pb2 import Timestamp
            expiration = Timestamp()
            expiration.FromDatetime(expires_at)
            operation['expires_at'] = expiration
        return await self._host('storage_set', timeout, idempotency_key, **operation)

    async def storage_delete(self, key, idempotency_key, timeout=5.0):
        return await self._host('storage_delete', timeout, idempotency_key, key=key)


class Plugin:
    async def on_start(self, context: Context) -> None:
        pass

    async def on_event(self, event, context: Context) -> None:
        raise PluginError(common.CAPABILITY_NOT_SUPPORTED)

    async def on_stop(self, reason: str) -> None:
        pass


def validate_manifest(manifest: plugins.PluginManifest) -> None:
    identifier = re.compile(r'[A-Za-z0-9_.-]{1,128}\Z')
    if (not identifier.fullmatch(manifest.id) or not identifier.fullmatch(manifest.version)
            or not manifest.name or len(manifest.name) > 128
            or manifest.api_version != 'kiarabot.plugin.v1'
            or not re.fullmatch(r'[A-Za-z_][\w.]*:[A-Za-z_]\w*', manifest.entrypoint)
            or len(manifest.entrypoint) > 256):
        raise PluginError(common.INVALID_ARGUMENT)
    if (manifest.runtime != 'python-rpc' or manifest.isolation != 'process'
            or manifest.dependencies or manifest.config_schema not in ('', '{}')):
        raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
    for values in (manifest.permissions, manifest.capabilities):
        if len(values) > 128 or len(set(values)) != len(values):
            raise PluginError(common.INVALID_ARGUMENT)


def load_manifest(path: str | Path) -> plugins.PluginManifest:
    try:
        raw = Path(path).read_bytes()
        if len(raw) > 65536:
            raise PluginError(common.INVALID_ARGUMENT)
        manifest = Parse(raw.decode('utf-8'), plugins.PluginManifest())
        validate_manifest(manifest)
        return manifest
    except (OSError, UnicodeError, ParseError, ValueError):
        raise PluginError(common.INVALID_ARGUMENT) from None


@dataclass(frozen=True)
class Options:
    endpoint: str
    token: str = field(repr=False)
    io_timeout: float = 5.0
    hook_timeout: float = 5.0
    heartbeat_interval: float = 1.0
    retry_delay: float = 1.0

    def __post_init__(self):
        address = re.fullmatch(r'127\.0\.0\.1:(\d+)', self.endpoint)
        if (not address or not 1 <= int(address[1]) <= 65535 or not 32 <= len(self.token) <= 4096
                or not all(33 <= ord(ch) <= 126 for ch in self.token)
                or min(self.io_timeout, self.hook_timeout, self.heartbeat_interval, self.retry_delay) <= 0):
            raise PluginError(common.INVALID_ARGUMENT)


async def invoke(hook, timeout):
    task = asyncio.create_task(hook)
    try:
        done, _ = await asyncio.wait({task}, timeout=timeout)
        if done:
            return await task
        task.cancel()
        done, _ = await asyncio.wait({task}, timeout=0.1)
        if not done:
            raise UncooperativeHook()
        with suppress(asyncio.CancelledError, Exception):
            await task
        raise PluginError(common.TIMEOUT, retryable=True)
    except asyncio.CancelledError:
        task.cancel()
        done, _ = await asyncio.wait({task}, timeout=0.1)
        if not done:
            raise UncooperativeHook()
        with suppress(asyncio.CancelledError, Exception):
            await task
        raise


class Runner:
    def __init__(self, manifest, plugin: Plugin, options: Options):
        validate_manifest(manifest)
        self.manifest = plugins.PluginManifest()
        self.manifest.CopyFrom(manifest)
        self.plugin, self.options = plugin, options
        self.context = Context()

    async def _write(self, call, request):
        await asyncio.wait_for(call.write(request), self.options.io_timeout)

    async def _session(self):
        self.context._permissions = frozenset()
        instance = str(uuid.uuid4())
        pending = {}
        expected = {}
        write_lock = asyncio.Lock()
        event_queue = asyncio.Queue(maxsize=1)
        worker = read_task = None
        async with grpc.aio.insecure_channel(self.options.endpoint, options=[
            ('grpc.max_receive_message_length', 1024 * 1024),
            ('grpc.max_send_message_length', 1024 * 1024),
        ]) as channel:
            call = services.PluginBridgeStub(channel).Connect(
                metadata=(('authorization', 'Bearer ' + self.options.token),))

            async def write(request):
                async with write_lock:
                    await self._write(call, request)

            async def rpc(request, timeout):
                if len(pending) >= 64:
                    raise PluginError(common.RATE_LIMITED, True)
                request_id = getattr(request, request.WhichOneof('body')).request_id
                future = asyncio.get_running_loop().create_future()
                pending[request_id] = future
                expected[request_id] = request.host_request.WhichOneof('operation') if request.HasField('host_request') else None
                try:
                    async with asyncio.timeout(timeout):
                        await write(request)
                        return await future
                except asyncio.TimeoutError:
                    raise PluginError(common.TIMEOUT, True) from None
                except grpc.RpcError:
                    raise PluginError(common.UPSTREAM_UNAVAILABLE, True) from None
                finally:
                    pending.pop(request_id, None)
                    expected.pop(request_id, None)
                    if not future.done():
                        future.cancel()
                    elif not future.cancelled():
                        future.exception()  # Consume a disconnect racing with write failure.

            async def events():
                while True:
                    event = await event_queue.get()
                    event_id, trace_id = event.event_id, event.trace_id
                    ack = plugins.PluginEventAck(event_id=event_id)
                    try:
                        await invoke(self.plugin.on_event(event, self.context), self.options.hook_timeout)
                        ack.accepted = True
                    except UncooperativeHook:
                        raise
                    except Exception as failure:
                        code = failure.code if isinstance(failure, PluginError) else common.INTERNAL
                        retryable = failure.retryable if isinstance(failure, PluginError) else False
                        ack.error.CopyFrom(common.Error(code=code, message=common.ErrorCode.Name(code),
                            request_id=event_id, trace_id=trace_id, retryable=retryable))
                    await write(plugins.PluginToCore(event_ack=ack))

            try:
                await write(plugins.PluginToCore(register=plugins.RegisterPluginRequest(
                    manifest=self.manifest, instance_id=instance, supported_features=['action.result.v1', 'host.storage.v1', 'host.model.v1', 'host.tasks.v1'])))
                response = await asyncio.wait_for(call.read(), self.options.io_timeout)
                if response is grpc.aio.EOF or response.WhichOneof('body') != 'register_result':
                    raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
                registered = response.register_result
                if not registered.accepted:
                    raise PluginError(registered.error.code, registered.error.retryable)
                if (registered.required_api_version != self.manifest.api_version
                        or set(registered.granted_permissions) != set(self.manifest.permissions)
                        or len(registered.granted_permissions) != len(set(registered.granted_permissions))
                        or registered.HasField('error')
                        or not set(registered.enabled_features).issubset({'action.result.v1', 'host.storage.v1', 'host.model.v1', 'host.tasks.v1'})):
                    raise PluginError(common.PERMISSION_DENIED)
                self.context._permissions = frozenset(registered.granted_permissions)
                self.context._features = frozenset(registered.enabled_features)
                self.context._rpc = rpc
                worker = asyncio.create_task(events())
                while True:
                    read_task = asyncio.create_task(call.read())
                    done, _ = await asyncio.wait({read_task, worker}, timeout=self.options.heartbeat_interval,
                                                return_when=asyncio.FIRST_COMPLETED)
                    if worker in done:
                        await worker
                    if not done:
                        await write(plugins.PluginToCore(heartbeat=plugins.PluginHeartbeat(instance_id=instance)))
                    response = await asyncio.wait_for(read_task, self.options.io_timeout)
                    read_task = None
                    if response is grpc.aio.EOF:
                        raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
                    kind = response.WhichOneof('body')
                    if kind == 'heartbeat_ack':
                        continue
                    if kind == 'error':
                        raise PluginError(response.error.code, response.error.retryable)
                    if kind == 'host_response':
                        result = response.host_response
                        future = pending.get(result.request_id)
                        if future is None or future.done():
                            continue
                        if result.HasField('error') and result.WhichOneof('result') is None:
                            future.set_exception(PluginError(result.error.code, result.error.retryable))
                        elif (not result.HasField('error') and expected.get(result.request_id)
                              and result.WhichOneof('result') == expected[result.request_id]):
                            future.set_result(result)
                        else:
                            future.set_exception(PluginError(common.UPSTREAM_UNAVAILABLE, True))
                        continue
                    if kind == 'action_response':
                        result = response.action_response
                        future = pending.get(result.request_id)
                        if future is None or future.done():
                            continue  # A response to a timed-out call cannot finish another call.
                        if expected.get(result.request_id) is not None:
                            future.set_exception(PluginError(common.UPSTREAM_UNAVAILABLE, True))
                            continue
                        outcome = result.WhichOneof('outcome')
                        if outcome == 'error':
                            future.set_exception(PluginError(result.error.code, result.error.retryable))
                        elif (outcome == 'result' and result.result.action_id and result.result.status in (1, 2, 3, 4, 5)
                              and (result.result.status == 1 or result.result.HasField('completed_at'))
                              and (result.result.status != 3 or (result.result.HasField('error') and result.result.error.code))):
                            future.set_result(result.result)
                        else:
                            future.set_exception(PluginError(common.UPSTREAM_UNAVAILABLE, True))
                        continue
                    if kind != 'event':
                        raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
                    try:
                        event_queue.put_nowait(response.event)
                    except asyncio.QueueFull:
                        raise PluginError(common.RATE_LIMITED, True) from None
            finally:
                self.context._permissions = frozenset()
                self.context._features = frozenset()
                self.context._rpc = None
                call.cancel()
                for future in pending.values():
                    if not future.done():
                        future.set_exception(PluginError(common.UPSTREAM_UNAVAILABLE, True))
                for task in (read_task, worker):
                    if task:
                        task.cancel()
                        with suppress(asyncio.CancelledError):
                            await task

    async def run(self, stop: asyncio.Event):
        session = None
        try:
            await invoke(self.plugin.on_start(self.context), self.options.hook_timeout)
            while not stop.is_set():
                stopper = asyncio.create_task(stop.wait())
                session = asyncio.create_task(self._session())
                try:
                    await asyncio.wait({stopper, session}, return_when=asyncio.FIRST_COMPLETED)
                    if stopper.done():
                        session.cancel()
                        with suppress(asyncio.CancelledError):
                            await session
                        return
                    await session
                except PluginError as failure:
                    if isinstance(failure, UncooperativeHook) or not failure.retryable:
                        raise
                    logger.warning('operation=plugin_connect error_code=%s', common.ErrorCode.Name(failure.code))
                except grpc.RpcError as failure:
                    if failure.code() in (grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED):
                        raise PluginError(common.PERMISSION_DENIED) from None
                    logger.warning('operation=plugin_connect error_code=UPSTREAM_UNAVAILABLE')
                except asyncio.TimeoutError:
                    logger.warning('operation=plugin_connect error_code=TIMEOUT')
                finally:
                    stopper.cancel()
                    with suppress(asyncio.CancelledError):
                        await stopper
                if asyncio.current_task().cancelling():
                    raise asyncio.CancelledError()
                if not stop.is_set():
                    with suppress(asyncio.TimeoutError):
                        await asyncio.wait_for(stop.wait(), self.options.retry_delay)
        finally:
            if session and not session.done():
                session.cancel()
                with suppress(asyncio.CancelledError):
                    await session
            await invoke(self.plugin.on_stop('shutdown'), self.options.hook_timeout)
