"""STR-01 specification oracle; this module is not a production stream implementation.

It checks observable wire shapes only. Passing cannot prove authorization, persistence,
Provider call counts, cancellation, bounded queues or a working streaming transport.
"""

import re

from kiarabot.v1 import common_pb2 as common
from kiarabot.v1 import model_stream_pb2 as stream
from kiarabot.v1 import platform_pb2 as platform


_UUID = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z')
_PROVIDER_ID = re.compile(r'[A-Za-z0-9._:-]{0,128}\Z')
_TERMINATIONS = set(stream.ModelStreamTermination.values()) - {0}
_FINISHES = set(stream.ModelStreamFinishReason.values())
_DISPOSITIONS = set(stream.ModelStreamDisposition.values()) - {0}
_ACCEPTANCES = {platform.NOT_ACCEPTED, platform.DURABLY_ACCEPTED,
                platform.ACCEPTANCE_UNKNOWN}
_RECOVERIES = set(platform.RecoveryAction.values()) - {0}


def usage_valid(usage):
    """Unknown token counters carry no authoritative estimate."""
    return usage.known or (usage.input_tokens == 0 and usage.output_tokens == 0)


def _same_usage(left, right):
    return (left.known, left.input_tokens, left.output_tokens) == (
        right.known, right.input_tokens, right.output_tokens)


def call_snapshot_valid(call, logical_call_id):
    if (call.logical_call_id != logical_call_id
            or not _UUID.fullmatch(call.logical_call_id)
            or not _UUID.fullmatch(call.owner_instance_id)
            or not _UUID.fullmatch(call.model_id)
            or call.config_version == 0 or call.secret_version == 0
            or not call.HasField('usage') or not usage_valid(call.usage)
            or not call.HasField('may_have_billed')
            or not call.HasField('observed_at') or call.observed_at.seconds <= 0
            or not 0 <= call.observed_at.nanos < 1_000_000_000
            or not _PROVIDER_ID.fullmatch(call.provider_request_id)
            or call.latency_ms > 300000):
        return False
    allowed_outcomes = {
        platform.OPERATION_RUNNING: {platform.EXECUTION_UNKNOWN},
        platform.OPERATION_BLOCKED: {platform.EXECUTION_UNKNOWN},
        platform.OPERATION_SUCCEEDED: {platform.EXECUTION_KNOWN_SUCCEEDED},
        # An explicitly resolved UNKNOWN can be terminal Failed with unknown execution.
        platform.OPERATION_FAILED: {platform.EXECUTION_KNOWN_FAILED, platform.EXECUTION_UNKNOWN},
    }
    if call.execution_outcome not in allowed_outcomes.get(call.state, set()):
        return False
    if call.state in {platform.OPERATION_RUNNING, platform.OPERATION_BLOCKED}:
        if not call.may_have_billed:
            return False
    if call.HasField('reason') and call.reason.code not in set(common.ErrorCode.values()) - {0}:
        return False
    return True


def _failure_valid(failure, event):
    return (failure.HasField('error') and failure.HasField('context')
            and failure.error.code in set(common.ErrorCode.values()) - {0}
            and failure.error.request_id == event.request_id
            and failure.error.trace_id == event.trace_id
            and failure.context.acceptance in _ACCEPTANCES
            and failure.context.recovery in _RECOVERIES)


def model_stream_errors(events, *, require_terminal=True):
    """Validate one ordered delivery attempt, not arbitrary interleaved Bridge traffic.

    require_terminal=False permits an unfinished prefix after hard transport loss. Such a
    prefix does not become successful; callers must still inspect a real COMPLETED terminal.
    Frame Fence and current permission checks remain production boundary responsibilities.
    """
    errors = []
    started = None
    correlation = None
    delta_count = 0
    emitted_bytes = 0
    terminated = False
    saw_event = False
    for event in events:
        saw_event = True
        ids = (event.request_id, event.logical_call_id, event.trace_id)
        if not all(_UUID.fullmatch(value) for value in ids):
            errors.append('invalid attempt identifiers')
        if correlation is None:
            correlation = ids
        elif ids != correlation:
            errors.append('attempt correlation changed')
        if terminated:
            errors.append('event after terminal')
            continue
        kind = event.WhichOneof('body')
        if kind == 'started':
            if started is not None or delta_count or event.sequence != 0:
                errors.append('started must occur once at sequence zero')
            started = event.started
            if (started.disposition not in _DISPOSITIONS or not started.HasField('call')
                    or not call_snapshot_valid(started.call, event.logical_call_id)):
                errors.append('invalid started snapshot/disposition')
            if started.disposition in {stream.MODEL_STREAM_FRESH,
                                       stream.MODEL_STREAM_REPLAY_IN_PROGRESS}:
                if started.call.state != platform.OPERATION_RUNNING:
                    errors.append('fresh/in-progress snapshot must be running')
            elif started.call.state == platform.OPERATION_RUNNING:
                errors.append('terminal replay cannot be running')
        elif kind == 'delta':
            if started is None or started.disposition != stream.MODEL_STREAM_FRESH:
                errors.append('delta requires a fresh started event')
            if event.sequence != delta_count + 1:
                errors.append('delta sequence gap/duplicate/reorder')
            try:
                size = len(event.delta.text.encode('utf-8'))
            except UnicodeError:
                size = 0
                errors.append('invalid UTF-8 delta')
            if not 1 <= size <= 4096:
                errors.append('delta exceeds nonempty 4096-byte bound')
            delta_count += 1
            emitted_bytes += size
            if emitted_bytes > 65536:
                errors.append('attempt exceeds 65536 text bytes')
        elif kind == 'terminal':
            terminated = True
            terminal = event.terminal
            expected_sequence = delta_count + 1 if started is not None else 0
            if event.sequence != expected_sequence:
                errors.append('terminal sequence mismatch')
            if terminal.termination not in _TERMINATIONS:
                errors.append('unknown/unspecified termination')
            if terminal.finish_reason not in _FINISHES:
                errors.append('unknown finish reason')
            if (terminal.emitted_bytes != emitted_bytes
                    or terminal.emitted_delta_count != delta_count):
                errors.append('terminal emission counts mismatch')
            expected_partial = emitted_bytes > 0 and terminal.termination != stream.MODEL_STREAM_COMPLETED
            if terminal.partial != expected_partial:
                errors.append('partial disagrees with this attempt')
            if not terminal.HasField('usage') or not usage_valid(terminal.usage):
                errors.append('missing or invalid terminal usage')
            if not terminal.HasField('may_have_billed'):
                errors.append('missing explicit terminal billing fact')
            has_failure = terminal.HasField('failure')
            if has_failure and not _failure_valid(terminal.failure, event):
                errors.append('invalid failure code/context/correlation')
            if terminal.termination == stream.MODEL_STREAM_FAILED and not has_failure:
                errors.append('failed terminal requires RpcFailure')
            if terminal.termination in {stream.MODEL_STREAM_COMPLETED, stream.MODEL_STREAM_TRUNCATED,
                                        stream.MODEL_STREAM_REPLAY} and has_failure:
                errors.append('non-failure termination must not carry RpcFailure')
            if terminal.HasField('call'):
                call = terminal.call
                if not call_snapshot_valid(call, event.logical_call_id):
                    errors.append('invalid terminal call snapshot')
                if (not _same_usage(terminal.usage, call.usage)
                        or terminal.may_have_billed != call.may_have_billed):
                    errors.append('terminal facts disagree with call')
                if started is not None:
                    pinned_fields = ('owner_instance_id', 'model_id', 'config_version', 'secret_version')
                    if any(getattr(call, key) != getattr(started.call, key) for key in pinned_fields):
                        errors.append('call identity/pinned version changed')
            if started is None and terminal.termination not in {
                    stream.MODEL_STREAM_FAILED, stream.MODEL_STREAM_CANCELLED}:
                errors.append('pre-start terminal cannot complete/replay/truncate')
            if started is not None:
                replay = started.disposition in {stream.MODEL_STREAM_REPLAY_IN_PROGRESS,
                                                  stream.MODEL_STREAM_REPLAY_TERMINAL}
                if replay != (terminal.termination == stream.MODEL_STREAM_REPLAY):
                    errors.append('replay admission/termination mismatch')
                if replay and (delta_count or not terminal.HasField('call')
                               or terminal.call != started.call):
                    errors.append('replay must carry unchanged metadata and no delta')
            if terminal.termination in {stream.MODEL_STREAM_COMPLETED, stream.MODEL_STREAM_TRUNCATED}:
                if (not terminal.HasField('call')
                        or terminal.call.state != platform.OPERATION_SUCCEEDED
                        or terminal.call.execution_outcome != platform.EXECUTION_KNOWN_SUCCEEDED):
                    errors.append('complete/truncated requires committed provider success')
                wanted = ({stream.MODEL_STREAM_FINISH_STOP}
                          if terminal.termination == stream.MODEL_STREAM_COMPLETED else
                          {stream.MODEL_STREAM_FINISH_LENGTH, stream.MODEL_STREAM_FINISH_CONTENT_FILTER})
                if terminal.finish_reason not in wanted:
                    errors.append('finish reason disagrees with completion type')
        else:
            errors.append('missing/unknown event body')
    if not saw_event:
        errors.append('empty attempt has no observed result')
    if require_terminal and not terminated:
        errors.append('missing terminal is unknown delivery')
    return errors


def model_stream_valid(events, *, require_terminal=True):
    return not model_stream_errors(events, require_terminal=require_terminal)


def cancel_response_valid(response, *, request_id, start_request_id, logical_call_id):
    """Receipt shape only; it never supplies a stream terminal or billing conclusion."""
    if (not all(_UUID.fullmatch(value) for value in (request_id, start_request_id, logical_call_id))
            or response.request_id != request_id or response.HasField('error')
            or response.WhichOneof('result') != 'model_stream_cancel'):
        return False
    receipt = response.model_stream_cancel
    return (receipt.start_request_id == start_request_id
            and receipt.logical_call_id == logical_call_id
            and receipt.disposition in set(stream.ModelStreamCancelDisposition.values()) - {0})
