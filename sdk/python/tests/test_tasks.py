import asyncio
from datetime import datetime, timedelta, timezone

import pytest
from kiarabot.v1 import common_pb2 as common, plugins_pb2 as plugins
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk.testing import FakeTargetRuntime
from test_runtime import get, manifest, options
from test_actions import ready


async def test_task_sdk_requires_feature_and_unambiguous_time():
    async with FakeTargetRuntime() as host:
        stop = asyncio.Event()
        runner = Runner(manifest(), Plugin(), options(host))
        task = asyncio.create_task(runner.run(stop))
        try:
            await ready(runner)
            with pytest.raises(PluginError) as unsupported:
                await runner.context.task_get('task')
            assert unsupported.value.code == common.CAPABILITY_NOT_SUPPORTED
            with pytest.raises(PluginError) as invalid:
                await runner.context.task_create('task', 'conversation', b'', 'idempotency-key-1',
                    run_at=datetime.now())
            assert invalid.value.code == common.INVALID_ARGUMENT
            assert host.host_requests.empty()
        finally:
            stop.set()
            await asyncio.wait_for(task, 3)


async def test_task_local_time_is_forwarded_without_local_dst_logic():
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        policy = manifest()
        policy.permissions.append('task.schedule')
        policy.capabilities.append('event.timer.triggered')
        stop = asyncio.Event()
        runner = Runner(policy, Plugin(), options(host))
        task = asyncio.create_task(runner.run(stop))
        try:
            await ready(runner)
            local = {'year': 2030, 'month': 3, 'day': 9, 'hour': 2, 'minute': 30,
                     'second': 0, 'time_zone': 'America/New_York'}
            pending = asyncio.create_task(runner.context.task_create(
                'task-1', 'conversation-1', b'payload', 'idempotency-key-1', local_time=local))
            request = await get(host.host_requests)
            assert request.task_create.local_time.time_zone == 'America/New_York'
            assert (request.task_create.local_time.hour,
                    request.task_create.local_time.minute) == (2, 30)
            assert request.task_create.WhichOneof('when') == 'local_time'
            await host.send_frame(plugins.CoreToPlugin(host_response=plugins.HostResponse(
                request_id=request.request_id,
                task_create=plugins.TaskResponse(task_id='task-1',
                                                 state=plugins.SCHEDULED_TASK_PENDING))))
            assert (await pending).state == plugins.SCHEDULED_TASK_PENDING
            for kwargs in ({}, {'run_at': datetime.now(timezone.utc), 'local_time': local}):
                with pytest.raises(PluginError) as failure:
                    await runner.context.task_create('task-2', 'conversation-1', b'payload',
                                                     'idempotency-key-2', **kwargs)
                assert failure.value.code == common.INVALID_ARGUMENT
        finally:
            stop.set()
            await asyncio.wait_for(task, 2)
