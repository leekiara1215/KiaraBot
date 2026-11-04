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
from kiarabot.v1 import common_pb2 as common, model_stream_pb2 as streaming
from kiarabot.v1 import platform_pb2 as platform, plugins_pb2 as plugins
from kiarabot.v1 import services_pb2_grpc as services

from . import actions as action_api, delivery, model_stream as stream_api
from . import tasks as task_api, transport
from .errors import PluginError

logger = logging.getLogger(__name__)


class UncooperativeHook(PluginError):
    def __init__(self):
        super().__init__(common.TIMEOUT)


class Context:
    def __init__(self):
        self._permissions = frozenset()
        self._rpc = None
        self._features = frozenset()
        self._capabilities = frozenset()
        self._config = None

    @property
    def config(self):
        if self._config is None:
            return None
        result = plugins.PluginConfig()
        result.CopyFrom(self._config)
        return result

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

    def _require_action_feature(self, timeout):
        if not isinstance(timeout, (int, float)) or not 0 < timeout <= action_api.MAX_DEADLINE_SECONDS:
            raise PluginError(common.INVALID_ARGUMENT)
        if not self._rpc:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        if action_api.ACTION_FEATURE not in self._features:
            raise PluginError(common.CAPABILITY_NOT_SUPPORTED)

    async def submit_action(self, action, *, idempotency_key=None, deadline=None, timeout=5.0):
        self._require_action_feature(timeout)
        prepared = action_api.prepare_submission(action, idempotency_key=idempotency_key,
                                                 deadline=deadline)
        return await self._call(plugins.PluginToCore(action=plugins.PluginActionRequest(
            request_id=str(uuid.uuid4()), action=prepared)), timeout)

    async def query_action(self, *, action_id=None, idempotency_key=None, timeout=5.0):
        self._require_action_feature(timeout)
        if (action_id is None) == (idempotency_key is None):
            raise PluginError(common.INVALID_ARGUMENT)
        request = plugins.PluginActionQuery(request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()))
        request.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=timeout))
        if action_id is not None:
            action_api.validate_identifier(action_id)
            request.action_id = action_id
        else:
            action_api.validate_identifier(idempotency_key)
            request.idempotency_key = idempotency_key
        return await self._call(plugins.PluginToCore(action_query=request), timeout)

    async def cancel_action(self, action_id, timeout=5.0):
        self._require_action_feature(timeout)
        action_api.validate_identifier(action_id)
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

    async def model_chat(self, model, messages, *, logical_call_id=None, timeout=5.0,
                         stream=False):
        """Non-streaming Model chat.

        ``logical_call_id`` is the durable call identity: generate a fresh UUIDv4 for a new call;
        for a retry or an UNKNOWN-outcome continuation reuse the exact same value so Model replays
        the recorded call instead of calling the provider again. Streaming is explicitly
        unsupported in this contract version. RATE_LIMITED and UPSTREAM_UNAVAILABLE
        (QUERY_SAME_INTENT) failures are marked retryable; never retry another failure blindly.
        """
        if stream:
            raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
        if not isinstance(model, str) or not model or len(model) > 128:
            raise PluginError(common.INVALID_ARGUMENT)
        if not isinstance(messages, (list, tuple)) or not messages or len(messages) > 128:
            raise PluginError(common.INVALID_ARGUMENT)
        wire_messages = []
        for message in messages:
            if not isinstance(message, dict):
                raise PluginError(common.INVALID_ARGUMENT)
            role = message.get('role')
            text = message.get('text', message.get('content'))
            if role not in ('system', 'user', 'assistant') or not isinstance(text, str):
                raise PluginError(common.INVALID_ARGUMENT)
            if len(text.encode('utf-8')) > 65536:
                raise PluginError(common.INVALID_ARGUMENT)
            wire_messages.append(plugins.ModelMessage(role=role, text=text))
        if logical_call_id is None:
            logical_call_id = str(uuid.uuid4())
        elif not isinstance(logical_call_id, str) or len(logical_call_id) != 36:
            raise PluginError(common.INVALID_ARGUMENT)
        request = plugins.HostRequest(request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()),
                                      model_chat=plugins.ModelChatRequest(
                                          model=model, messages=wire_messages,
                                          logical_call_id=logical_call_id),
                                      idempotency_key='')
        request.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=timeout))
        try:
            response = await self._call(plugins.PluginToCore(host_request=request), timeout,
                                        'host.model.v1')
        except PluginError as error:
            retryable = error.retryable and error.code in (common.RATE_LIMITED,
                                                           common.UPSTREAM_UNAVAILABLE,
                                                           common.TIMEOUT)
            raise PluginError(error.code, retryable) from None
        # _call/_rpc resolves host requests to the HostResponse itself, not the enclosing frame.
        result = response
        if result.HasField('error'):
            raise PluginError(result.error.code, result.error.retryable)
        return result.model_chat

    def model_stream(self, model, messages, *, logical_call_id=None,
                     timeout=stream_api.DEFAULT_DEADLINE_SECONDS):
        """Open one text stream attempt (negotiated ``host.model.stream.v1``).

        Returns a :class:`~kiarabot_sdk.model_stream.ModelStream` async context
        manager. ``logical_call_id`` is the stable business identity: generate a
        fresh UUIDv4 for a new call and reuse the exact same value for a retry or
        replay so Model returns replay metadata instead of calling the provider
        again. ``timeout`` is this attempt's HostRequest deadline (seconds,
        <=30). The optional feature is never implied by ``host.model.v1`` and the
        current permission is the existing ``model.chat``.

        An unsupported/not-negotiated feature fails locally with
        ``CAPABILITY_NOT_SUPPORTED`` and a missing ``model.chat`` grant with
        ``PERMISSION_DENIED``. ``model_chat(..., stream=True)`` keeps raising
        ``CAPABILITY_NOT_SUPPORTED`` and points callers here (no return-type
        change for the old API).
        """
        if not self._rpc:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        if stream_api.STREAM_FEATURE not in self._features:
            raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
        if stream_api.MODEL_PERMISSION not in self._permissions:
            raise PluginError(common.PERMISSION_DENIED)
        return stream_api.ModelStream(self._rpc, model, messages,
                                      logical_call_id=logical_call_id, timeout=timeout)

    def _require_tasks(self, timeout, *, creating=False):
        """P10c task preflight: feature, plugin permission and (create) timer subscription."""
        if not isinstance(timeout, (int, float)) or not 0 < timeout <= 300:
            raise PluginError(common.INVALID_ARGUMENT)
        if not self._rpc:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        if task_api.TASK_FEATURE not in self._features:
            raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
        if task_api.TASK_PERMISSION not in self._permissions:
            raise PluginError(common.PERMISSION_DENIED)
        if creating and task_api.TIMER_CAPABILITY not in self._capabilities:
            # Creating a task is only useful with event.timer.triggered delivery.
            raise PluginError(common.CAPABILITY_NOT_SUPPORTED)

    async def _task_call(self, operation, request, timeout, idempotency_key=''):
        host = plugins.HostRequest(request_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()),
                                   idempotency_key=idempotency_key,
                                   **{operation: request})
        host.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=timeout))
        response = await self._call(plugins.PluginToCore(host_request=host), timeout,
                                    task_api.TASK_FEATURE)
        return task_api.validate_response(getattr(response, operation), request.task_id)

    async def task_create(self, task_id, target_conversation, payload, idempotency_key,
                          *, run_at=None, local_time=None, timeout=5.0):
        self._require_tasks(timeout, creating=True)
        task_api.validate_idempotency_key(idempotency_key)
        request = task_api.create_request(task_id, target_conversation, payload,
                                          run_at=run_at, local_time=local_time)
        return await self._task_call('task_create', request, timeout, idempotency_key)

    async def task_get(self, task_id, timeout=5.0):
        self._require_tasks(timeout)
        return await self._task_call('task_get', task_api.get_request(task_id), timeout)

    async def task_cancel(self, task_id, idempotency_key, timeout=5.0):
        self._require_tasks(timeout)
        task_api.validate_idempotency_key(idempotency_key)
        return await self._task_call('task_cancel', task_api.cancel_request(task_id), timeout,
                                     idempotency_key)

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

    async def on_config(self, config, context: Context) -> None:
        pass

    async def on_stop(self, reason: str) -> None:
        pass


# Mirrors the host allow-list in core/src/application/plugin_hub.cpp: unknown
# capabilities and capabilities whose required permission was not requested are
# rejected by Core; failing locally keeps load_manifest a complete pre-flight.
CAPABILITY_PERMISSIONS = {
    'event.message.received': 'message.read',
    'event.message.sent': 'message.read',
    'event.group.member.changed': 'member.read',
    'event.friend.request': 'request.read',
    'event.adapter.status.changed': 'adapter.read',
    'event.timer.triggered': 'task.schedule',
}
MANIFEST_IDENTIFIER = re.compile(r'[A-Za-z0-9_.-]{1,128}\Z')
MANIFEST_ENTRYPOINT = re.compile(r'[A-Za-z_][\w.]*:[A-Za-z_]\w*\Z')
INSTANCE_ID = re.compile(r'[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\Z')


def validate_manifest(manifest: plugins.PluginManifest) -> None:
    if (not MANIFEST_IDENTIFIER.fullmatch(manifest.id)
            or not MANIFEST_IDENTIFIER.fullmatch(manifest.version)
            or not manifest.name or len(manifest.name) > 128
            or manifest.api_version != 'kiarabot.plugin.v1'
            # The SDK imports module:factory itself, so it must stay importable
            # even though the host only checks length. Intentionally stricter.
            or not MANIFEST_ENTRYPOINT.fullmatch(manifest.entrypoint)
            or len(manifest.entrypoint) > 256):
        raise PluginError(common.INVALID_ARGUMENT)
    if (manifest.runtime != 'python-rpc' or manifest.isolation != 'process'
            or manifest.dependencies or manifest.config_schema not in ('', '{}')):
        raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
    for values in (manifest.permissions, manifest.capabilities):
        if len(values) > 128 or len(set(values)) != len(values):
            raise PluginError(common.INVALID_ARGUMENT)
        if any(not MANIFEST_IDENTIFIER.fullmatch(value) for value in values):
            raise PluginError(common.INVALID_ARGUMENT)
    for capability in manifest.capabilities:
        required = CAPABILITY_PERMISSIONS.get(capability)
        if required is None:
            raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
        if required not in manifest.permissions:
            raise PluginError(common.PERMISSION_DENIED)
    # `ordered` is a delivery property persisted by Core and compared on
    # re-registration (CONFLICT on drift). The SDK forwards it unchanged and
    # never reinterprets it, so there is no local value to validate.


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
    #: PEM CA bundle (bytes) or a path to it. Required: the target Bridge is
    #: reachable only over server-verified TLS; there is no insecure fallback.
    tls_root_cert: bytes | str | Path | None = field(default=None, repr=False)
    tls_server_name: str | None = None
    #: Deployment-provided stable instance identity. When unset the SDK uses
    #: the instance id returned in the registration fence (development only).
    instance_id: str | None = None
    io_timeout: float = 5.0
    hook_timeout: float = 5.0
    heartbeat_interval: float = 5.0
    retry_delay: float = 1.0
    lease_renew_interval: float = delivery.RENEW_AFTER_SECONDS

    def __post_init__(self):
        transport.validate_endpoint(self.endpoint)
        if (not 32 <= len(self.token) <= 4096
                or not all(33 <= ord(ch) <= 126 for ch in self.token)
                or min(self.io_timeout, self.hook_timeout, self.heartbeat_interval,
                       self.retry_delay, self.lease_renew_interval) <= 0):
            raise PluginError(common.INVALID_ARGUMENT)
        root = transport.load_root_certificate(self.tls_root_cert)
        if not root:
            # Missing CA must fail closed before any connection is attempted.
            raise PluginError(common.INVALID_ARGUMENT)
        object.__setattr__(self, 'tls_root_certificate', root)
        if self.instance_id is not None and not INSTANCE_ID.fullmatch(self.instance_id):
            raise PluginError(common.INVALID_ARGUMENT)
        if self.tls_server_name is not None and (
                not self.tls_server_name or len(self.tls_server_name) > 253):
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


class _Rpc:
    """Callable closure used by :class:`Context` to multiplex stream RPC frames.

    One-answer-one-question calls use the bounded ``pending`` future map. Model
    text streams are independent: they are registered in ``streams`` before the
    start frame is written and their ordered events never complete a
    ``HostResponse`` future. A pre-start rejection may still arrive as one
    ``HostResponse`` for the start ``request_id``; the session loop resolves
    that into the stream registry instead of a pending call.
    """

    def __init__(self, write, pending, expected):
        self._write, self._pending, self._expected = write, pending, expected
        self.streams = stream_api.StreamRegistry()
        self.streams.cancel_callback = self._cancel_callback
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    async def __call__(self, frame, timeout):
        if self._closed:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        if len(self._pending) >= 64:
            raise PluginError(common.RATE_LIMITED, True)
        request_id = getattr(frame, frame.WhichOneof('body')).request_id
        future = asyncio.get_running_loop().create_future()
        self._pending[request_id] = future
        body = frame.WhichOneof('body')
        if body == 'host_request':
            expected = frame.host_request.WhichOneof('operation')
        elif body == 'action':
            expected = ('submit', frame.action.action.action_id)
        elif body == 'action_query':
            expected = ('query', frame.action_query.action_id or None)
        elif body == 'action_cancel':
            expected = ('cancel', frame.action_cancel.action_id)
        else:
            expected = None
        self._expected[request_id] = expected
        try:
            async with asyncio.timeout(timeout):
                await self._write(frame)
                return await future
        except asyncio.TimeoutError:
            raise PluginError(common.TIMEOUT, True) from None
        except grpc.RpcError:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True) from None
        finally:
            self._pending.pop(request_id, None)
            self._expected.pop(request_id, None)
            if not future.done():
                future.cancel()
            elif not future.cancelled():
                future.exception()  # Consume a disconnect racing with write failure.

    async def start_stream(self, request, attempt):
        """Write one ``model_stream_start`` and await admission.

        Admission resolves from the ordered stream events (started or a
        pre-start terminal) or from a pre-start HostResponse rejection. There is
        no successful HostResponse completion for a start request.
        """
        if self._closed:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        try:
            async with asyncio.timeout(attempt.timeout):
                await self._write(plugins.PluginToCore(host_request=request))
                return await attempt.wait_admission()
        except asyncio.TimeoutError:
            raise PluginError(common.TIMEOUT, True) from None
        except grpc.RpcError:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True) from None

    async def cancel_stream(self, start_request_id, logical_call_id, timeout):
        """Send one cancel request and return the typed receipt."""
        if self._closed:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        request = plugins.HostRequest(
            request_id=str(uuid.uuid4()),
            trace_id=str(uuid.uuid4()),
            idempotency_key='',
            model_stream_cancel=streaming.ModelStreamCancelRequest(
                start_request_id=start_request_id, logical_call_id=logical_call_id))
        request.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=timeout))
        response = await self(plugins.PluginToCore(host_request=request), timeout)
        if response.HasField('error'):
            raise PluginError(response.error.code, response.error.retryable)
        return response.model_stream_cancel

    async def _cancel_callback(self, start_request_id, logical_call_id):
        return await self.cancel_stream(start_request_id, logical_call_id,
                                        stream_api.DEFAULT_CANCEL_TIMEOUT)

    async def aclose(self):
        """Fail all in-flight streams as unknown delivery and join cancels."""
        self._closed = True
        await self.streams.close()


class Runner:
    def __init__(self, manifest, plugin: Plugin, options: Options):
        validate_manifest(manifest)
        self.manifest = plugins.PluginManifest()
        self.manifest.CopyFrom(manifest)
        self.plugin, self.options = plugin, options
        self.context = Context()
        self.context._capabilities = frozenset(self.manifest.capabilities)
        # A plugin instance keeps its identity across TLS reconnects; only the
        # connection generation changes. Deployment may pin it explicitly.
        self._instance_id = options.instance_id or str(uuid.uuid4())
        #: Last delivery dispositions observed, for tests/observability.
        self.last_dispositions: list[int] = []
        self._config_seen = {}
        self._applied_config_version = 0
        # P08 registration fact: the Runtime's durable desired config version for this
        # instance. The SDK only observes it; applied evidence stays Runtime-owned.
        self._desired_config_version = 0
        self._draining = False

    async def _write(self, call, request):
        try:
            await asyncio.wait_for(call.write(request), self.options.io_timeout)
        except (grpc.RpcError, asyncio.InvalidStateError):
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True) from None

    async def _configure(self, config, state, write):
        if not config.HasField('fence') or config.fence != state.fence:
            raise PluginError(common.INVALID_ARGUMENT)
        if (not config.config_version or not config.version or len(config.version) > 128
                or not config.delivery_id or len(config.delivery_id) > 128
                or not 1 <= config.apply_timeout_ms <= 5000):
            raise PluginError(common.INVALID_ARGUMENT)
        fingerprint = config.SerializeToString(deterministic=True)
        previous = self._config_seen.get(config.delivery_id)
        if previous:
            if previous[0] != fingerprint:
                raise PluginError(common.INVALID_ARGUMENT)
            await write(plugins.PluginToCore(config_ack=previous[1]))
            return
        ack = plugins.PluginConfigAck(version=config.version, delivery_id=config.delivery_id,
                                      config_version=config.config_version)
        ack.fence.CopyFrom(state.fence)
        snapshot = plugins.PluginConfig()
        snapshot.CopyFrom(config)
        if (self._desired_config_version
                and config.config_version > self._desired_config_version):
            # P08: a delivery above the Runtime's durable desired version is malformed and
            # must never be applied or advance local applied evidence. Report it in-band.
            ack.error.CopyFrom(common.Error(code=common.INVALID_ARGUMENT,
                                            message=common.ErrorCode.Name(common.INVALID_ARGUMENT),
                                            request_id=config.delivery_id))
            self._remember_config_ack(config, fingerprint, ack)
            await write(plugins.PluginToCore(config_ack=ack))
            return
        stale = config.config_version < self._applied_config_version
        try:
            if stale:
                # A lower config version never replaces applied evidence.
                ack.accepted = True
            else:
                await invoke(self.plugin.on_config(config, self.context),
                             min(self.options.hook_timeout, config.apply_timeout_ms / 1000))
                self.context._config = snapshot
                self._applied_config_version = config.config_version
                ack.accepted = True
        except UncooperativeHook:
            raise
        except Exception as failure:
            code = failure.code if isinstance(failure, PluginError) else common.INTERNAL
            retryable = failure.retryable if isinstance(failure, PluginError) else False
            if not code or code not in common.ErrorCode.values():
                code, retryable = common.INTERNAL, False
            ack.error.CopyFrom(common.Error(code=code, message=common.ErrorCode.Name(code),
                                            request_id=ack.delivery_id, retryable=retryable))
        self._remember_config_ack(config, fingerprint, ack)
        await write(plugins.PluginToCore(config_ack=ack))

    def _remember_config_ack(self, config, fingerprint, ack):
        """Cache the ACK per delivery so a replayed delivery never repeats the callback."""
        self._config_seen[ack.delivery_id] = (fingerprint, ack)
        if len(self._config_seen) > 32:
            del self._config_seen[next(iter(self._config_seen))]

    @staticmethod
    def _reject_stream_start(attempt, result):
        """Resolve a pending stream start from one HostResponse.

        A start attempt is normally answered by ordered ``model_stream_event``
        frames. A HostResponse can only be a pre-start rejection or an unknown
        outcome, so it must end the pending start instead of leaving it waiting
        for events that will never arrive. It is never treated as stream success.
        """
        if attempt.started is not None:
            attempt.fail_unknown('unexpected_host_response')
            return
        if result.HasField('error') and result.WhichOneof('result') is None:
            attempt.reject_admission(
                PluginError(result.error.code, result.error.retryable))
        else:
            attempt.reject_admission(PluginError(common.UPSTREAM_UNAVAILABLE, True))

    async def _session(self):
        self.context._permissions = frozenset()
        self.context._config = None
        instance = self._instance_id
        pending = {}
        expected = {}
        receipt_waiters = {}
        refresh_waiters = {}
        write_lock = asyncio.Lock()
        event_queue = asyncio.Queue(maxsize=2)
        incoming = asyncio.Queue()
        failure_future = asyncio.get_running_loop().create_future()
        reader = worker = refresh_task = None
        connection = None
        channel = transport.open_channel(self.options.endpoint,
                                         self.options.tls_root_certificate,
                                         self.options.tls_server_name)
        async with channel:
            call = services.PluginBridgeStub(channel).Connect(
                metadata=(('authorization', 'Bearer ' + self.options.token),))

            async def write(frame):
                async with write_lock:
                    if connection is not None:
                        connection.attach(frame)
                    await self._write(call, frame)

            async def reader_loop():
                while True:
                    try:
                        frame = await call.read()
                    except (grpc.aio.EOF, grpc.RpcError):
                        await incoming.put(None)
                        return
                    if frame is grpc.aio.EOF:
                        await incoming.put(None)
                        return
                    await incoming.put(frame)

            async def refresh_loop():
                nonlocal connection
                while True:
                    state = connection
                    if state is None:
                        await asyncio.sleep(0.5)
                        continue
                    delay = max(0.5, state.seconds_until_expiry(transport.monotonic_seconds())
                                - transport.REFRESH_LEAD_SECONDS)
                    await asyncio.sleep(delay)
                    state = connection
                    if state is None:
                        continue
                    request_id = str(uuid.uuid4())
                    future = asyncio.get_running_loop().create_future()
                    refresh_waiters[request_id] = future
                    try:
                        await write(plugins.PluginToCore(
                            refresh_authorization=transport.build_refresh(state, request_id)))
                        result = await asyncio.wait_for(future, self.options.io_timeout)
                        connection = transport.accept_refresh(
                            state, request_id, result, transport.monotonic_seconds())
                    finally:
                        refresh_waiters.pop(request_id, None)
                        if not future.done():
                            future.cancel()

            async def renew(lease, stop):
                while not stop.is_set():
                    try:
                        await asyncio.wait_for(stop.wait(), self.options.lease_renew_interval)
                        return
                    except asyncio.TimeoutError:
                        pass
                    if lease.total_exhausted(transport.monotonic_seconds()):
                        return
                    await write(plugins.PluginToCore(renew_delivery=lease.attempt))

            async def deliver(frame):
                state = connection
                if state is None:
                    raise PluginError(common.UNAUTHENTICATED, retryable=True)
                event = frame.event
                lease = delivery.DeliveryLease.validate(
                    frame.delivery_attempt, event.event_id, state.fence,
                    transport.monotonic_seconds())
                waiter = asyncio.get_running_loop().create_future()
                receipt_waiters[(lease.event_id, lease.attempt_id)] = waiter
                stop_renew = asyncio.Event()
                renew_task = asyncio.create_task(renew(lease, stop_renew))
                ack = plugins.PluginEventAck(event_id=event.event_id)
                try:
                    await invoke(self.plugin.on_event(event, self.context), self.options.hook_timeout)
                    ack.accepted = True
                except UncooperativeHook:
                    raise
                except Exception as failure:
                    code = failure.code if isinstance(failure, PluginError) else common.INTERNAL
                    retryable = failure.retryable if isinstance(failure, PluginError) else False
                    ack.error.CopyFrom(common.Error(code=code, message=common.ErrorCode.Name(code),
                                                   request_id=event.event_id,
                                                   trace_id=event.trace_id, retryable=retryable))
                finally:
                    stop_renew.set()
                    renew_task.cancel()
                    with suppress(asyncio.CancelledError):
                        await renew_task
                ack.attempt.CopyFrom(lease.attempt)
                await write(plugins.PluginToCore(event_ack=ack))
                try:
                    receipt = await asyncio.wait_for(waiter, self.options.io_timeout)
                except asyncio.TimeoutError:
                    # Receipt loss leaves the outcome unknown; the runtime will
                    # redeliver or keep the event unacknowledged.
                    logger.warning('operation=plugin_delivery outcome=unknown attempt_id=%s',
                                   lease.attempt_id)
                    return
                finally:
                    receipt_waiters.pop((lease.event_id, lease.attempt_id), None)
                self.last_dispositions.append(
                    delivery.validate_receipt(receipt, lease, ack.accepted))

            async def events():
                try:
                    while True:
                        frame = await event_queue.get()
                        try:
                            if frame.WhichOneof('body') == 'config':
                                await self._configure(frame.config, connection, write)
                            else:
                                await deliver(frame)
                        finally:
                            event_queue.task_done()
                except Exception as failure:  # noqa: BLE001 - surfaced to the session loop
                    if not failure_future.done():
                        failure_future.set_exception(failure)
                    incoming.put_nowait(None)

            try:
                # First frame is the pre-registration frame: no fence/proof yet.
                await self._write(call, plugins.PluginToCore(register=plugins.RegisterPluginRequest(
                    manifest=self.manifest, instance_id=instance,
                    supported_features=list(transport.TARGET_FEATURES))))
                response = await asyncio.wait_for(call.read(), self.options.io_timeout)
                if response is grpc.aio.EOF or response.WhichOneof('body') != 'register_result':
                    raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
                registered = response.register_result
                expected_instance = self.options.instance_id or registered.fence.instance_id
                connection = transport.validate_registration_response(
                    registered, expected_instance, transport.monotonic_seconds())
                if (registered.required_api_version != self.manifest.api_version
                        or set(registered.granted_permissions) != set(self.manifest.permissions)
                        or len(registered.granted_permissions) != len(set(registered.granted_permissions))
                        or registered.HasField('error')
                        or not set(registered.enabled_features).issubset(set(transport.TARGET_FEATURES))):
                    raise PluginError(common.PERMISSION_DENIED)
                self._desired_config_version = registered.desired_config_version
                self.context._permissions = frozenset(registered.granted_permissions)
                self.context._features = frozenset(registered.enabled_features)
                self.context._rpc = _Rpc(write, pending, expected)
                worker = asyncio.create_task(events())
                refresh_task = asyncio.create_task(refresh_loop())
                reader = asyncio.create_task(reader_loop())
                drain_deadline = None
                while True:
                    if self._draining:
                        # Keep reading delivery receipts/host responses while accepted work
                        # finishes; otherwise an in-flight ACK would wait forever for a receipt
                        # that the already-broken reader never processes.
                        join_task = asyncio.create_task(event_queue.join())
                        incoming_task = asyncio.create_task(incoming.get())
                        now = asyncio.get_running_loop().time()
                        remaining = (drain_deadline - now
                                     if drain_deadline is not None else self.options.io_timeout)
                        timeout = max(0.0, min(self.options.heartbeat_interval, remaining))
                        done, _ = await asyncio.wait({join_task, incoming_task}, timeout=timeout,
                                                     return_when=asyncio.FIRST_COMPLETED)
                        if join_task in done:
                            if not incoming_task.done():
                                incoming_task.cancel()
                                with suppress(asyncio.CancelledError):
                                    await incoming_task
                            break
                        if incoming_task not in done:
                            incoming_task.cancel()
                            with suppress(asyncio.CancelledError):
                                await incoming_task
                            if (drain_deadline is not None
                                    and asyncio.get_running_loop().time() >= drain_deadline):
                                break
                            continue
                        response = incoming_task.result()
                        join_task.cancel()
                        with suppress(asyncio.CancelledError):
                            await join_task
                    else:
                        try:
                            response = await asyncio.wait_for(incoming.get(),
                                                              self.options.heartbeat_interval)
                        except asyncio.TimeoutError:
                            await write(plugins.PluginToCore(
                                heartbeat=plugins.PluginHeartbeat(
                                    instance_id=connection.fence.instance_id)))
                            continue
                    if response is None:
                        if failure_future.done():
                            raise failure_future.exception()
                        raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
                    kind = response.WhichOneof('body')
                    if kind is None:
                        raise PluginError(common.INVALID_ARGUMENT)
                    if kind == 'shutdown':
                        self._draining = True
                        drain_ms = response.shutdown.drain_timeout_ms
                        drain_deadline = asyncio.get_running_loop().time() + max(
                            0.001,
                            min(drain_ms / 1000.0 if drain_ms else self.options.io_timeout,
                                self.options.io_timeout))
                        continue
                    transport.validate_frame_authorization(response, connection)
                    if kind == 'heartbeat_ack':
                        continue
                    if kind == 'authorization_result':
                        result = response.authorization_result
                        waiter = refresh_waiters.get(result.request_id)
                        if waiter is not None and not waiter.done():
                            waiter.set_result(result)
                        continue
                    if kind == 'delivery_receipt':
                        receipt = response.delivery_receipt
                        disposition = receipt.disposition
                        if disposition in (platform.DELIVERY_RENEWED, platform.DELIVERY_STALE):
                            self.last_dispositions.append(disposition)
                            continue
                        attempt = receipt.attempt
                        waiter = receipt_waiters.get((attempt.event_id, attempt.attempt_id))
                        if waiter is not None and not waiter.done():
                            waiter.set_result(receipt)
                        continue
                    if kind == 'error':
                        raise PluginError(response.error.code, response.error.retryable)
                    if kind == 'model_stream_event':
                        # Ordered stream events have their own registry; they never
                        # complete a HostResponse future. Dispatch is non-blocking so a
                        # slow stream consumer cannot stall heartbeat/refresh/ACK here.
                        rpc = self.context._rpc
                        if rpc is not None:
                            rpc.streams.dispatch(response.model_stream_event)
                        continue
                    if kind == 'host_response':
                        result = response.host_response
                        future = pending.get(result.request_id)
                        if future is None or future.done():
                            # A start request is answered by stream events, but a
                            # pre-start rejection/unknown may arrive as a HostResponse.
                            # Ending that pending stream is mandatory.
                            rpc = self.context._rpc
                            attempt = rpc.streams.find(result.request_id) if rpc else None
                            if attempt is not None:
                                self._reject_stream_start(attempt, result)
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
                        try:
                            action_api.validate_response(result,
                                                         expected.get(result.request_id))
                        except PluginError as failure:
                            future.set_exception(failure)
                            continue
                        outcome = result.WhichOneof('outcome')
                        if outcome == 'error':
                            future.set_exception(PluginError(result.error.code,
                                                             result.error.retryable))
                        else:
                            future.set_result(result.result)
                        continue
                    if kind == 'config':
                        if 'plugin.config.v1' not in self.context._features:
                            raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
                    elif kind != 'event':
                        raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
                    try:
                        event_queue.put_nowait(response)
                    except asyncio.QueueFull:
                        raise PluginError(common.RATE_LIMITED, True) from None
                if self._draining:
                    with suppress(asyncio.TimeoutError):
                        await asyncio.wait_for(event_queue.join(), self.options.io_timeout)
            finally:
                rpc = self.context._rpc
                self.context._permissions = frozenset()
                self.context._features = frozenset()
                self.context._config = None
                self.context._rpc = None
                if rpc is not None:
                    # End every in-flight stream as unknown delivery before the
                    # transport is torn down; no stream may wait for a terminal
                    # that this connection can no longer deliver.
                    await rpc.aclose()
                call.cancel()
                for future in pending.values():
                    if not future.done():
                        future.set_exception(PluginError(common.UPSTREAM_UNAVAILABLE, True))
                for future in refresh_waiters.values():
                    if not future.done():
                        future.set_exception(PluginError(common.UPSTREAM_UNAVAILABLE, True))
                for task in (reader, worker, refresh_task):
                    if task:
                        task.cancel()
                        with suppress(asyncio.CancelledError, Exception):
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
                if self._draining:
                    # A graceful ShutdownNotice asks the client to stop; do not
                    # reconnect after the in-flight work is acknowledged.
                    return
                if not stop.is_set():
                    with suppress(asyncio.TimeoutError):
                        await asyncio.wait_for(stop.wait(), self.options.retry_delay)
        finally:
            if session and not session.done():
                session.cancel()
                with suppress(asyncio.CancelledError):
                    await session
            await invoke(self.plugin.on_stop('shutdown'), self.options.hook_timeout)

