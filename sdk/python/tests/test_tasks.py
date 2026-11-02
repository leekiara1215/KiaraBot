"""P10c plugin task client: frozen task frames, validation and stable errors."""
import asyncio
from datetime import datetime, timedelta, timezone

import pytest
from kiarabot.v1 import common_pb2 as common, plugins_pb2 as plugins
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk import tasks as task_api
from kiarabot_sdk.testing import FakeTargetRuntime
from test_actions import ready
from test_runtime import get, manifest, options


def task_manifest(*, permissions=('task.schedule',), capabilities=('event.timer.triggered',)):
    policy = manifest()
    for permission in permissions:
        if permission not in policy.permissions:
            policy.permissions.append(permission)
    for capability in capabilities:
        if capability not in policy.capabilities:
            policy.capabilities.append(capability)
    return policy


def local_time(**overrides):
    value = {'year': 2030, 'month': 3, 'day': 9, 'hour': 2, 'minute': 30,
             'second': 0, 'time_zone': 'America/New_York'}
    value.update(overrides)
    return value


def future(seconds=300):
    return datetime.now(timezone.utc) + timedelta(seconds=seconds)


async def run(host, policy=None):
    stop = asyncio.Event()
    runner = Runner(policy or task_manifest(), Plugin(), options(host))
    task = asyncio.create_task(runner.run(stop))
    await ready(runner)
    return runner, stop, task


async def finish(stop, task):
    stop.set()
    await asyncio.wait_for(task, 2)


def respond(request, *, result=None, error=None, operation=None, retryable=False):
    response = plugins.HostResponse(request_id=request.request_id)
    if error is not None:
        response.error.CopyFrom(common.Error(code=error, message=common.ErrorCode.Name(error),
                                             retryable=retryable))
    else:
        getattr(response, operation).CopyFrom(result)
    return plugins.CoreToPlugin(host_response=response)


def task_response(task_id='task-1', state=common.SCHEDULED_TASK_PENDING, **kwargs):
    return plugins.TaskResponse(task_id=task_id, state=state, **kwargs)


async def test_task_create_maps_frozen_frame_and_validates_response():
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            pending = asyncio.create_task(runner.context.task_create(
                'task-1', 'conversation-1', b'payload', 'idempotency-key-1',
                run_at=future()))
            request = await get(host.host_requests)
            assert request.task_create.task_id == 'task-1'
            assert request.task_create.target_conversation == 'conversation-1'
            assert request.task_create.payload == b'payload'
            assert request.task_create.WhichOneof('when') == 'run_at'
            assert request.idempotency_key == 'idempotency-key-1'
            assert request.deadline.seconds > 0
            await host.send_frame(respond(
                request, result=task_response(), operation='task_create'))
            result = await pending
            assert result.state == common.SCHEDULED_TASK_PENDING
            assert result.task_id == 'task-1'
            assert not task_api.is_terminal(result.state)
            assert not task_api.handed_off(result.state)
        finally:
            await finish(stop, running)


async def test_local_time_is_forwarded_without_local_dst_logic():
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            pending = asyncio.create_task(runner.context.task_create(
                'task-1', 'conversation-1', b'payload', 'idempotency-key-1',
                local_time=local_time(hour=2, minute=30)))
            request = await get(host.host_requests)
            local = request.task_create.local_time
            assert request.task_create.WhichOneof('when') == 'local_time'
            assert (local.year, local.month, local.day) == (2030, 3, 9)
            assert local.time_zone == 'America/New_York'
            await host.send_frame(respond(
                request, result=task_response(), operation='task_create'))
            assert (await pending).state == common.SCHEDULED_TASK_PENDING
        finally:
            await finish(stop, running)


@pytest.mark.parametrize('change', [
    {'task_id': ''}, {'task_id': 'bad id'}, {'task_id': 'x' * 129},
    {'target_conversation': ''}, {'target_conversation': 'x' * 257},
    {'payload': 'not-bytes'}, {'payload': b'x' * (64 * 1024 + 1)},
    {'idempotency_key': ''}, {'idempotency_key': 'bad key'},
    {'run_at': datetime.now()}, {'run_at': datetime(2030, 1, 1)},
    {'run_at': datetime.now(timezone(timedelta(hours=1)))},
    {'run_at': datetime.now(timezone.utc) - timedelta(seconds=1)},
    {'local_time': local_time(year=1969)}, {'local_time': local_time(year=2101)},
    {'local_time': local_time(month=13)}, {'local_time': local_time(day=32)},
    {'local_time': local_time(day=31, month=2)},
    {'local_time': local_time(hour=24)}, {'local_time': local_time(minute=60)},
    {'local_time': local_time(second=60)},
    {'local_time': local_time(time_zone='')},
    {'local_time': local_time(time_zone='/etc/passwd')},
    {'local_time': local_time(time_zone='America/../New_York')},
    {'local_time': {**local_time(), 'extra': 1}},
    {'local_time': {key: value for key, value in local_time().items() if key != 'time_zone'}},
])
async def test_create_preflight_rejects_without_sending(change):
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            kwargs = dict(task_id='task-1', target_conversation='conversation-1',
                          payload=b'payload', idempotency_key='idempotency-key-1',
                          run_at=future())
            kwargs.update(change)
            if 'run_at' in change:
                kwargs.pop('local_time', None)
            with pytest.raises(PluginError) as failure:
                await runner.context.task_create(**kwargs)
            assert failure.value.code == common.INVALID_ARGUMENT
            assert host.host_requests.empty()
        finally:
            await finish(stop, running)


@pytest.mark.parametrize('run_at,local_time_value', [
    (None, None),
])
async def test_create_requires_exactly_one_time_form(run_at, local_time_value):
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            with pytest.raises(PluginError) as failure:
                await runner.context.task_create(
                    'task-1', 'conversation-1', b'payload', 'idempotency-key-1',
                    run_at=run_at, local_time=local_time_value)
            assert failure.value.code == common.INVALID_ARGUMENT
            assert host.host_requests.empty()
        finally:
            await finish(stop, running)


async def test_both_time_forms_are_rejected():
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            with pytest.raises(PluginError) as failure:
                await runner.context.task_create(
                    'task-1', 'conversation-1', b'payload', 'idempotency-key-1',
                    run_at=future(), local_time=local_time())
            assert failure.value.code == common.INVALID_ARGUMENT
            assert host.host_requests.empty()
        finally:
            await finish(stop, running)


async def test_task_permission_and_timer_subscription_gates():
    # Registered without task.schedule: local PERMISSION_DENIED, no frame.
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host, manifest())
        try:
            with pytest.raises(PluginError) as failure:
                await runner.context.task_get('task-1')
            assert failure.value.code == common.PERMISSION_DENIED
            assert host.host_requests.empty()
        finally:
            await finish(stop, running)

    # Permission granted but no event.timer.triggered subscription: create fails locally,
    # queries remain usable.
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host, task_manifest(capabilities=()))
        try:
            with pytest.raises(PluginError) as failure:
                await runner.context.task_create(
                    'task-1', 'conversation-1', b'payload', 'idempotency-key-1',
                    run_at=future())
            assert failure.value.code == common.CAPABILITY_NOT_SUPPORTED
            assert host.host_requests.empty()
            pending = asyncio.create_task(runner.context.task_get('task-1'))
            request = await get(host.host_requests)
            await host.send_frame(respond(request, result=task_response(), operation='task_get'))
            assert (await pending).state == common.SCHEDULED_TASK_PENDING
        finally:
            await finish(stop, running)


async def test_task_feature_not_negotiated_fails_locally():
    async with FakeTargetRuntime(features=[]) as host:
        runner, stop, running = await run(host)
        try:
            with pytest.raises(PluginError) as failure:
                await runner.context.task_get('task-1')
            assert failure.value.code == common.CAPABILITY_NOT_SUPPORTED
            with pytest.raises(PluginError) as failure:
                await runner.context.task_cancel('task-1', 'idempotency-key-1')
            assert failure.value.code == common.CAPABILITY_NOT_SUPPORTED
            with pytest.raises(PluginError) as failure:
                await runner.context.task_create(
                    'task-1', 'conversation-1', b'payload', 'idempotency-key-1',
                    run_at=future())
            assert failure.value.code == common.CAPABILITY_NOT_SUPPORTED
            assert host.host_requests.empty()
        finally:
            await finish(stop, running)


async def test_get_and_cancel_correlate_and_surface_terminal_states():
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            get_call = asyncio.create_task(runner.context.task_get('task-1'))
            request = await get(host.host_requests)
            assert request.WhichOneof('operation') == 'task_get'
            assert request.task_get.task_id == 'task-1'
            assert request.idempotency_key == ''
            await host.send_frame(respond(request, result=task_response(
                state=common.SCHEDULED_TASK_COMPLETED, version=3,
                occurrence_id='occurrence-1'), operation='task_get'))
            completed = await get_call
            assert completed.state == common.SCHEDULED_TASK_COMPLETED
            assert completed.occurrence_id == 'occurrence-1'
            assert task_api.is_terminal(completed.state)
            assert task_api.handed_off(completed.state)

            cancel = asyncio.create_task(
                runner.context.task_cancel('task-1', 'idempotency-key-1'))
            request = await get(host.host_requests)
            assert request.WhichOneof('operation') == 'task_cancel'
            assert request.idempotency_key == 'idempotency-key-1'
            await host.send_frame(respond(request, result=task_response(
                state=common.SCHEDULED_TASK_CANCELLED), operation='task_cancel'))
            cancelled = await cancel
            assert cancelled.state == common.SCHEDULED_TASK_CANCELLED
            assert task_api.is_terminal(cancelled.state)
            assert not task_api.handed_off(cancelled.state)
        finally:
            await finish(stop, running)


@pytest.mark.parametrize('code,retryable', [
    (common.CONFLICT, False), (common.NOT_FOUND, False),
    (common.RATE_LIMITED, True), (common.UPSTREAM_UNAVAILABLE, True),
    (common.PERMISSION_DENIED, False),
])
async def test_task_errors_keep_stable_codes(code, retryable):
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            pending = asyncio.create_task(runner.context.task_cancel(
                'task-1', 'idempotency-key-1'))
            request = await get(host.host_requests)
            await host.send_frame(respond(request, error=code, retryable=retryable))
            with pytest.raises(PluginError) as failure:
                await pending
            assert failure.value.code == code
            assert failure.value.retryable is retryable
        finally:
            await finish(stop, running)


@pytest.mark.parametrize('bad', ['wrong-id', 'unknown-state', 'bad-occurrence'])
async def test_malformed_task_response_stays_unknown(bad):
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            pending = asyncio.create_task(runner.context.task_get('task-1'))
            request = await get(host.host_requests)
            if bad == 'wrong-id':
                result = task_response(task_id='other-task')
            elif bad == 'unknown-state':
                result = task_response(state=99)
            else:
                result = task_response(occurrence_id='bad occurrence')
            await host.send_frame(respond(request, result=result, operation='task_get'))
            with pytest.raises(PluginError) as failure:
                await pending
            assert failure.value.code == common.UPSTREAM_UNAVAILABLE
            assert failure.value.retryable
        finally:
            await finish(stop, running)


async def test_timeout_is_unknown_and_caller_reuses_idempotency_key():
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner, stop, running = await run(host)
        try:
            with pytest.raises(PluginError) as failure:
                await runner.context.task_create(
                    'task-1', 'conversation-1', b'payload', 'idempotency-key-1',
                    run_at=future(), timeout=.05)
            assert failure.value.code == common.TIMEOUT
            assert failure.value.retryable
            first = await get(host.host_requests)
            assert first.idempotency_key == 'idempotency-key-1'
            # The caller retries explicitly with the same identity; the SDK never auto-replays.
            retry = asyncio.create_task(runner.context.task_create(
                'task-1', 'conversation-1', b'payload', 'idempotency-key-1',
                run_at=future()))
            second = await get(host.host_requests)
            assert second.idempotency_key == first.idempotency_key
            assert second.task_create.task_id == first.task_create.task_id
            await host.send_frame(respond(
                second, result=task_response(), operation='task_create'))
            assert (await retry).task_id == 'task-1'
        finally:
            await finish(stop, running)


def test_plugin_bridge_has_no_task_list_operation():
    """Frozen proto exposes create/get/cancel only; ListTasks is a Control/Runtime RPC."""
    request = plugins.HostRequest()
    fields = {field.name for field in request.DESCRIPTOR.oneofs_by_name['operation'].fields}
    assert fields == {'model_chat', 'storage_get', 'storage_set', 'storage_delete',
                      'task_create', 'task_cancel', 'task_get',
                      'storage_compare_and_set'}
    assert not hasattr(plugins, 'TaskListRequest')
    assert not hasattr(Runner, 'list_tasks')
