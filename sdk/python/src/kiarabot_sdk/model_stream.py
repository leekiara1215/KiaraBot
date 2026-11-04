"""Async text streaming client for the frozen model-stream contract.

The wire contract is frozen in ``proto/kiarabot/v1/model_stream.proto`` and
``plugins.proto``:

* ``HostRequest.model_stream_start`` (18) is answered **only** by ordered
  ``CoreToPlugin.model_stream_event`` (11) frames, never by a normal
  ``HostResponse`` result. A pre-start rejection may arrive as a
  ``HostResponse.error`` for the start ``request_id``; the SDK ends the pending
  start either way and never waits forever for a stream event.
* ``HostRequest.model_stream_cancel`` (19) is a normal one-answer-one-question
  request whose ``HostResponse.model_stream_cancel`` (18) receipt only
  acknowledges local cancellation handling. The receipt is not proof that the
  provider did not run or was not billed, and it never replaces the attempt's
  terminal event.
* ``logical_call_id`` is the stable business identity (lowercase UUIDv4,
  generated before a new call and reused on a retry/replay). The start
  ``request_id`` and ``trace_id`` identify one delivery attempt.

Ordered attempt events are ``started(sequence=0)``, ``delta(1..N)``,
``terminal(N+1)``. A pre-start rejection has only ``terminal(sequence=0)``.
A hard disconnect may deliver no terminal at all; that is an *unknown
delivery*, so the SDK never fabricates completion and keeps the locally
received partial text plus received counters.

This module owns the per-connection stream registry. ``Context.model_stream``
returns a :class:`ModelStream` object that registers itself before the start
frame is written, so no event can race ahead of the registration. The shared
reader dispatches events with non-blocking local queues: a slow consumer can
only lose its own stream (local unknown termination plus one best-effort
cancel), never heartbeat/refresh/Action/ACK processing.
"""

from __future__ import annotations

import asyncio
import re
import uuid
from collections import deque
from contextlib import suppress
from datetime import datetime, timedelta, timezone

from kiarabot.v1 import common_pb2 as common
from kiarabot.v1 import model_stream_pb2 as streaming
from kiarabot.v1 import platform_pb2 as platform
from kiarabot.v1 import plugins_pb2 as plugins

from .errors import PluginError

#: Independent negotiation feature. It is not implied by ``host.model.v1``.
STREAM_FEATURE = 'host.model.stream.v1'
#: The streaming path reuses the existing model permission; no new permission.
MODEL_PERMISSION = 'model.chat'

#: Frozen service limits (also enforced server side; the SDK mirrors them so a
#: local adapter cannot silently exceed the negotiated bounds).
MAX_ACTIVE_STREAMS = 4
MAX_QUEUED_DELTAS = 32
MAX_QUEUED_BYTES = 65536
MAX_DELTA_BYTES = 4096
MAX_RESULT_BYTES = 65536
MAX_INPUT_BYTES = 65536
MAX_INPUT_MESSAGES = 128
MAX_MODEL_BYTES = 256
MAX_DEADLINE_SECONDS = 30.0
DEFAULT_DEADLINE_SECONDS = 30.0
DEFAULT_CANCEL_TIMEOUT = 5.0

DISPOSITION_FRESH = streaming.MODEL_STREAM_FRESH
DISPOSITION_REPLAY_IN_PROGRESS = streaming.MODEL_STREAM_REPLAY_IN_PROGRESS
DISPOSITION_REPLAY_TERMINAL = streaming.MODEL_STREAM_REPLAY_TERMINAL
_DISPOSITIONS = frozenset({DISPOSITION_FRESH, DISPOSITION_REPLAY_IN_PROGRESS,
                           DISPOSITION_REPLAY_TERMINAL})

TERMINATION_COMPLETED = streaming.MODEL_STREAM_COMPLETED
TERMINATION_TRUNCATED = streaming.MODEL_STREAM_TRUNCATED
TERMINATION_FAILED = streaming.MODEL_STREAM_FAILED
TERMINATION_CANCELLED = streaming.MODEL_STREAM_CANCELLED
TERMINATION_REPLAY = streaming.MODEL_STREAM_REPLAY
_TERMINATIONS = frozenset({TERMINATION_COMPLETED, TERMINATION_TRUNCATED,
                           TERMINATION_FAILED, TERMINATION_CANCELLED, TERMINATION_REPLAY})

FINISH_UNSPECIFIED = streaming.MODEL_STREAM_FINISH_REASON_UNSPECIFIED
FINISH_STOP = streaming.MODEL_STREAM_FINISH_STOP
FINISH_LENGTH = streaming.MODEL_STREAM_FINISH_LENGTH
FINISH_CONTENT_FILTER = streaming.MODEL_STREAM_FINISH_CONTENT_FILTER
_FINISH_REASONS = frozenset({FINISH_UNSPECIFIED, FINISH_STOP, FINISH_LENGTH,
                             FINISH_CONTENT_FILTER})

CANCEL_REQUESTED = streaming.MODEL_STREAM_CANCELLATION_REQUESTED
CANCEL_ALREADY_TERMINAL = streaming.MODEL_STREAM_ALREADY_TERMINAL
CANCEL_NOT_ACTIVE = streaming.MODEL_STREAM_NOT_ACTIVE
_CANCEL_DISPOSITIONS = frozenset({CANCEL_REQUESTED, CANCEL_ALREADY_TERMINAL, CANCEL_NOT_ACTIVE})

_UUID4 = re.compile(
    r'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z')
_CANONICAL_DOMAIN = b'kiarabot.model.Chat.v1'
VALID_ROLES = frozenset({'system', 'user', 'assistant'})


def valid_uuid4(value) -> bool:
    """Return True for a lowercase RFC 4122 UUIDv4 string."""
    return isinstance(value, str) and bool(_UUID4.fullmatch(value))


def new_uuid4() -> str:
    return str(uuid.uuid4())


def _utf8_bytes(text):
    if not isinstance(text, str) or '\x00' in text:
        return None
    try:
        return text.encode('utf-8')
    except UnicodeError:
        return None


def prepare_messages(model, messages):
    """Validate one start request locally and return ``(model, wire_messages)``.

    Mirrors the frozen input bounds: 1..128 messages, only system/user/assistant
    roles, no NUL in text, and a canonical fingerprint (model + roles + texts)
    of at most 64 KiB. A local rejection happens before any start frame is sent.
    """
    model_bytes = _utf8_bytes(model)
    if model_bytes is None or not model_bytes or len(model_bytes) > MAX_MODEL_BYTES:
        raise PluginError(common.INVALID_ARGUMENT)
    if not isinstance(messages, (list, tuple)) or not messages:
        raise PluginError(common.INVALID_ARGUMENT)
    if len(messages) > MAX_INPUT_MESSAGES:
        raise PluginError(common.INVALID_ARGUMENT)
    canonical = 4 + len(_CANONICAL_DOMAIN) + len(model_bytes) + 4
    wire_messages = []
    for message in messages:
        if not isinstance(message, dict):
            raise PluginError(common.INVALID_ARGUMENT)
        role = message.get('role')
        text = message.get('text', message.get('content'))
        text_bytes = _utf8_bytes(text)
        if role not in VALID_ROLES or text_bytes is None:
            raise PluginError(common.INVALID_ARGUMENT)
        canonical += 4 + len(role) + 4 + len(text_bytes)
        if canonical > MAX_INPUT_BYTES:
            raise PluginError(common.INVALID_ARGUMENT)
        wire_messages.append(plugins.ModelMessage(role=role, text=text))
    return model, wire_messages


def validate_deadline(timeout) -> float:
    """Return ``timeout`` as a float bounded by the frozen 30-second maximum."""
    if (not isinstance(timeout, (int, float)) or isinstance(timeout, bool)
            or not 0 < timeout <= MAX_DEADLINE_SECONDS):
        raise PluginError(common.INVALID_ARGUMENT)
    return float(timeout)


def _usage_valid(usage) -> bool:
    """``known=false`` requires zero counters; zero then means unknown, not free."""
    return bool(usage.known) or (usage.input_tokens == 0 and usage.output_tokens == 0)


def _valid_error(error) -> bool:
    return bool(error.code) and error.code in common.ErrorCode.values()


def _valid_call_snapshot(call, logical_call_id: str) -> bool:
    """Validate the sanitized call projection carried by started/terminal."""
    if (call.logical_call_id != logical_call_id
            or not valid_uuid4(call.owner_instance_id)
            or not valid_uuid4(call.model_id)
            or not call.config_version or not call.secret_version
            or not call.HasField('usage') or not _usage_valid(call.usage)
            or not call.HasField('may_have_billed')
            or not call.HasField('observed_at') or call.observed_at.seconds <= 0
            or len(call.provider_request_id.encode('utf-8', 'replace')) > 128
            or call.latency_ms > 300000):
        return False
    state, outcome = call.state, call.execution_outcome
    if state == platform.OPERATION_RUNNING:
        return (outcome == platform.EXECUTION_UNKNOWN and call.may_have_billed)
    if state == platform.OPERATION_BLOCKED:
        return (outcome == platform.EXECUTION_UNKNOWN and call.may_have_billed)
    if state == platform.OPERATION_SUCCEEDED:
        return outcome == platform.EXECUTION_KNOWN_SUCCEEDED
    if state == platform.OPERATION_FAILED:
        return outcome in (platform.EXECUTION_KNOWN_FAILED, platform.EXECUTION_UNKNOWN)
    return False


def _failure_error(failure):
    """Convert a wire ``RpcFailure`` into a typed :class:`PluginError`."""
    error = failure.error
    context = failure.context if failure.HasField('context') else None
    return PluginError(
        error.code or common.INTERNAL,
        bool(error.retryable),
        acceptance=context.acceptance if context is not None else None,
        recovery=context.recovery if context is not None else None,
        retry_after_ms=(context.retry_after_ms or None) if context is not None else None,
    )


def _terminal_error(terminal):
    """Build the typed error for a terminal that rejects the start/admission."""
    if terminal.HasField('failure'):
        error = _failure_error(terminal.failure)
    else:
        error = PluginError(common.UPSTREAM_UNAVAILABLE, False)
    error.terminal = terminal
    error.termination = terminal.termination
    error.partial = terminal.partial
    error.may_have_billed = terminal.may_have_billed
    error.usage = terminal.usage
    error.delivery_unknown = False
    return error


PROTOCOL_FAILURE_CAUSES = frozenset({
    'correlation_mismatch', 'started_shape', 'started_call', 'started_disposition',
    'replay_has_deltas', 'delta_sequence', 'delta_text', 'unknown_body',
    'prestart_terminal', 'terminal_sequence', 'terminal_shape', 'malformed_event',
})


def _unknown_delivery_failure(partial: bool, accepted: bool, cause: str) -> PluginError:
    """Local failure for unknown delivery: never fabricated as provider failure.

    A started attempt was durably admitted, so its acceptance is known even
    though delivery is not. Corrupt protocol frames are not retryable; genuine
    transport loss/queue overflow keeps the wire retryable hint (which is still
    not an automatic retry permission).
    """
    error = PluginError(
        common.UPSTREAM_UNAVAILABLE,
        cause not in PROTOCOL_FAILURE_CAUSES,
        acceptance=(platform.DURABLY_ACCEPTED if accepted
                    else platform.ACCEPTANCE_UNKNOWN),
        recovery=platform.QUERY_SAME_INTENT,
        partial=partial,
        delivery_unknown=True,
    )
    error.cause = cause
    return error


def _set_failure(error: PluginError, *, partial_text: str = '', received_bytes: int = 0,
                 received_delta_count: int = 0, logical_call_id: str | None = None):
    """Attach locally observed delivery facts to a typed failure."""
    error.partial = bool(received_bytes)
    error.partial_text = partial_text
    error.received_bytes = received_bytes
    error.received_delta_count = received_delta_count
    error.logical_call_id = logical_call_id
    return error


class StreamRegistry:
    """Per-connection registry of in-flight start attempts.

    Registration happens before the start frame is written. The shared reader
    calls :meth:`dispatch` synchronously: it never awaits, so event delivery can
    not block heartbeat/refresh/Action/ACK processing. Entries are released by
    the owning :class:`ModelStream` context; ``close`` fails every in-flight
    attempt as an unknown delivery when the session ends.
    """

    def __init__(self):
        self._attempts: dict[str, '_Attempt'] = {}
        self._cancel_tasks: set[asyncio.Task] = set()
        self._closed = False
        self.cancel_callback = None  # async (start_request_id, logical_call_id) -> receipt

    def __len__(self) -> int:
        return len(self._attempts)

    def register(self, attempt: '_Attempt') -> None:
        if self._closed:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        if len(self._attempts) >= MAX_ACTIVE_STREAMS:
            # Local mirror of the server's per-connection admission bound.
            raise PluginError(common.RATE_LIMITED, True)
        if attempt.request_id in self._attempts:
            raise PluginError(common.INTERNAL)
        self._attempts[attempt.request_id] = attempt

    def find(self, request_id: str) -> '_Attempt | None':
        return self._attempts.get(request_id)

    def release(self, attempt: '_Attempt') -> None:
        attempt.released = True
        self._attempts.pop(attempt.request_id, None)

    def dispatch(self, event) -> bool:
        """Route one ``ModelStreamEvent`` to its attempt without blocking."""
        attempt = self._attempts.get(event.request_id)
        if attempt is None:
            # A late event for an already released attempt (for example after a
            # best-effort cancel) carries no authority; drop it silently.
            return False
        return attempt.deliver(event)

    def request_cancel(self, attempt: '_Attempt') -> None:
        """Schedule one best-effort cancel for a locally terminated attempt."""
        if attempt.cancel_requested or self._closed or self.cancel_callback is None:
            return
        attempt.cancel_requested = True
        task = asyncio.create_task(self._run_cancel(attempt))
        self._cancel_tasks.add(task)
        task.add_done_callback(self._cancel_tasks.discard)

    async def _run_cancel(self, attempt: '_Attempt') -> None:
        try:
            attempt.cancel_receipt = await self.cancel_callback(
                attempt.request_id, attempt.logical_call_id)
        except Exception:  # noqa: BLE001 - best effort: the stream is already unknown locally
            return

    async def close(self) -> None:
        """Fail every in-flight attempt as unknown delivery and join cancels."""
        self._closed = True
        for attempt in list(self._attempts.values()):
            attempt.fail_unknown('connection_closed', close=False)
        self._attempts.clear()
        tasks = list(self._cancel_tasks)
        for task in tasks:
            task.cancel()
        for task in tasks:
            with suppress(asyncio.CancelledError, Exception):
                await task
        self._cancel_tasks.clear()


class _Attempt:
    """Mutable delivery state for one start attempt.

    A single consumer awaits deltas through :meth:`next_delta`; the shared
    reader mutates this object from the same event loop without awaiting.
    """

    def __init__(self, request_id: str, logical_call_id: str, trace_id: str,
                 timeout: float):
        loop = asyncio.get_running_loop()
        self.request_id = request_id
        self.logical_call_id = logical_call_id
        self.trace_id = trace_id
        self.timeout = timeout
        self.admission: asyncio.Future = loop.create_future()
        self.changed = asyncio.Event()
        self.deltas: deque = deque()
        self.queued_bytes = 0
        self.started = None
        self.terminal = None
        self.failure: PluginError | None = None
        self.partial_parts: list[str] = []
        self.received_bytes = 0
        self.received_delta_count = 0
        self.last_sequence = -1
        self.delivery_open = False
        self.cancel_requested = False
        self.cancel_receipt = None
        self.protocol_anomaly = False
        self.released = False
        self._finished = False

    # -- admission ---------------------------------------------------------
    def admit(self, outcome: str) -> None:
        if not self.admission.done():
            self.admission.set_result(outcome)

    def wait_admission(self):
        return self.admission

    def reject_admission(self, error: PluginError) -> None:
        if not self.admission.done():
            self.admission.set_exception(error)
        self.failure = error
        self.changed.set()

    # -- shared-reader dispatch (must not await) ---------------------------
    def deliver(self, event) -> bool:
        try:
            body = event.WhichOneof('body')
            if self._finished or self.failure is not None:
                # Post-terminal traffic is a server anomaly, not an authority
                # that can overwrite the single frozen terminal.
                self.protocol_anomaly = True
                return False
            if (event.request_id != self.request_id
                    or event.logical_call_id != self.logical_call_id
                    or event.trace_id != self.trace_id):
                self.fail_unknown('correlation_mismatch')
                return False
            if body == 'started':
                if (self.started is not None or self.last_sequence != -1
                        or event.sequence != 0
                        or not event.started.HasField('call')
                        or event.started.disposition not in _DISPOSITIONS):
                    self.fail_unknown('started_shape')
                    return False
                if not _valid_call_snapshot(event.started.call, self.logical_call_id):
                    self.fail_unknown('started_call')
                    return False
                disposition = event.started.disposition
                started_state = event.started.call.state
                if ((disposition == DISPOSITION_REPLAY_TERMINAL)
                        == (started_state == platform.OPERATION_RUNNING)):
                    self.fail_unknown('started_disposition')
                    return False
                if (disposition != DISPOSITION_FRESH and self.received_delta_count):
                    self.fail_unknown('replay_has_deltas')
                    return False
                self.started = event.started
                self.last_sequence = event.sequence
                self.delivery_open = True
                self.admit('started')
                self.changed.set()
                return True
            if body == 'delta':
                if (self.started is None or self.last_sequence != event.sequence - 1
                        or event.sequence == 0):
                    self.fail_unknown('delta_sequence')
                    return False
                if self.started.disposition != DISPOSITION_FRESH:
                    self.fail_unknown('replay_has_deltas')
                    return False
                text = event.delta.text
                text_bytes = _utf8_bytes(text)
                if (text_bytes is None or not text_bytes
                        or len(text_bytes) > MAX_DELTA_BYTES
                        or self.received_bytes + len(text_bytes) > MAX_RESULT_BYTES):
                    self.fail_unknown('delta_text')
                    return False
                if (len(self.deltas) >= MAX_QUEUED_DELTAS
                        or self.queued_bytes + len(text_bytes) > MAX_QUEUED_BYTES):
                    # The shared reader must never wait for this consumer.
                    self.fail_unknown('queue_overflow')
                    return False
                self.deltas.append(event.delta)
                self.queued_bytes += len(text_bytes)
                self.received_bytes += len(text_bytes)
                self.received_delta_count += 1
                self.partial_parts.append(text)
                self.last_sequence = event.sequence
                self.changed.set()
                return True
            if body == 'terminal':
                return self._deliver_terminal(event)
            self.fail_unknown('unknown_body')
            return False
        except Exception:  # noqa: BLE001 - a malformed frame must not kill the reader
            self.fail_unknown('malformed_event')
            return False

    def _deliver_terminal(self, event) -> bool:
        terminal = event.terminal
        expected = self.last_sequence + 1
        if not self.started:
            # A pre-start rejection/unknown admission has only sequence=0 and
            # may have no call snapshot.
            if (event.sequence != 0 or self.last_sequence != -1
                    or terminal.termination not in (TERMINATION_FAILED, TERMINATION_CANCELLED)):
                self.fail_unknown('prestart_terminal')
                return False
        elif event.sequence != expected:
            self.fail_unknown('terminal_sequence')
            return False
        if not self._terminal_consistent(terminal):
            self.fail_unknown('terminal_shape')
            return False
        self.terminal = terminal
        self.last_sequence = event.sequence
        self.delivery_open = False
        self._finished = True
        if not self.started:
            # The start itself was rejected: end the pending admission with a
            # typed outcome instead of waiting for events that will never come.
            if not self.admission.done():
                self.admission.set_exception(_terminal_error(terminal))
            self.changed.set()
            return True
        self.admit('terminal')
        self.changed.set()
        return True

    def _terminal_consistent(self, terminal) -> bool:
        termination = terminal.termination
        if termination not in _TERMINATIONS:
            return False
        if not terminal.HasField('usage') or not _usage_valid(terminal.usage):
            return False
        if not terminal.HasField('may_have_billed'):
            return False
        if terminal.finish_reason not in _FINISH_REASONS:
            return False
        if (terminal.emitted_bytes != self.received_bytes
                or terminal.emitted_delta_count != self.received_delta_count):
            return False
        if terminal.partial != (self.received_bytes != 0
                                and termination != TERMINATION_COMPLETED):
            return False
        failed = termination == TERMINATION_FAILED
        if failed != terminal.HasField('failure'):
            return False
        if terminal.HasField('failure') and not _valid_error(terminal.failure.error):
            return False
        replay = bool(self.started) and self.started.disposition != DISPOSITION_FRESH
        if replay != (termination == TERMINATION_REPLAY):
            return False
        if replay:
            if (self.received_delta_count or not terminal.HasField('call')
                    or not _valid_call_snapshot(terminal.call, self.logical_call_id)
                    or terminal.call != self.started.call):
                return False
        if terminal.HasField('call'):
            if not _valid_call_snapshot(terminal.call, self.logical_call_id):
                return False
            if (terminal.call.usage != terminal.usage
                    or terminal.call.may_have_billed != terminal.may_have_billed):
                return False
            if self.started is not None:
                pinned = self.started.call
                if (terminal.call.owner_instance_id != pinned.owner_instance_id
                        or terminal.call.model_id != pinned.model_id
                        or terminal.call.config_version != pinned.config_version
                        or terminal.call.secret_version != pinned.secret_version):
                    return False
        if termination in (TERMINATION_COMPLETED, TERMINATION_TRUNCATED):
            if not terminal.HasField('call'):
                return False
            if terminal.call.state != platform.OPERATION_SUCCEEDED:
                return False
            if (termination == TERMINATION_COMPLETED
                    and terminal.finish_reason != FINISH_STOP):
                return False
            if (termination == TERMINATION_TRUNCATED
                    and terminal.finish_reason not in (FINISH_LENGTH, FINISH_CONTENT_FILTER)):
                return False
        return True

    def fail_unknown(self, cause: str, close: bool = True) -> None:
        """Locally terminate delivery as unknown; never fabricate a terminal."""
        if self.failure is not None:
            return
        accepted = self.started is not None
        error = _set_failure(
            _unknown_delivery_failure(bool(self.received_bytes), accepted, cause),
            partial_text=''.join(self.partial_parts),
            received_bytes=self.received_bytes,
            received_delta_count=self.received_delta_count,
            logical_call_id=self.logical_call_id)
        error.termination = None
        self.failure = error
        self.delivery_open = False
        if not self.admission.done():
            self.admission.set_exception(error)
        self.changed.set()
        if close:
            registry = getattr(self, 'registry', None)
            if registry is not None:
                registry.request_cancel(self)

    # -- consumer ----------------------------------------------------------
    def pending(self) -> bool:
        return bool(self.deltas) or self.terminal is not None or self.failure is not None

    def pop_delta(self):
        delta = self.deltas.popleft()
        self.queued_bytes -= len(delta.text.encode('utf-8'))
        return delta

    async def next_delta(self):
        """Await the next queued delta; return None at terminal/failure."""
        while True:
            if self.deltas:
                return self.pop_delta()
            if self.terminal is not None or self.failure is not None:
                return None
            self.changed.clear()
            if self.deltas or self.terminal is not None or self.failure is not None:
                continue
            await self.changed.wait()


class ModelStream:
    """One model text stream attempt.

    Use it as an async context manager and iterate the typed deltas::

        async with context.model_stream('default', messages) as stream:
            async for delta in stream:
                chunks.append(delta.text)
        outcome = stream.termination

    The object registers its attempt *before* the start frame is sent. Leaving
    the context while the attempt is not terminal sends one best-effort cancel
    and always releases the registry entry. A locally unknown delivery (queue
    overflow, protocol violation, disconnect, ...) raises ``PluginError`` with
    ``delivery_unknown=True`` and the locally received partial text preserved;
    it never turns into a fabricated ``COMPLETED``.
    """

    def __init__(self, rpc, model, messages, *, logical_call_id=None,
                 timeout=DEFAULT_DEADLINE_SECONDS):
        self._rpc = rpc
        self._model, self._messages = prepare_messages(model, messages)
        self._timeout = validate_deadline(timeout)
        if logical_call_id is None:
            logical_call_id = new_uuid4()
        elif not valid_uuid4(logical_call_id):
            raise PluginError(common.INVALID_ARGUMENT)
        self.logical_call_id = logical_call_id
        self.request_id = new_uuid4()
        self.trace_id = new_uuid4()
        self._attempt = _Attempt(self.request_id, self.logical_call_id, self.trace_id,
                                 self._timeout)
        self._attempt.registry = getattr(rpc, 'streams', None)
        self._entered = False
        self._released = False
        self._deadline = None

    # -- identity/outcome accessors ---------------------------------------
    @property
    def started(self):
        return self._attempt.started

    @property
    def terminal(self):
        return self._attempt.terminal

    @property
    def termination(self):
        return self._attempt.terminal.termination if self._attempt.terminal else None

    @property
    def finish_reason(self):
        return self._attempt.terminal.finish_reason if self._attempt.terminal else None

    @property
    def completed(self) -> bool:
        return self.termination == TERMINATION_COMPLETED

    @property
    def partial(self) -> bool:
        if self._attempt.terminal is not None:
            return self._attempt.terminal.partial
        return bool(self._attempt.received_bytes)

    @property
    def partial_text(self) -> str:
        """Locally received text. Not an acknowledgement of durable state."""
        return ''.join(self._attempt.partial_parts)

    @property
    def received_bytes(self) -> int:
        return self._attempt.received_bytes

    @property
    def received_delta_count(self) -> int:
        return self._attempt.received_delta_count

    @property
    def usage(self):
        return self._attempt.terminal.usage if self._attempt.terminal else None

    @property
    def may_have_billed(self):
        if self._attempt.terminal is not None:
            return self._attempt.terminal.may_have_billed
        return None

    @property
    def failure(self):
        return self._attempt.failure

    @property
    def delivery_unknown(self) -> bool:
        return self._attempt.failure is not None

    @property
    def cancel_receipt(self):
        return self._attempt.cancel_receipt

    @property
    def protocol_anomaly(self) -> bool:
        return self._attempt.protocol_anomaly

    @property
    def closed(self) -> bool:
        return self._released

    # -- context manager ---------------------------------------------------
    async def __aenter__(self) -> 'ModelStream':
        if self._entered:
            raise PluginError(common.INVALID_ARGUMENT)
        self._entered = True
        rpc = self._rpc
        if rpc is None or getattr(rpc, 'closed', True):
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        registry = getattr(rpc, 'streams', None)
        if registry is None:
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        try:
            registry.register(self._attempt)
        except PluginError:
            self._entered = False
            raise
        self._deadline = datetime.now(timezone.utc) + timedelta(seconds=self._timeout)
        try:
            await rpc.start_stream(self._start_request(), self._attempt)
        except BaseException as error:
            # Includes asyncio cancellation: never leave a half-registered
            # attempt behind, and never claim provider non-execution.
            try:
                await self._cancel_best_effort()
            finally:
                self._release()
            raise error
        if self._attempt.terminal is not None and self._attempt.started is None:
            # A pre-start rejection ends the start with a typed outcome; it is
            # not a successfully opened stream.
            error = _terminal_error(self._attempt.terminal)
            _set_failure(error, partial_text='', received_bytes=0,
                         received_delta_count=0, logical_call_id=self.logical_call_id)
            self._release()
            raise error
        if self._attempt.failure is not None:
            error = self._attempt.failure
            self._release()
            raise error
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> bool:
        if issubclass(exc_type, asyncio.CancelledError) if exc_type is not None else False:
            self._attempt.consumer_cancelled = True
        try:
            if not self._released and self._attempt.terminal is None:
                await self._cancel_best_effort()
        finally:
            self._release()
        return False

    def __aiter__(self) -> 'ModelStream':
        return self

    async def __anext__(self):
        if not self._entered:
            raise PluginError(common.INVALID_ARGUMENT)
        while True:
            delta = await self._attempt.next_delta()
            if delta is not None:
                return delta
            if self._attempt.failure is not None:
                error = self._attempt.failure
                self._release()
                raise error
            self._release()
            raise StopAsyncIteration

    # -- cancellation ------------------------------------------------------
    async def cancel(self, timeout: float = DEFAULT_CANCEL_TIMEOUT):
        """Request cancellation and return the typed receipt.

        The receipt acknowledges local cancellation handling only; the attempt
        terminal is still authoritative for the outcome and billing remains
        unknown until an authoritative fact exists.
        """
        if self._released:
            raise PluginError(common.INVALID_ARGUMENT)
        if (not isinstance(timeout, (int, float)) or isinstance(timeout, bool)
                or not 0 < timeout <= MAX_DEADLINE_SECONDS):
            raise PluginError(common.INVALID_ARGUMENT)
        rpc = self._rpc
        if rpc is None or getattr(rpc, 'closed', True):
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        receipt = await rpc.cancel_stream(self.request_id, self.logical_call_id, float(timeout))
        if (receipt.start_request_id != self.request_id
                or receipt.logical_call_id != self.logical_call_id
                or receipt.disposition not in _CANCEL_DISPOSITIONS):
            raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
        self._attempt.cancel_requested = True
        self._attempt.cancel_receipt = receipt
        return receipt

    async def aclose(self) -> None:
        """Release the attempt; cancel best-effort when not terminal."""
        if self._released:
            return
        try:
            if self._attempt.terminal is None:
                await self._cancel_best_effort()
        finally:
            self._release()

    # -- internals ---------------------------------------------------------
    def _start_request(self) -> plugins.HostRequest:
        request = plugins.HostRequest(
            request_id=self.request_id,
            trace_id=self.trace_id,
            idempotency_key='',
            model_stream_start=plugins.ModelStreamStartRequest(
                model=self._model,
                messages=self._messages,
                logical_call_id=self.logical_call_id))
        request.deadline.FromDatetime(self._deadline)
        return request

    def _release(self) -> None:
        if self._released:
            return
        self._released = True
        registry = getattr(self._rpc, 'streams', None)
        if registry is not None:
            registry.release(self._attempt)

    async def _cancel_best_effort(self) -> None:
        attempt = self._attempt
        if attempt.cancel_requested or attempt.terminal is not None:
            return
        rpc = self._rpc
        if rpc is None or getattr(rpc, 'closed', True):
            return
        attempt.cancel_requested = True
        try:
            receipt = await asyncio.wait_for(
                rpc.cancel_stream(self.request_id, self.logical_call_id,
                                  DEFAULT_CANCEL_TIMEOUT),
                timeout=DEFAULT_CANCEL_TIMEOUT)
            attempt.cancel_receipt = receipt
        except asyncio.CancelledError:
            raise
        except Exception:  # noqa: BLE001 - best effort; the outcome stays unknown
            return
