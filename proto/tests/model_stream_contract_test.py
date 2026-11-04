#!/usr/bin/env python3
"""STR-01 executable specification fixtures, NOT production implementation evidence.

Run with generated kiarabot.v1 on PYTHONPATH. No Provider, database, Bridge, SDK,
authorization or cancellation implementation is exercised by this test module.
"""

import sys
import unittest

from google.protobuf.message import DecodeError
from kiarabot.v1 import common_pb2 as common
from kiarabot.v1 import model_stream_pb2 as stream
from kiarabot.v1 import platform_pb2 as platform
from kiarabot.v1 import plugins_pb2 as plugins

from model_stream_contract_rules import cancel_response_valid, model_stream_errors, model_stream_valid


REQUEST = '11111111-1111-4111-8111-111111111111'
LOGICAL = '22222222-2222-4222-8222-222222222222'
TRACE = '33333333-3333-4333-8333-333333333333'
OWNER = '44444444-4444-4444-8444-444444444444'
MODEL = '55555555-5555-4555-8555-555555555555'
CANCEL = '66666666-6666-4666-8666-666666666666'


def event(sequence, **body):
    return stream.ModelStreamEvent(request_id=REQUEST, logical_call_id=LOGICAL,
                                   trace_id=TRACE, sequence=sequence, **body)


def call(state=platform.OPERATION_RUNNING, outcome=platform.EXECUTION_UNKNOWN,
         known=False, billed=True):
    result = stream.ModelStreamCallSnapshot(
        logical_call_id=LOGICAL, owner_instance_id=OWNER, model_id=MODEL,
        config_version=7, secret_version=2, state=state, execution_outcome=outcome,
        usage=stream.ModelStreamUsage(known=known, input_tokens=12 if known else 0,
                                      output_tokens=3 if known else 0),
        may_have_billed=billed)
    result.observed_at.seconds = 1700000000
    return result


def failure(code=common.UPSTREAM_UNAVAILABLE, acceptance=platform.ACCEPTANCE_UNKNOWN,
            recovery=platform.QUERY_SAME_INTENT):
    return platform.RpcFailure(
        error=common.Error(code=code, message=common.ErrorCode.Name(code),
                           request_id=REQUEST, trace_id=TRACE),
        context=platform.FailureContext(acceptance=acceptance, recovery=recovery))


def fresh(text='你好', known=True):
    result = [event(0, started=stream.ModelStreamStarted(disposition=stream.MODEL_STREAM_FRESH,
                                                        call=call()))]
    if text:
        result.append(event(1, delta=stream.ModelStreamDelta(text=text)))
    terminal_call = call(platform.OPERATION_SUCCEEDED, platform.EXECUTION_KNOWN_SUCCEEDED,
                         known=known)
    result.append(event(2 if text else 1, terminal=stream.ModelStreamTerminal(
        termination=stream.MODEL_STREAM_COMPLETED, emitted_bytes=len(text.encode()),
        emitted_delta_count=1 if text else 0, finish_reason=stream.MODEL_STREAM_FINISH_STOP,
        call=terminal_call, usage=terminal_call.usage, may_have_billed=True)))
    return result


def interrupted(termination=stream.MODEL_STREAM_FAILED, text='你好'):
    result = fresh(text)
    terminal = result[-1].terminal
    terminal.termination = termination
    terminal.partial = bool(text)
    terminal.finish_reason = stream.MODEL_STREAM_FINISH_REASON_UNSPECIFIED
    terminal.failure.CopyFrom(failure())
    terminal.call.CopyFrom(call(platform.OPERATION_BLOCKED, platform.EXECUTION_UNKNOWN))
    terminal.usage.CopyFrom(terminal.call.usage)
    return result


def replay(in_progress=False):
    snapshot = call() if in_progress else call(platform.OPERATION_SUCCEEDED,
                                               platform.EXECUTION_KNOWN_SUCCEEDED, known=True)
    disposition = (stream.MODEL_STREAM_REPLAY_IN_PROGRESS if in_progress else
                   stream.MODEL_STREAM_REPLAY_TERMINAL)
    return [event(0, started=stream.ModelStreamStarted(disposition=disposition, call=snapshot)),
            event(1, terminal=stream.ModelStreamTerminal(
                termination=stream.MODEL_STREAM_REPLAY, call=snapshot,
                usage=snapshot.usage, may_have_billed=snapshot.may_have_billed))]


class ModelStreamSpecificationTest(unittest.TestCase):
    """Wire acceptance/rejection oracle; these are not live server assertions."""

    def assert_valid(self, frames):
        self.assertEqual(model_stream_errors(frames), [])

    def test_fresh_text_and_empty_completion(self):
        self.assert_valid(fresh())
        self.assert_valid(fresh(''))
        self.assert_valid(fresh(known=False))

    def test_partial_failure_and_zero_delta_unknown(self):
        self.assert_valid(interrupted())
        self.assert_valid(interrupted(text=''))

    def test_prestart_rejection_without_call_and_explicit_false_billing(self):
        frames = [event(0, terminal=stream.ModelStreamTerminal(
            termination=stream.MODEL_STREAM_FAILED,
            failure=failure(common.INVALID_ARGUMENT, platform.NOT_ACCEPTED, platform.CORRECT_INPUT),
            usage=stream.ModelStreamUsage(known=False), may_have_billed=False))]
        self.assert_valid(frames)
        wire = frames[0].SerializeToString()
        decoded = stream.ModelStreamEvent.FromString(wire)
        self.assertTrue(decoded.terminal.HasField('may_have_billed'))
        self.assertFalse(decoded.terminal.may_have_billed)

    def test_prestart_unknown_admission_never_becomes_known_failure(self):
        frames = [event(0, terminal=stream.ModelStreamTerminal(
            termination=stream.MODEL_STREAM_FAILED, failure=failure(),
            usage=stream.ModelStreamUsage(known=False), may_have_billed=True))]
        self.assert_valid(frames)
        frames[0].sequence = 1
        self.assertFalse(model_stream_valid(frames))

    def test_replay_terminal_and_in_progress_have_no_text(self):
        self.assert_valid(replay())
        self.assert_valid(replay(in_progress=True))

    def test_truncation_preserves_known_provider_success(self):
        for finish in (stream.MODEL_STREAM_FINISH_LENGTH, stream.MODEL_STREAM_FINISH_CONTENT_FILTER):
            frames = fresh()
            terminal = frames[-1].terminal
            terminal.termination = stream.MODEL_STREAM_TRUNCATED
            terminal.finish_reason = finish
            terminal.partial = True
            self.assert_valid(frames)

    def test_cancel_after_dispatch_preserves_unknown_billing(self):
        self.assert_valid(interrupted(stream.MODEL_STREAM_CANCELLED))

    def test_known_not_executed_cancellation_before_provider(self):
        frames = interrupted(stream.MODEL_STREAM_CANCELLED, text='')
        terminal = frames[-1].terminal
        terminal.call.CopyFrom(call(platform.OPERATION_FAILED, platform.EXECUTION_KNOWN_FAILED,
                                   known=True, billed=False))
        terminal.call.usage.Clear()
        terminal.call.usage.known = True
        terminal.usage.CopyFrom(terminal.call.usage)
        terminal.may_have_billed = False
        self.assert_valid(frames)

    def test_hard_close_is_a_valid_prefix_but_not_a_complete_result(self):
        frames = fresh()[:-1]
        self.assertTrue(model_stream_valid(frames, require_terminal=False))
        self.assertFalse(model_stream_valid(frames))
        self.assertFalse(model_stream_valid([], require_terminal=False))

    def test_missing_delta_body_or_started(self):
        self.assertFalse(model_stream_valid([event(0)]))
        self.assertFalse(model_stream_valid(fresh()[1:]))

    def test_empty_delta_is_distinct_from_no_delta_completion(self):
        frames = fresh()
        frames[1].delta.text = ''
        frames[-1].terminal.emitted_bytes = 0
        self.assertFalse(model_stream_valid(frames))
        self.assert_valid(fresh(''))

    def test_invalid_utf8_wire_is_rejected_by_protobuf_string_boundary(self):
        # text (field 1) containing one invalid UTF-8 byte, not a Python surrogate fixture.
        with self.assertRaises(DecodeError):
            stream.ModelStreamDelta.FromString(b'\x0a\x01\xff')

    def test_gap_duplicate_and_reordered_sequences(self):
        for number in (0, 2, 99):
            frames = fresh()
            frames[1].sequence = number
            self.assertFalse(model_stream_valid(frames))
        frames = fresh()
        self.assertFalse(model_stream_valid([frames[0], frames[1], frames[1], frames[2]]))

    def test_duplicate_started_terminal_or_post_terminal_delta(self):
        frames = fresh()
        self.assertFalse(model_stream_valid([frames[0], *frames]))
        self.assertFalse(model_stream_valid([*frames, frames[-1]]))
        self.assertFalse(model_stream_valid([*frames, frames[1]]))

    def test_terminal_sequence_and_counts_are_exact_utf8_bytes(self):
        for field, wrong in (('emitted_bytes', 2), ('emitted_delta_count', 0)):
            frames = fresh()
            setattr(frames[-1].terminal, field, wrong)
            self.assertFalse(model_stream_valid(frames))
        frames = fresh()
        frames[-1].sequence = 1
        self.assertFalse(model_stream_valid(frames))

    def test_attempt_and_failure_correlation_cannot_change(self):
        for field in ('request_id', 'logical_call_id', 'trace_id'):
            frames = fresh()
            setattr(frames[1], field, CANCEL)
            self.assertFalse(model_stream_valid(frames))
        for field in ('request_id', 'trace_id'):
            frames = interrupted()
            setattr(frames[-1].terminal.failure.error, field, CANCEL)
            self.assertFalse(model_stream_valid(frames))

    def test_missing_and_invalid_usage(self):
        frames = fresh()
        frames[-1].terminal.ClearField('usage')
        self.assertFalse(model_stream_valid(frames))
        for field in ('input_tokens', 'output_tokens'):
            frames = fresh(known=False)
            setattr(frames[-1].terminal.usage, field, 1)
            self.assertFalse(model_stream_valid(frames))
        frames = fresh()
        frames[-1].terminal.usage.input_tokens += 1
        self.assertFalse(model_stream_valid(frames))

    def test_billing_presence_and_conservative_running_unknown(self):
        for target in ('terminal', 'call', 'started'):
            frames = fresh()
            value = (frames[-1].terminal if target == 'terminal' else frames[-1].terminal.call
                     if target == 'call' else frames[0].started.call)
            value.ClearField('may_have_billed')
            self.assertFalse(model_stream_valid(frames))
        frames = interrupted()
        frames[-1].terminal.call.may_have_billed = False
        frames[-1].terminal.may_have_billed = False
        self.assertFalse(model_stream_valid(frames))

    def test_partial_flag_cannot_hide_or_invent_partial_text(self):
        for frames in (fresh(), interrupted(), interrupted(text='')):
            frames[-1].terminal.partial = not frames[-1].terminal.partial
            self.assertFalse(model_stream_valid(frames))

    def test_replay_never_delivers_delta_or_fresh_completion(self):
        frames = replay()
        frames.insert(1, event(1, delta=stream.ModelStreamDelta(text='old text')))
        self.assertFalse(model_stream_valid(frames))
        frames = fresh()
        frames[-1].terminal.termination = stream.MODEL_STREAM_REPLAY
        self.assertFalse(model_stream_valid(frames))
        frames = replay()
        frames[-1].terminal.call.usage.output_tokens += 1
        self.assertFalse(model_stream_valid(frames))

    def test_completion_requires_success_snapshot_and_stop(self):
        for field in ('call',):
            frames = fresh()
            frames[-1].terminal.ClearField(field)
            self.assertFalse(model_stream_valid(frames))
        frames = fresh()
        frames[-1].terminal.call.state = platform.OPERATION_BLOCKED
        self.assertFalse(model_stream_valid(frames))
        frames = fresh()
        frames[-1].terminal.finish_reason = stream.MODEL_STREAM_FINISH_LENGTH
        self.assertFalse(model_stream_valid(frames))

    def test_failure_is_required_only_on_failure_types(self):
        frames = interrupted()
        frames[-1].terminal.ClearField('failure')
        self.assertFalse(model_stream_valid(frames))
        frames = fresh()
        frames[-1].terminal.failure.CopyFrom(failure())
        self.assertFalse(model_stream_valid(frames))
        for field in ('error', 'context'):
            frames = interrupted()
            frames[-1].terminal.failure.ClearField(field)
            self.assertFalse(model_stream_valid(frames))
        for field in ('acceptance', 'recovery'):
            frames = interrupted()
            setattr(frames[-1].terminal.failure.context, field, 999)
            self.assertFalse(model_stream_valid(frames))

    def test_unknown_enum_values_are_rejected(self):
        for field in ('termination', 'finish_reason'):
            frames = fresh()
            setattr(frames[-1].terminal, field, 999)
            self.assertFalse(model_stream_valid(frames))
        frames = fresh()
        frames[0].started.disposition = 999
        self.assertFalse(model_stream_valid(frames))

    def test_identity_and_pinned_config_secret_versions_do_not_change(self):
        for field, wrong in (('owner_instance_id', CANCEL), ('model_id', CANCEL),
                             ('config_version', 8), ('secret_version', 3)):
            frames = fresh()
            setattr(frames[-1].terminal.call, field, wrong)
            self.assertFalse(model_stream_valid(frames))

    def test_per_delta_and_total_byte_limits(self):
        self.assert_valid(fresh('a' * 4096))
        self.assertFalse(model_stream_valid(fresh('a' * 4097)))
        self.assertFalse(model_stream_valid(fresh('你' * 1366)))
        frames = fresh('a' * 4096)
        frames = [frames[0]] + [event(i, delta=stream.ModelStreamDelta(text='a' * 4096))
                                for i in range(1, 17)] + [frames[-1]]
        frames[-1].sequence = 17
        frames[-1].terminal.emitted_delta_count = 16
        frames[-1].terminal.emitted_bytes = 65536
        self.assert_valid(frames)
        frames.insert(-1, event(17, delta=stream.ModelStreamDelta(text='x')))
        frames[-1].sequence = 18
        frames[-1].terminal.emitted_delta_count += 1
        frames[-1].terminal.emitted_bytes += 1
        self.assertFalse(model_stream_valid(frames))

    def test_cancel_receipt_is_correlated_but_does_not_complete_a_stream(self):
        for disposition in (stream.MODEL_STREAM_CANCELLATION_REQUESTED,
                            stream.MODEL_STREAM_ALREADY_TERMINAL, stream.MODEL_STREAM_NOT_ACTIVE):
            response = plugins.HostResponse(request_id=CANCEL,
                model_stream_cancel=stream.ModelStreamCancelResponse(
                    start_request_id=REQUEST, logical_call_id=LOGICAL, disposition=disposition))
            args = dict(request_id=CANCEL, start_request_id=REQUEST, logical_call_id=LOGICAL)
            self.assertTrue(cancel_response_valid(response, **args))
            self.assertFalse(model_stream_valid(fresh()[:-1]))
            response.model_stream_cancel.logical_call_id = CANCEL
            self.assertFalse(cancel_response_valid(response, **args))


def check_model_stream_contract():
    """Shared contract gate entry point; passing is specification evidence only."""
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ModelStreamSpecificationTest)
    result = unittest.TextTestRunner(stream=sys.stdout, verbosity=0).run(suite)
    if not result.wasSuccessful():
        raise AssertionError('Model streaming specification oracle failed')
    return result.testsRun


if __name__ == '__main__':
    count = check_model_stream_contract()
    print(f'Model stream specification oracle: {count} tests passed; no production execution tested')
