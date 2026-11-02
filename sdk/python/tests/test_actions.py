"""P07b SDK Action semantics: submission preflight, correlation and result interpretation."""
import asyncio
from datetime import datetime, timedelta, timezone
import uuid

import pytest
from kiarabot.v1 import actions_pb2 as actions, events_pb2 as events, plugins_pb2 as plugins
from kiarabot.v1 import common_pb2 as common, platform_pb2 as platform
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk import actions as action_api
from kiarabot_sdk.testing import FakeTargetRuntime
from test_runtime import manifest, options

TERMINAL = (actions.SUCCEEDED, actions.FAILED, actions.EXPIRED, actions.CANCELLED)


async def ready(runner):
    async with asyncio.timeout(2):
        while not runner.context._rpc:
            await asyncio.sleep(.01)


def submission(action_id='sent', idempotency_key='key', deadline_seconds=60,
               payload='send_message'):
    value = actions.ActionEnvelope(action_id=action_id, idempotency_key=idempotency_key,
                                   trace_id='trace', requested_by='plugin')
    value.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=deadline_seconds))
    getattr(value, payload).SetInParent()
    return value


def action_result(action_id='sent', status=actions.ACCEPTED, outcome=None,
                  dispatch_attempt_id='', error_code=0):
    if outcome is None:
        outcome = {actions.ACCEPTED: platform.EXECUTION_NOT_STARTED,
                   actions.SUCCEEDED: platform.EXECUTION_KNOWN_SUCCEEDED,
                   actions.FAILED: platform.EXECUTION_KNOWN_FAILED,
                   actions.EXPIRED: platform.EXECUTION_NOT_STARTED,
                   actions.CANCELLED: platform.EXECUTION_NOT_STARTED}[status]
    if not dispatch_attempt_id and outcome in (platform.EXECUTION_UNKNOWN,
                                               platform.EXECUTION_KNOWN_SUCCEEDED,
                                               platform.EXECUTION_KNOWN_FAILED):
        dispatch_attempt_id = str(uuid.uuid4())
    result = actions.ActionResult(action_id=action_id, status=status,
                                  execution_outcome=outcome,
                                  dispatch_attempt_id=dispatch_attempt_id)
    if status in TERMINAL:
        result.completed_at.GetCurrentTime()
    if error_code or status in (actions.FAILED, actions.EXPIRED):
        result.error.CopyFrom(common.Error(
            code=error_code or (common.TIMEOUT if status == actions.EXPIRED
                                else common.UPSTREAM_UNAVAILABLE),
            message='test'))
    return result


def response(request_id, *, result=None, error=None):
    if error is not None:
        return plugins.CoreToPlugin(action_response=plugins.PluginActionResponse(
            request_id=request_id, error=common.Error(code=error, message='test')))
    return plugins.CoreToPlugin(action_response=plugins.PluginActionResponse(
        request_id=request_id, result=result))


async def next_request(host):
    return await asyncio.wait_for(host.actions.get(), 2)


def within_deadline_limit(deadline):
    remaining = deadline.seconds + deadline.nanos / 1e9 - datetime.now(timezone.utc).timestamp()
    return 0 < remaining <= 300


class _Plugin(Plugin):
    pass


async def run_runner(host):
    stop = asyncio.Event()
    runner = Runner(manifest(), _Plugin(), options(host))
    task = asyncio.create_task(runner.run(stop))
    await ready(runner)

    async def finish():
        stop.set()
        await asyncio.wait_for(task, 2)
    return runner, finish


async def test_submit_action_preflight_and_correlation():
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            action = submission()
            action.idempotency_key = ''
            action.trace_id = ''
            action.deadline.Clear()
            pending = asyncio.create_task(runner.context.submit_action(
                action, idempotency_key='filled-key',
                deadline=datetime.now(timezone.utc) + timedelta(seconds=30)))
            frame = (await next_request(host)).action
            assert frame.request_id
            assert frame.action.action_id
            assert frame.action.idempotency_key == 'filled-key'
            assert frame.action.trace_id
            assert within_deadline_limit(frame.action.deadline)
            await host.send_frame(response(frame.request_id, result=action_result(
                action_id=frame.action.action_id)))
            result = await pending
            assert result.status == actions.ACCEPTED
            assert result.execution_outcome == platform.EXECUTION_NOT_STARTED
            assert not action_api.is_terminal(result)
            assert not action_api.may_have_executed(result)
        finally:
            await finish()


@pytest.mark.parametrize('mutation,code', [
    ('schedule_task', common.CAPABILITY_NOT_SUPPORTED),
    ('missing_payload', common.CAPABILITY_NOT_SUPPORTED),
    ('dispatch_attempt', common.INVALID_ARGUMENT),
    ('dispatch_sequence', common.INVALID_ARGUMENT),
    ('missing_key', common.INVALID_ARGUMENT),
    ('missing_deadline', common.INVALID_ARGUMENT),
    ('past_deadline', common.INVALID_ARGUMENT),
    ('far_deadline', common.INVALID_ARGUMENT),
    ('bad_action_id', common.INVALID_ARGUMENT),
])
async def test_submission_preflight_rejections_never_send(mutation, code):
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            action = submission()
            if mutation == 'schedule_task':
                action.ClearField('send_message')
                action.schedule_task.task_id = 'legacy'
            elif mutation == 'missing_payload':
                action.ClearField('send_message')
            elif mutation == 'dispatch_attempt':
                action.dispatch_attempt_id = str(uuid.uuid4())
            elif mutation == 'dispatch_sequence':
                action.dispatch_sequence = 1
            elif mutation == 'missing_key':
                action.idempotency_key = ''
            elif mutation == 'missing_deadline':
                action.deadline.Clear()
            elif mutation == 'past_deadline':
                action.deadline.FromDatetime(datetime.now(timezone.utc) - timedelta(seconds=1))
            elif mutation == 'far_deadline':
                action.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=301))
            elif mutation == 'bad_action_id':
                action.action_id = 'bad id'
            with pytest.raises(PluginError) as failure:
                await runner.context.submit_action(action)
            assert failure.value.code == code
            assert host.actions.empty()
        finally:
            await finish()


async def test_all_supported_action_kinds_exposed_and_schedule_task_rejected():
    kinds = {
        'send_message': actions.ACTION_KIND_SEND_MESSAGE,
        'react_message': actions.ACTION_KIND_REACT_MESSAGE,
        'delete_message': actions.ACTION_KIND_DELETE_MESSAGE,
        'mute_member': actions.ACTION_KIND_MUTE_MEMBER,
        'kick_member': actions.ACTION_KIND_KICK_MEMBER,
        'approve_request': actions.ACTION_KIND_APPROVE_REQUEST,
    }
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            for payload, kind in kinds.items():
                action = submission(action_id='kind-' + payload,
                                     idempotency_key='kind-' + payload + '-key',
                                     payload=payload)
                assert action_api.action_kind(action) == kind
                pending = asyncio.create_task(runner.context.submit_action(action))
                frame = (await next_request(host)).action
                assert frame.action.WhichOneof('payload') == payload
                await host.send_frame(response(frame.request_id, result=action_result(
                    action_id=action.action_id)))
                assert (await pending).action_id == action.action_id
            legacy = submission(action_id='legacy')
            legacy.ClearField('send_message')
            legacy.schedule_task.task_id = 'legacy'
            with pytest.raises(PluginError) as failure:
                await runner.context.submit_action(legacy)
            assert failure.value.code == common.CAPABILITY_NOT_SUPPORTED
            assert host.actions.empty()
        finally:
            await finish()


async def test_duplicate_submission_returns_original_action_snapshot():
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            first = submission(action_id='first-action')
            pending = asyncio.create_task(runner.context.submit_action(first))
            frame = (await next_request(host)).action
            original = action_result(action_id='original-action')
            await host.send_frame(response(frame.request_id, result=original))
            assert (await pending).action_id == 'original-action'

            # Same idempotency key/content may be submitted with a fresh action_id; Runtime
            # replies with the original snapshot (receipt DUPLICATE equivalent).
            again = submission(action_id='second-action', idempotency_key='key')
            pending = asyncio.create_task(runner.context.submit_action(again))
            frame = (await next_request(host)).action
            assert frame.action.idempotency_key == 'key'
            await host.send_frame(response(frame.request_id, result=original))
            assert (await pending).action_id == 'original-action'
        finally:
            await finish()


async def test_late_unknown_result_is_evidence_only_not_failed():
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            action = submission()
            pending = asyncio.create_task(runner.context.submit_action(action))
            frame = (await next_request(host)).action
            expired = action_result(action_id=action.action_id, status=actions.EXPIRED,
                                    outcome=platform.EXECUTION_UNKNOWN,
                                    error_code=common.TIMEOUT)
            await host.send_frame(response(frame.request_id, result=expired))
            result = await pending
            assert result.status == actions.EXPIRED
            assert result.execution_outcome == platform.EXECUTION_UNKNOWN
            assert result.dispatch_attempt_id
            assert action_api.is_terminal(result)
            assert action_api.is_unknown(result)
            assert action_api.may_have_executed(result)
            assert result.status != actions.FAILED
        finally:
            await finish()


async def test_conflict_error_maps_to_stable_code_without_retry():
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            pending = asyncio.create_task(runner.context.submit_action(submission()))
            frame = (await next_request(host)).action
            await host.send_frame(response(frame.request_id, error=common.CONFLICT))
            with pytest.raises(PluginError) as failure:
                await pending
            assert failure.value.code == common.CONFLICT
            assert host.actions.empty()
        finally:
            await finish()


@pytest.mark.parametrize('mode', ['missing_outcome', 'wrong_outcome', 'failed_without_error',
                                  'expired_wrong_code', 'succeeded_with_error',
                                  'unknown_without_attempt', 'terminal_without_completed_at',
                                  'unknown_status'])
async def test_malformed_results_stay_unknown_never_failed(mode):
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            pending = asyncio.create_task(runner.context.query_action(action_id='action'))
            request = (await next_request(host)).action_query
            if mode == 'missing_outcome':
                result = actions.ActionResult(action_id='action', status=actions.ACCEPTED)
            elif mode == 'wrong_outcome':
                result = action_result(status=actions.ACCEPTED,
                                       outcome=platform.EXECUTION_KNOWN_SUCCEEDED)
            elif mode == 'failed_without_error':
                result = action_result(status=actions.FAILED,
                                       outcome=platform.EXECUTION_KNOWN_FAILED)
                result.ClearField('error')
            elif mode == 'expired_wrong_code':
                result = action_result(status=actions.EXPIRED,
                                       outcome=platform.EXECUTION_NOT_STARTED,
                                       error_code=common.CONFLICT)
            elif mode == 'succeeded_with_error':
                result = action_result(status=actions.SUCCEEDED)
                result.error.CopyFrom(common.Error(code=common.UPSTREAM_UNAVAILABLE))
            elif mode == 'unknown_without_attempt':
                result = action_result(status=actions.ACCEPTED,
                                       outcome=platform.EXECUTION_UNKNOWN)
                result.dispatch_attempt_id = ''
            elif mode == 'terminal_without_completed_at':
                result = action_result(status=actions.SUCCEEDED)
                result.ClearField('completed_at')
            else:
                result = actions.ActionResult(action_id='action', status=999,
                                              execution_outcome=platform.EXECUTION_UNKNOWN,
                                              dispatch_attempt_id=str(uuid.uuid4()))
            await host.send_frame(response(request.request_id, result=result))
            with pytest.raises(PluginError) as failure:
                await pending
            assert failure.value.code == common.UPSTREAM_UNAVAILABLE
            assert failure.value.retryable
        finally:
            await finish()


async def test_query_correlation_and_attempt_mapping():
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            attempt = str(uuid.uuid4())
            pending = asyncio.create_task(runner.context.query_action(action_id='action'))
            request = (await next_request(host)).action_query
            assert request.action_id == 'action' and request.deadline.seconds > 0
            await host.send_frame(response(request.request_id, result=action_result(
                action_id='action', status=actions.SUCCEEDED,
                dispatch_attempt_id=attempt)))
            result = await pending
            assert result.dispatch_attempt_id == attempt
            assert result.execution_outcome == platform.EXECUTION_KNOWN_SUCCEEDED

            # Query by idempotency key cannot correlate a client-known action_id.
            pending = asyncio.create_task(runner.context.query_action(idempotency_key='key'))
            request = (await next_request(host)).action_query
            await host.send_frame(response(request.request_id, result=action_result(
                action_id='server-action')))
            assert (await pending).action_id == 'server-action'

            # A response correlated to the wrong action stays an unknown transport error.
            pending = asyncio.create_task(runner.context.query_action(action_id='action'))
            request = (await next_request(host)).action_query
            await host.send_frame(response(request.request_id, result=action_result(
                action_id='other-action')))
            with pytest.raises(PluginError) as failure:
                await pending
            assert failure.value.code == common.UPSTREAM_UNAVAILABLE
        finally:
            await finish()


async def test_cancel_semantics_and_dispatched_race():
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            pending = asyncio.create_task(runner.context.cancel_action('action'))
            request = (await next_request(host)).action_cancel
            assert request.action_id == 'action' and request.trace_id
            await host.send_frame(response(request.request_id, result=action_result(
                action_id='action', status=actions.CANCELLED,
                outcome=platform.EXECUTION_NOT_STARTED)))
            cancelled = await pending
            assert cancelled.status == actions.CANCELLED
            assert not action_api.may_have_executed(cancelled)

            # Cancel after dispatch is CONFLICT; the SDK reports it, it never fabricates a result.
            pending = asyncio.create_task(runner.context.cancel_action('action'))
            request = (await next_request(host)).action_cancel
            await host.send_frame(response(request.request_id, error=common.CONFLICT))
            with pytest.raises(PluginError) as failure:
                await pending
            assert failure.value.code == common.CONFLICT

            for bad in ('', 'bad id', None):
                with pytest.raises(PluginError) as failure:
                    await runner.context.cancel_action(bad)
                assert failure.value.code == common.INVALID_ARGUMENT
            with pytest.raises(PluginError) as failure:
                await runner.context.cancel_action('action', timeout=301)
            assert failure.value.code == common.INVALID_ARGUMENT
        finally:
            await finish()


async def test_action_feature_not_negotiated_fails_locally():
    async with FakeTargetRuntime() as host:
        runner, finish = await run_runner(host)
        try:
            for call in (runner.context.submit_action(submission()),
                         runner.context.query_action(action_id='action'),
                         runner.context.cancel_action('action')):
                with pytest.raises(PluginError) as failure:
                    await call
                assert failure.value.code == common.CAPABILITY_NOT_SUPPORTED
            assert host.actions.empty()
        finally:
            await finish()



async def test_action_in_event_callback_and_out_of_order_responses():
    class Sender(Plugin):
        async def on_event(self, event, context):
            assert (await context.submit_action(submission())).status == actions.ACCEPTED

    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        stop = asyncio.Event()
        runner = Runner(manifest(), Sender(), options(host))
        task = asyncio.create_task(runner.run(stop))
        try:
            await ready(runner)
            await host.send_event(events.EventEnvelope(event_id='event-1', trace_id='trace-1'))
            request = (await next_request(host)).action
            assert request.action.action_id == 'sent'
            await host.send_frame(response(request.request_id, result=action_result()))
            ack = await asyncio.wait_for(host.acknowledgements.get(), 2)
            assert ack.accepted and ack.attempt.event_id == 'event-1'

            # Out-of-order responses are matched by request_id only.
            pending = [asyncio.create_task(runner.context.query_action(action_id='first')),
                       asyncio.create_task(runner.context.query_action(idempotency_key='second'))]
            requests = [(await next_request(host)).action_query for _ in range(2)]
            for query in reversed(requests):
                key = query.action_id or query.idempotency_key
                await host.send_frame(response(query.request_id, result=action_result(
                    action_id=key, status=actions.SUCCEEDED)))
            assert [value.action_id for value in await asyncio.gather(*pending)] == [
                'first', 'second']
        finally:
            stop.set()
            await asyncio.wait_for(task, 2)


async def test_disconnect_before_submit_response_then_query_does_not_replay():
    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        runner, finish = await run_runner(host)
        try:
            pending = asyncio.create_task(runner.context.submit_action(submission()))
            request = (await next_request(host)).action
            await host.outgoing.put(None)
            with pytest.raises(PluginError) as failure:
                await pending
            assert failure.value.code == common.UPSTREAM_UNAVAILABLE
            # The session loop reconnects on its own; never resend a possibly executed Action.
            registration = await asyncio.wait_for(host.registrations.get(), 3)
            assert registration.instance_id
            await ready(runner)
            query = asyncio.create_task(runner.context.query_action(idempotency_key='key'))
            frame = (await next_request(host)).action_query
            await host.send_frame(response(frame.request_id, result=action_result(
                action_id='server-action', status=actions.SUCCEEDED)))
            assert (await query).action_id == 'server-action'
            await asyncio.sleep(.05)
            # Exactly one submit frame was ever sent; recovery used query only.
            assert sum(1 for kind, _ in host.action_requests if kind == 'action') == 1
            assert request.action.action_id == 'sent'
        finally:
            await finish()


async def test_fake_action_policy_answers_all_three_frames():
    responses = {
        'action': {'result': action_result(action_id='policy-action')},
        'action_query': {'result': action_result(action_id='policy-action',
                                                 status=actions.SUCCEEDED)},
        'action_cancel': {'error': common.CONFLICT},
    }

    async with FakeTargetRuntime() as host:
        host.features = [action_api.ACTION_FEATURE]
        host.action_policy = lambda kind, request, sequence: responses[kind]
        runner, finish = await run_runner(host)
        try:
            submitted = await runner.context.submit_action(submission())
            assert submitted.action_id == 'policy-action'
            assert submitted.status == actions.ACCEPTED
            queried = await runner.context.query_action(action_id='policy-action')
            assert queried.status == actions.SUCCEEDED
            with pytest.raises(PluginError) as failure:
                await runner.context.cancel_action('policy-action')
            assert failure.value.code == common.CONFLICT
            assert [kind for kind, _ in host.action_requests] == [
                'action', 'action_query', 'action_cancel']
        finally:
            await finish()
