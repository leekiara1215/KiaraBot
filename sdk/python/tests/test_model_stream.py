"""STR-08 Python SDK async model stream contract tests.

Two layers:

* unit tests drive a real :class:`kiarabot_sdk.runtime._Rpc` with a captured
  write sink and dispatch generated ``ModelStreamEvent`` frames directly (no
  gRPC). They cover correlation/sequence validation, bounded queues, typed
  local unknown delivery, cancellation receipts and registry release;
* integration tests run the real :class:`Runner` + ``FakeTargetRuntime`` TLS
  gRPC fixture so the shared reader path, pre-start HostResponse rejection,
  connection teardown and cancel through the real writer are exercised.

No test talks to a real provider, database or QQ.
"""

import asyncio
import uuid
from collections import namedtuple
from contextlib import suppress

import pytest
from kiarabot.v1 import common_pb2 as common, model_stream_pb2 as streaming
from kiarabot.v1 import platform_pb2 as platform, plugins_pb2 as plugins
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk import model_stream as stream_api
from kiarabot_sdk.runtime import Context, _Rpc
from kiarabot_sdk.testing import FakeTargetRuntime
from test_runtime import manifest, options

Ids = namedtuple('Ids', 'request_id logical_call_id trace_id')
Messages = [{'role': 'user', 'text': 'hello'}]
FRESH = streaming.MODEL_STREAM_FRESH
REPLAY_IN_PROGRESS = streaming.MODEL_STREAM_REPLAY_IN_PROGRESS
REPLAY_TERMINAL = streaming.MODEL_STREAM_REPLAY_TERMINAL
COMPLETED = streaming.MODEL_STREAM_COMPLETED
TRUNCATED = streaming.MODEL_STREAM_TRUNCATED
FAILED = streaming.MODEL_STREAM_FAILED
CANCELLED = streaming.MODEL_STREAM_CANCELLED
REPLAY = streaming.MODEL_STREAM_REPLAY
FINISH_STOP = streaming.MODEL_STREAM_FINISH_STOP
FINISH_LENGTH = streaming.MODEL_STREAM_FINISH_LENGTH
FINISH_UNSPECIFIED = streaming.MODEL_STREAM_FINISH_REASON_UNSPECIFIED


# -- generated-frame helpers (valid shapes unless a test overrides one) ----

def ids(source) -> Ids:
    """Accept a ModelStream or a plugins.HostRequest and return attempt ids."""
    if isinstance(source, plugins.HostRequest):
        return Ids(source.request_id, source.model_stream_start.logical_call_id,
                   source.trace_id)
    return Ids(source.request_id, source.logical_call_id, source.trace_id)


def call_snapshot(logical_call_id, *, state=platform.OPERATION_RUNNING, usage_known=False,
                  input_tokens=0, output_tokens=0, may_have_billed=True, owner=None,
                  model=None, config_version=1, secret_version=1):
    outcome = {platform.OPERATION_RUNNING: platform.EXECUTION_UNKNOWN,
               platform.OPERATION_BLOCKED: platform.EXECUTION_UNKNOWN,
               platform.OPERATION_SUCCEEDED: platform.EXECUTION_KNOWN_SUCCEEDED,
               platform.OPERATION_FAILED: platform.EXECUTION_KNOWN_FAILED}[state]
    call = streaming.ModelStreamCallSnapshot(
        logical_call_id=logical_call_id,
        owner_instance_id=owner or str(uuid.uuid4()),
        model_id=model or str(uuid.uuid4()),
        config_version=config_version,
        secret_version=secret_version,
        state=state,
        execution_outcome=outcome,
        may_have_billed=may_have_billed)
    # unknown usage requires zero counters by contract
    call.usage.known = usage_known
    call.usage.input_tokens = input_tokens if usage_known else 0
    call.usage.output_tokens = output_tokens if usage_known else 0
    call.observed_at.GetCurrentTime()
    return call


def failure(code, *, retryable=True, acceptance=platform.NOT_ACCEPTED,
            recovery=platform.CORRECT_INPUT):
    result = platform.RpcFailure()
    result.error.code = code
    result.error.retryable = retryable
    result.context.acceptance = acceptance
    result.context.recovery = recovery
    return result


def _event(idsvalue, sequence):
    return streaming.ModelStreamEvent(
        request_id=idsvalue.request_id,
        logical_call_id=idsvalue.logical_call_id,
        trace_id=idsvalue.trace_id,
        sequence=sequence)


def started_event(source, *, disposition=FRESH, call=None):
    call = call if call is not None else call_snapshot(ids(source).logical_call_id)
    event = _event(ids(source), 0)
    event.started.disposition = disposition
    event.started.call.CopyFrom(call)
    return event


def delta_event(source, text, sequence):
    event = _event(ids(source), sequence)
    event.delta.text = text
    return event


def terminal_event(source, termination, *, sequence=None, call=None, failure_value=None,
                   finish_reason=FINISH_UNSPECIFIED, partial=None, emitted_bytes=None,
                   emitted_delta_count=None, usage_known=True, input_tokens=3,
                   output_tokens=4, may_have_billed=False):
    """Build a terminal that matches the deltas already received by ``source``."""
    event = _event(ids(source), sequence if sequence is not None
                   else _next_sequence(source))
    terminal = event.terminal
    terminal.termination = termination
    terminal.emitted_bytes = (getattr(source, 'received_bytes', 0)
                              if emitted_bytes is None else emitted_bytes)
    terminal.emitted_delta_count = (getattr(source, 'received_delta_count', 0)
                                    if emitted_delta_count is None else emitted_delta_count)
    if partial is None:
        partial = terminal.emitted_bytes != 0 and termination != COMPLETED
    terminal.partial = partial
    terminal.finish_reason = finish_reason
    terminal.usage.known = usage_known
    terminal.usage.input_tokens = input_tokens if usage_known else 0
    terminal.usage.output_tokens = output_tokens if usage_known else 0
    terminal.may_have_billed = may_have_billed
    if failure_value is not None:
        terminal.failure.CopyFrom(failure_value)
    if call is not None:
        terminal.call.CopyFrom(call)
    return event


def _next_sequence(source):
    if isinstance(source, plugins.HostRequest):
        return 1
    return source._attempt.last_sequence + 1


def succeeded_call(logical_call_id, *, input_tokens=3, output_tokens=4,
                   may_have_billed=False):
    return call_snapshot(logical_call_id, state=platform.OPERATION_SUCCEEDED,
                         usage_known=True, input_tokens=input_tokens,
                         output_tokens=output_tokens,
                         may_have_billed=may_have_billed)


def matching_call(bridge, stream, *, state=platform.OPERATION_SUCCEEDED,
                  usage_known=True, input_tokens=3, output_tokens=4,
                  may_have_billed=False):
    """Rebuild the terminal call snapshot with the started attempt's pinned identity."""
    pinned = bridge.started_calls[stream.logical_call_id]
    return call_snapshot(stream.logical_call_id, state=state, usage_known=usage_known,
                         input_tokens=input_tokens, output_tokens=output_tokens,
                         may_have_billed=may_have_billed,
                         owner=pinned.owner_instance_id, model=pinned.model_id,
                         config_version=pinned.config_version,
                         secret_version=pinned.secret_version)


def complete_stream(bridge, stream, *, input_tokens=3, output_tokens=4,
                    may_have_billed=False):
    call = matching_call(bridge, stream, input_tokens=input_tokens,
                         output_tokens=output_tokens,
                         may_have_billed=may_have_billed)
    terminal = terminal_event(
        stream, COMPLETED, call=call, finish_reason=FINISH_STOP,
        usage_known=True, input_tokens=input_tokens, output_tokens=output_tokens,
        may_have_billed=may_have_billed)
    assert bridge.rpc.streams.dispatch(terminal)
    return terminal


async def until(predicate, timeout=3.0):
    """Spin the event loop until ``predicate()`` is true (no implicit sleep)."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while not predicate():
        if loop.time() >= deadline:
            raise AssertionError('condition was not reached in time')
        await asyncio.sleep(0)


async def drain(stream):
    seen = []
    async for delta in stream:
        seen.append(delta.text)
    return seen


# -- unit fixture: real _Rpc plus a captured write sink ---------------------

class UnitBridge:
    """Capture frames written by ``_Rpc`` and answer cancel receipts locally."""

    def __init__(self):
        self.frames = []
        self.cancel_requests = []
        self.cancel_disposition = streaming.MODEL_STREAM_CANCELLATION_REQUESTED
        self.registry_size_at_first_write = None
        self.started_calls = {}
        self.rpc = _Rpc(self._write, {}, {})

    async def _write(self, frame):
        self.frames.append(frame)
        if self.registry_size_at_first_write is None:
            self.registry_size_at_first_write = len(self.rpc.streams)
        if frame.WhichOneof('body') != 'host_request':
            return
        request = frame.host_request
        if request.WhichOneof('operation') != 'model_stream_cancel':
            return
        cancel = request.model_stream_cancel
        self.cancel_requests.append((cancel.start_request_id, cancel.logical_call_id))
        # A real Runtime answers cancel with one HostResponse receipt; mirror it
        # so the SDK exercises the same one-answer path without gRPC.
        waiter = self.rpc._pending.get(request.request_id)
        if waiter is not None and not waiter.done():
            waiter.set_result(plugins.HostResponse(
                request_id=request.request_id,
                model_stream_cancel=streaming.ModelStreamCancelResponse(
                    start_request_id=cancel.start_request_id,
                    logical_call_id=cancel.logical_call_id,
                    disposition=self.cancel_disposition)))

    def context(self, *, features=('host.model.stream.v1',),
                permissions=('model.chat',)):
        result = Context()
        result._rpc = self.rpc
        result._features = frozenset(features)
        result._permissions = frozenset(permissions)
        return result


async def open_stream(bridge, *, logical_call_id=None, timeout=30.0):
    """Enter one stream and feed the FRESH started event so admission resolves."""
    context = bridge.context()
    stream = context.model_stream('default', Messages, logical_call_id=logical_call_id,
                                  timeout=timeout)
    entering = asyncio.create_task(stream.__aenter__())
    await until(lambda: any(frame.WhichOneof('body') == 'host_request'
                            and frame.host_request.request_id == stream.request_id
                            for frame in bridge.frames))
    call = call_snapshot(stream.logical_call_id)
    bridge.started_calls[stream.logical_call_id] = call
    assert bridge.rpc.streams.dispatch(started_event(stream, call=call))
    await asyncio.wait_for(entering, 2)
    return stream


# -- feature/permission/input preflight ------------------------------------

async def test_model_stream_requires_negotiated_feature_and_permission():
    bridge = UnitBridge()
    with pytest.raises(PluginError) as unsupported:
        bridge.context(features=('host.model.v1',)).model_stream('default', Messages)
    assert unsupported.value.code == common.CAPABILITY_NOT_SUPPORTED

    with pytest.raises(PluginError) as denied:
        bridge.context(permissions=()).model_stream('default', Messages)
    assert denied.value.code == common.PERMISSION_DENIED

    context = Context()
    with pytest.raises(PluginError) as unavailable:
        context.model_stream('default', Messages)
    assert unavailable.value.code == common.UPSTREAM_UNAVAILABLE
    assert unavailable.value.retryable is True
    assert bridge.frames == []


@pytest.mark.parametrize('kwargs', [
    {'logical_call_id': 'not-a-uuid'},
    {'logical_call_id': str(uuid.uuid4()).upper()},
    {'logical_call_id': str(uuid.uuid1())},
    {'timeout': 0},
    {'timeout': 30.5},
    {'timeout': True},
])
async def test_rejects_invalid_stream_identity_and_deadline(kwargs):
    bridge = UnitBridge()
    with pytest.raises(PluginError) as failure_value:
        bridge.context().model_stream('default', Messages, **kwargs)
    assert failure_value.value.code == common.INVALID_ARGUMENT
    assert bridge.frames == []


@pytest.mark.parametrize('model,messages', [
    ('', Messages),
    ('m' * 257, Messages),
    ('default', []),
    ('default', [{'role': 'tool', 'text': 'x'}]),
    ('default', [{'role': 'user', 'text': None}]),
    ('default', [{'role': 'user', 'text': 'a\x00b'}]),
    ('default', [{'role': 'user', 'text': 'x'}] * 129),
    ('default', [{'role': 'system', 'text': 's' * 65536}]),
])
async def test_rejects_invalid_stream_input(model, messages):
    bridge = UnitBridge()
    with pytest.raises(PluginError) as failure_value:
        bridge.context().model_stream(model, messages)
    assert failure_value.value.code == common.INVALID_ARGUMENT
    assert bridge.frames == []


async def test_combined_canonical_input_is_bounded_by_64_kib():
    bridge = UnitBridge()
    messages = [{'role': 'user', 'text': 'x' * 32768},
                {'role': 'user', 'text': 'y' * 32768}]
    with pytest.raises(PluginError) as failure_value:
        bridge.context().model_stream('default', messages)
    assert failure_value.value.code == common.INVALID_ARGUMENT


async def test_old_model_chat_stream_flag_still_points_at_new_api():
    async def rpc(frame, timeout):
        raise AssertionError('stream=True must not reach the runtime')

    context = Context()
    context._rpc = rpc
    context._features = frozenset({'host.model.v1'})
    with pytest.raises(PluginError) as rejected:
        await context.model_chat('default', Messages, stream=True)
    assert rejected.value.code == common.CAPABILITY_NOT_SUPPORTED


# -- start registration / wire shape ---------------------------------------

async def test_start_is_registered_before_send_and_uses_attempt_identity():
    bridge = UnitBridge()
    call_id = str(uuid.uuid4())
    stream = await open_stream(bridge, logical_call_id=call_id)
    assert bridge.registry_size_at_first_write == 1
    frame = bridge.frames[0]
    assert frame.WhichOneof('body') == 'host_request'
    request = frame.host_request
    start = request.model_stream_start
    assert start.model == 'default'
    assert list(start.messages) == [plugins.ModelMessage(role='user', text='hello')]
    assert start.logical_call_id == call_id == stream.logical_call_id
    assert request.idempotency_key == ''
    assert request.HasField('deadline')
    for value in (stream.request_id, stream.trace_id, stream.logical_call_id):
        assert str(uuid.UUID(value)) == value
    assert len({stream.request_id, stream.trace_id, stream.logical_call_id}) == 3
    await stream.aclose()


async def test_new_attempt_reuses_explicit_logical_call_id():
    bridge = UnitBridge()
    call_id = str(uuid.uuid4())
    first = await open_stream(bridge, logical_call_id=call_id)
    first_id = first.request_id
    complete_stream(bridge, first)
    assert await drain(first) == []
    await first.aclose()
    second = await open_stream(bridge, logical_call_id=call_id)
    assert second.logical_call_id == call_id
    assert second.request_id != first_id
    await second.aclose()


# -- ordered delivery and typed terminal -----------------------------------

async def test_completed_stream_exposes_terminal_usage_and_partial_text():
    bridge = UnitBridge()
    stream = await open_stream(bridge)
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'Hello', 1))
    assert bridge.rpc.streams.dispatch(delta_event(stream, ' 世界', 2))
    complete_stream(bridge, stream, input_tokens=3, output_tokens=4)
    assert await drain(stream) == ['Hello', ' 世界']
    await stream.aclose()
    assert stream.completed is True
    assert stream.termination == COMPLETED
    assert stream.started.disposition == FRESH
    assert stream.terminal.usage.known is True
    assert stream.terminal.usage.output_tokens == 4
    assert stream.may_have_billed is False
    assert stream.partial is False
    assert stream.partial_text == 'Hello 世界'
    assert stream.received_bytes == len('Hello 世界'.encode('utf-8'))
    assert stream.received_delta_count == 2
    assert stream.failure is None
    assert bridge.cancel_requests == []
    assert len(bridge.rpc.streams) == 0
    assert bridge.rpc._pending == {} and bridge.rpc._expected == {}


async def test_truncated_terminal_keeps_partial_and_known_billing():
    bridge = UnitBridge()
    stream = await open_stream(bridge)
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'half', 1))
    truncated = terminal_event(
        stream, TRUNCATED, call=matching_call(bridge, stream, input_tokens=2,
                                              output_tokens=1,
                                              may_have_billed=True),
        finish_reason=FINISH_LENGTH, usage_known=True, input_tokens=2,
        output_tokens=1, may_have_billed=True)
    assert bridge.rpc.streams.dispatch(truncated)
    assert await drain(stream) == ['half']
    await stream.aclose()
    assert stream.completed is False
    assert stream.termination == TRUNCATED
    assert stream.partial is True
    assert stream.partial_text == 'half'
    assert stream.may_have_billed is True


async def test_terminal_without_any_delta_is_not_partial_but_not_free():
    bridge = UnitBridge()
    stream = await open_stream(bridge)
    failed = terminal_event(
        stream, FAILED,
        call=matching_call(bridge, stream, state=platform.OPERATION_BLOCKED,
                           usage_known=False, may_have_billed=True),
        failure_value=failure(common.UPSTREAM_UNAVAILABLE),
        may_have_billed=True, usage_known=False)
    assert bridge.rpc.streams.dispatch(failed)
    assert await drain(stream) == []
    await stream.aclose()
    assert stream.termination == FAILED
    assert stream.terminal.failure.error.code == common.UPSTREAM_UNAVAILABLE
    assert stream.terminal.may_have_billed is True
    assert stream.terminal.usage.known is False
    assert stream.partial is False
    assert stream.usage.input_tokens == 0


async def test_replay_is_started_plus_replay_terminal_with_no_delta():
    bridge = UnitBridge()
    logical_call_id = str(uuid.uuid4())
    call = succeeded_call(logical_call_id, input_tokens=7, output_tokens=8)
    stream = bridge.context().model_stream('default', Messages,
                                           logical_call_id=logical_call_id)
    entering = asyncio.create_task(stream.__aenter__())
    await until(lambda: any(frame.WhichOneof('body') == 'host_request'
                            and frame.host_request.request_id == stream.request_id
                            for frame in bridge.frames))
    assert bridge.rpc.streams.dispatch(
        started_event(stream, disposition=REPLAY_TERMINAL, call=call))
    await asyncio.wait_for(entering, 2)
    replay_terminal = terminal_event(stream, REPLAY, call=call, usage_known=True,
                                     input_tokens=7, output_tokens=8)
    assert bridge.rpc.streams.dispatch(replay_terminal)
    assert await drain(stream) == []
    await stream.aclose()
    assert stream.completed is False
    assert stream.termination == REPLAY
    assert stream.terminal.call.state == platform.OPERATION_SUCCEEDED
    assert stream.partial is False
    assert stream.partial_text == ''


async def test_replay_attempt_rejects_delta():
    bridge = UnitBridge()
    logical_call_id = str(uuid.uuid4())
    stream = bridge.context().model_stream('default', Messages,
                                           logical_call_id=logical_call_id)
    entering = asyncio.create_task(stream.__aenter__())
    await until(lambda: any(frame.WhichOneof('body') == 'host_request'
                            and frame.host_request.request_id == stream.request_id
                            for frame in bridge.frames))
    # REPLAY_IN_PROGRESS means the pinned call is still running.
    assert bridge.rpc.streams.dispatch(
        started_event(stream, disposition=REPLAY_IN_PROGRESS,
                      call=call_snapshot(logical_call_id)))
    await asyncio.wait_for(entering, 2)
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'replayed', 1)) is False
    with pytest.raises(PluginError) as rejected:
        await drain(stream)
    assert rejected.value.delivery_unknown is True
    assert rejected.value.code == common.UPSTREAM_UNAVAILABLE
    assert stream.terminal is None
    assert stream.failure.cause == 'replay_has_deltas'


# -- protocol violations terminate locally as unknown delivery -------------

@pytest.mark.parametrize('corrupt', [
    'gap', 'duplicate', 'delta_before_started', 'unknown_body', 'correlation',
    'started_twice', 'bad_terminal_counts', 'failed_without_failure',
])
async def test_protocol_violations_terminate_unknown_but_keep_partial(corrupt):
    bridge = UnitBridge()
    stream = bridge.context().model_stream('default', Messages)
    entering = asyncio.create_task(stream.__aenter__())
    await until(lambda: any(frame.WhichOneof('body') == 'host_request'
                            and frame.host_request.request_id == stream.request_id
                            for frame in bridge.frames))
    if corrupt in ('delta_before_started', 'unknown_body', 'correlation'):
        if corrupt == 'delta_before_started':
            frame = delta_event(stream, 'early', 1)
        elif corrupt == 'unknown_body':
            frame = _event(ids(stream), 0)
        else:
            frame = started_event(stream)
            frame.logical_call_id = str(uuid.uuid4())
        assert bridge.rpc.streams.dispatch(frame) is False
        with pytest.raises(PluginError) as failure_value:
            await asyncio.wait_for(entering, 2)
        assert failure_value.value.delivery_unknown is True
        assert failure_value.value.code == common.UPSTREAM_UNAVAILABLE
        assert len(bridge.rpc.streams) == 0
        return
    assert bridge.rpc.streams.dispatch(started_event(stream))
    await asyncio.wait_for(entering, 2)
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'one', 1))
    if corrupt == 'gap':
        frame = delta_event(stream, 'gap', 3)
    elif corrupt == 'duplicate':
        frame = delta_event(stream, 'one', 1)
    elif corrupt == 'started_twice':
        frame = started_event(stream)
    elif corrupt == 'bad_terminal_counts':
        frame = terminal_event(stream, CANCELLED, emitted_bytes=999, partial=True)
    else:
        frame = terminal_event(stream, FAILED)
    assert bridge.rpc.streams.dispatch(frame) is False
    with pytest.raises(PluginError) as failure_value:
        await drain(stream)
    assert failure_value.value.delivery_unknown is True
    assert failure_value.value.code == common.UPSTREAM_UNAVAILABLE
    assert failure_value.value.retryable is False
    assert failure_value.value.acceptance == platform.DURABLY_ACCEPTED
    assert failure_value.value.partial is True
    assert failure_value.value.partial_text == 'one'
    assert failure_value.value.logical_call_id == stream.logical_call_id
    assert stream.terminal is None
    assert stream.completed is False
    assert stream.partial_text == 'one'
    assert len(bridge.rpc.streams) == 0
    await until(lambda: bridge.cancel_requests)


async def test_post_terminal_events_do_not_rewrite_the_terminal():
    bridge = UnitBridge()
    stream = await open_stream(bridge)
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'ok', 1))
    terminal = complete_stream(bridge, stream)
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'late', 3)) is False
    assert bridge.rpc.streams.dispatch(terminal) is False
    assert await drain(stream) == ['ok']
    await stream.aclose()
    assert stream.completed is True
    assert stream.protocol_anomaly is True
    assert stream.terminal.emitted_delta_count == 1


# -- bounded queues: never wait for a slow consumer ------------------------

async def test_frame_queue_overflow_terminates_and_cancels_best_effort():
    bridge = UnitBridge()
    stream = await open_stream(bridge)
    for sequence in range(1, stream_api.MAX_QUEUED_DELTAS + 1):
        assert bridge.rpc.streams.dispatch(delta_event(stream, 'x', sequence)) is True
    assert bridge.rpc.streams.dispatch(
        delta_event(stream, 'overflow', stream_api.MAX_QUEUED_DELTAS + 1)) is False
    assert stream.delivery_unknown is True
    assert stream.failure.cause == 'queue_overflow'
    await until(lambda: bridge.cancel_requests)
    seen = []
    with pytest.raises(PluginError) as failure_value:
        async for delta in stream:
            seen.append(delta.text)
    assert seen == ['x'] * stream_api.MAX_QUEUED_DELTAS
    assert failure_value.value.delivery_unknown is True
    assert failure_value.value.retryable is True
    assert failure_value.value.acceptance == platform.DURABLY_ACCEPTED
    assert failure_value.value.partial_text == 'x' * stream_api.MAX_QUEUED_DELTAS
    assert len(bridge.rpc.streams) == 0


async def test_delta_size_and_total_output_are_bounded():
    bridge = UnitBridge()
    stream = await open_stream(bridge)
    oversized = 'x' * (stream_api.MAX_DELTA_BYTES + 1)
    assert bridge.rpc.streams.dispatch(delta_event(stream, oversized, 1)) is False
    assert stream.failure.cause == 'delta_text'
    await stream.aclose()
    bridge = UnitBridge()
    stream = await open_stream(bridge)
    chunk = 'x' * stream_api.MAX_DELTA_BYTES
    for sequence in range(1, 17):
        assert bridge.rpc.streams.dispatch(delta_event(stream, chunk, sequence)) is True
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'y', 17)) is False
    assert stream.failure.cause == 'delta_text'
    assert stream.received_bytes == stream_api.MAX_RESULT_BYTES
    await stream.aclose()
    assert len(bridge.rpc.streams) == 0


async def test_local_active_stream_limit_is_four():
    bridge = UnitBridge()
    streams = [await open_stream(bridge) for _ in range(stream_api.MAX_ACTIVE_STREAMS)]
    frames_before = len(bridge.frames)
    fifth = bridge.context().model_stream('default', Messages)
    with pytest.raises(PluginError) as limited:
        await fifth.__aenter__()
    assert limited.value.code == common.RATE_LIMITED
    assert limited.value.retryable is True
    assert len(bridge.frames) == frames_before
    for stream in streams:
        await stream.aclose()
    assert len(bridge.rpc.streams) == 0


# -- pre-start rejection paths --------------------------------------------

async def test_prestart_terminal_sequence_zero_ends_pending_start_typed():
    bridge = UnitBridge()
    stream = bridge.context().model_stream('default', Messages)
    entering = asyncio.create_task(stream.__aenter__())
    await until(lambda: any(frame.WhichOneof('body') == 'host_request'
                            and frame.host_request.request_id == stream.request_id
                            for frame in bridge.frames))
    rejection = terminal_event(
        stream, FAILED, sequence=0,
        failure_value=failure(common.RATE_LIMITED, acceptance=platform.NOT_ACCEPTED,
                              recovery=platform.RETRY_SAME_ID),
        emitted_bytes=0, emitted_delta_count=0, partial=False,
        usage_known=False, may_have_billed=False)
    assert bridge.rpc.streams.dispatch(rejection) is True
    with pytest.raises(PluginError) as rejected:
        await asyncio.wait_for(entering, 2)
    assert rejected.value.code == common.RATE_LIMITED
    assert rejected.value.retryable is True
    assert rejected.value.acceptance == platform.NOT_ACCEPTED
    assert rejected.value.recovery == platform.RETRY_SAME_ID
    assert rejected.value.terminal.termination == FAILED
    assert rejected.value.delivery_unknown is False
    assert len(bridge.rpc.streams) == 0
    assert bridge.cancel_requests == []


async def test_prestart_terminal_requires_failed_or_cancelled():
    bridge = UnitBridge()
    stream = bridge.context().model_stream('default', Messages)
    entering = asyncio.create_task(stream.__aenter__())
    await until(lambda: any(frame.WhichOneof('body') == 'host_request'
                            and frame.host_request.request_id == stream.request_id
                            for frame in bridge.frames))
    assert bridge.rpc.streams.dispatch(terminal_event(
        stream, REPLAY, sequence=0, partial=False, emitted_bytes=0,
        emitted_delta_count=0, may_have_billed=False)) is False
    with pytest.raises(PluginError) as rejected:
        await asyncio.wait_for(entering, 2)
    assert rejected.value.delivery_unknown is True
    assert rejected.value.code == common.UPSTREAM_UNAVAILABLE


async def test_host_response_rejection_ends_pending_start():
    bridge = UnitBridge()
    stream = bridge.context().model_stream('default', Messages)
    entering = asyncio.create_task(stream.__aenter__())
    await until(lambda: any(frame.WhichOneof('body') == 'host_request'
                            and frame.host_request.request_id == stream.request_id
                            for frame in bridge.frames))
    attempt = bridge.rpc.streams.find(stream.request_id)
    Runner._reject_stream_start(attempt, plugins.HostResponse(
        request_id=stream.request_id,
        error=common.Error(code=common.PERMISSION_DENIED, message='',
                           retryable=False)))
    with pytest.raises(PluginError) as rejected:
        await asyncio.wait_for(entering, 2)
    assert rejected.value.code == common.PERMISSION_DENIED
    assert rejected.value.retryable is False
    assert len(bridge.rpc.streams) == 0


# -- consumer cancellation / early exit ------------------------------------

async def test_consumer_exit_sends_best_effort_cancel_and_releases_registry():
    bridge = UnitBridge()
    stream = bridge.context().model_stream('default', Messages)
    received = []

    async def consume():
        async with stream:
            async for delta in stream:
                received.append(delta.text)
                return

    task = asyncio.create_task(consume())
    await until(lambda: bridge.frames)
    assert bridge.rpc.streams.dispatch(started_event(stream))
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'kept', 1))
    await asyncio.wait_for(task, 3)
    assert received == ['kept']
    assert stream.terminal is None
    assert stream.completed is False
    assert stream.partial_text == 'kept'
    await until(lambda: bridge.cancel_requests)
    assert bridge.cancel_requests == [(stream.request_id, stream.logical_call_id)]
    assert stream.cancel_receipt.disposition == \
        streaming.MODEL_STREAM_CANCELLATION_REQUESTED
    assert len(bridge.rpc.streams) == 0


async def test_consumer_task_cancellation_is_not_disguised_as_not_executed():
    bridge = UnitBridge()
    stream = bridge.context().model_stream('default', Messages)
    consumed = []

    async def consume():
        async with stream:
            async for delta in stream:
                consumed.append(delta.text)
                await asyncio.sleep(30)

    task = asyncio.create_task(consume())
    await until(lambda: bridge.frames)
    assert bridge.rpc.streams.dispatch(started_event(stream))
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'typed', 1))
    await until(lambda: consumed == ['typed'])
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert consumed == ['typed']
    assert stream.terminal is None
    assert stream.completed is False
    assert stream.partial_text == 'typed'
    assert stream._attempt.consumer_cancelled is True
    await until(lambda: bridge.cancel_requests)
    assert bridge.cancel_requests == [(stream.request_id, stream.logical_call_id)]
    assert len(bridge.rpc.streams) == 0


async def test_explicit_cancel_returns_typed_receipt():
    bridge = UnitBridge()
    stream = await open_stream(bridge)
    bridge.cancel_disposition = streaming.MODEL_STREAM_ALREADY_TERMINAL
    receipt = await stream.cancel()
    assert receipt.start_request_id == stream.request_id
    assert receipt.logical_call_id == stream.logical_call_id
    assert receipt.disposition == streaming.MODEL_STREAM_ALREADY_TERMINAL
    assert stream.cancel_receipt == receipt
    assert bridge.cancel_requests == [(stream.request_id, stream.logical_call_id)]
    await stream.aclose()
    assert len(bridge.rpc.streams) == 0


# -- connection teardown / terminal loss -----------------------------------

async def test_connection_close_marks_pending_stream_unknown_but_keeps_partial():
    bridge = UnitBridge()
    stream = bridge.context().model_stream('default', Messages)
    consumed = []

    async def consume():
        async with stream:
            async for delta in stream:
                consumed.append(delta.text)

    task = asyncio.create_task(consume())
    await until(lambda: any(frame.WhichOneof('body') == 'host_request'
                            and frame.host_request.request_id == stream.request_id
                            for frame in bridge.frames))
    assert bridge.rpc.streams.dispatch(started_event(stream))
    assert bridge.rpc.streams.dispatch(delta_event(stream, 'partial', 1))
    await until(lambda: consumed == ['partial'])
    await bridge.rpc.aclose()
    with pytest.raises(PluginError) as failure_value:
        await asyncio.wait_for(task, 2)
    assert failure_value.value.code == common.UPSTREAM_UNAVAILABLE
    assert failure_value.value.delivery_unknown is True
    assert failure_value.value.retryable is True
    assert failure_value.value.partial_text == 'partial'
    assert stream.terminal is None
    assert stream.completed is False
    assert stream.partial_text == 'partial'
    assert len(bridge.rpc.streams) == 0
    with pytest.raises(PluginError) as unavailable:
        await bridge.rpc.cancel_stream(stream.request_id, stream.logical_call_id, 5.0)
    assert unavailable.value.code == common.UPSTREAM_UNAVAILABLE


async def test_start_write_failure_releases_registry():
    calls = []

    async def failing_write(frame):
        calls.append(frame)
        raise asyncio.TimeoutError

    rpc = _Rpc(failing_write, {}, {})
    rpc.streams.cancel_callback = None
    context = Context()
    context._rpc = rpc
    context._features = frozenset({'host.model.stream.v1'})
    context._permissions = frozenset({'model.chat'})
    stream = context.model_stream('default', Messages)
    with pytest.raises(PluginError) as failure_value:
        await stream.__aenter__()
    assert failure_value.value.code == common.TIMEOUT
    assert rpc.streams.find(stream.request_id) is None


# -- integration: real Runner + TLS PluginBridge fixture -------------------

def stream_manifest():
    value = manifest()
    value.permissions.append('model.chat')
    return value


async def start_runner(host):
    stop = asyncio.Event()

    class Idle(Plugin):
        pass

    runner = Runner(stream_manifest(), Idle(), options(host))
    task = asyncio.create_task(runner.run(stop))
    async with asyncio.timeout(3):
        while not runner.context._rpc:
            if task.done():
                await task
            await asyncio.sleep(.01)
    return runner, stop, task


async def stop_runner(stop, task):
    stop.set()
    with suppress(asyncio.CancelledError, Exception):
        await asyncio.wait_for(task, 5)


async def receive(queue):
    return await asyncio.wait_for(queue.get(), 3)


async def test_reader_stream_delivery_does_not_block_control_frames():
    async with FakeTargetRuntime(features=('host.model.stream.v1',)) as host:
        runner, stop, run_task = await start_runner(host)
        registration = await receive(host.registrations)
        assert 'host.model.stream.v1' in registration.supported_features
        stream = runner.context.model_stream('default', Messages)
        stream_task = asyncio.create_task(stream.__aenter__())
        start = await receive(host.stream_starts)
        assert ids(start) == ids(stream)
        await host.send_stream_event(started_event(stream))
        await asyncio.wait_for(stream_task, 3)
        # Flood without a consumer: the bounded local queue overflows, the SDK
        # locally terminates and asks the Runtime to cancel, while the shared
        # reader keeps processing heartbeats and the cancel receipt.
        for sequence in range(1, stream_api.MAX_QUEUED_DELTAS + 2):
            await host.send_stream_event(delta_event(stream, 'x', sequence))
        cancel = await receive(host.stream_cancels)
        assert cancel.model_stream_cancel.start_request_id == stream.request_id
        assert cancel.model_stream_cancel.logical_call_id == stream.logical_call_id
        await until(lambda: stream.cancel_receipt is not None)
        assert stream.cancel_receipt.disposition == \
            streaming.MODEL_STREAM_CANCELLATION_REQUESTED
        await receive(host.heartbeats)
        assert runner.context._rpc is not None
        with pytest.raises(PluginError) as failure_value:
            async for _ in stream:
                pass
        assert failure_value.value.delivery_unknown is True
        assert failure_value.value.partial is True
        await stop_runner(stop, run_task)


async def test_session_disconnect_ends_pending_stream_and_reconnects():
    async with FakeTargetRuntime(features=('host.model.stream.v1',)) as host:
        runner, stop, run_task = await start_runner(host)
        stream = runner.context.model_stream('default', Messages)
        consumed = []

        async def consume():
            async with stream:
                async for delta in stream:
                    consumed.append(delta.text)

        consumer = asyncio.create_task(consume())
        start = await receive(host.stream_starts)
        await host.send_stream_event(started_event(stream))
        await host.send_stream_event(delta_event(stream, 'kept', 1))
        await until(lambda: consumed == ['kept'])
        await host.disconnect()
        with pytest.raises(PluginError) as failure_value:
            await asyncio.wait_for(consumer, 3)
        assert failure_value.value.delivery_unknown is True
        assert failure_value.value.code == common.UPSTREAM_UNAVAILABLE
        assert failure_value.value.partial_text == 'kept'
        assert stream.terminal is None
        # The runner reconnects with a fresh fence; reusing the stable logical id
        # is a new start request id (replay metadata, never replayed text).
        await until(lambda: host.connection_generation >= 2
                     and runner.context._rpc is not None
                     and 'host.model.stream.v1' in runner.context._features)
        replacement = runner.context.model_stream(
            'default', Messages, logical_call_id=stream.logical_call_id)
        replacement_task = asyncio.create_task(replacement.__aenter__())
        second_start = await receive(host.stream_starts)
        assert ids(second_start).logical_call_id == stream.logical_call_id
        assert second_start.request_id != start.request_id
        call = succeeded_call(stream.logical_call_id)
        await host.send_stream_event(started_event(
            replacement, disposition=REPLAY_TERMINAL, call=call))
        await asyncio.wait_for(replacement_task, 3)
        await host.send_stream_event(terminal_event(
            replacement, REPLAY, call=call, usage_known=True, input_tokens=3,
            output_tokens=4))
        assert await drain(replacement) == []
        await replacement.aclose()
        assert replacement.termination == REPLAY
        await stop_runner(stop, run_task)


async def test_prestart_host_response_rejection_ends_pending_stream_integration():
    async with FakeTargetRuntime(features=('host.model.stream.v1',)) as host:
        runner, stop, run_task = await start_runner(host)
        stream = runner.context.model_stream('default', Messages)
        stream_task = asyncio.create_task(stream.__aenter__())
        start = await receive(host.stream_starts)
        await host.reject_stream_start(start, common.RATE_LIMITED, retryable=True)
        with pytest.raises(PluginError) as rejected:
            await asyncio.wait_for(stream_task, 3)
        assert rejected.value.code == common.RATE_LIMITED
        assert rejected.value.retryable is True
        rpc = runner.context._rpc
        assert rpc is not None and len(rpc.streams) == 0
        await stop_runner(stop, run_task)


async def test_integration_consumer_exit_sends_cancel_through_writer():
    async with FakeTargetRuntime(features=('host.model.stream.v1',)) as host:
        runner, stop, run_task = await start_runner(host)
        stream = runner.context.model_stream('default', Messages)
        received = []

        async def consume():
            async with stream:
                async for delta in stream:
                    received.append(delta.text)
                    return

        task = asyncio.create_task(consume())
        start = await receive(host.stream_starts)
        await host.send_stream_event(started_event(stream))
        await host.send_stream_event(delta_event(stream, 'bye', 1))
        await asyncio.wait_for(task, 3)
        cancel = await receive(host.stream_cancels)
        assert cancel.model_stream_cancel.start_request_id == start.request_id
        assert stream.cancel_receipt.disposition == \
            streaming.MODEL_STREAM_CANCELLATION_REQUESTED
        assert len(runner.context._rpc.streams) == 0
        await stop_runner(stop, run_task)
