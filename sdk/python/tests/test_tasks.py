import asyncio
from datetime import datetime, timedelta, timezone
import re
import signal

import pytest
from kiarabot.v1 import plugins_pb2 as plugins, common_pb2 as common
from kiarabot_sdk import Options, Plugin, PluginError, Runner
from kiarabot_sdk.testing import FakeHost
from test_runtime import get, manifest, options
from test_actions import ready
from test_process import environment, launch, cleanup


async def test_tasks_real_core_restart_and_timer_delivery(tmp_path):
    env = environment(tmp_path, 'graceful')
    for key in ('KIARABOT_PLUGIN_EXECUTABLE', 'KIARABOT_PLUGIN_MANIFEST', 'KIARABOT_PLUGIN_PYTHONPATH'):
        env.pop(key)
    env['KIARABOT_PLUGIN_PERMISSIONS'] += ',task.schedule'
    policy = manifest()
    policy.permissions.append('task.schedule')
    policy.capabilities.append('event.timer.triggered')
    received = asyncio.Queue()
    class Timer(Plugin):
        async def on_event(self, event, context):
            await received.put(event)
    run_at = datetime.now(timezone.utc) + timedelta(seconds=2)
    for attempt in range(2):
        process = await launch(env)
        task = None
        stop = asyncio.Event()
        try:
            line = await asyncio.wait_for(process.stdout.readline(), 5)
            endpoint = '127.0.0.1:' + re.search(rb'port=(\d+)', line)[1].decode()
            runner = Runner(policy, Timer(), Options(endpoint, 'p' * 32, heartbeat_interval=.05))
            task = asyncio.create_task(runner.run(stop))
            await ready(runner)
            if attempt == 0:
                response = await runner.context.task_create('timer', 'conversation', b'binary\0payload',
                    'timer-create-idempotency', run_at=run_at)
                assert response.state == plugins.SCHEDULED_TASK_PENDING
                replay = await runner.context.task_create('timer', 'conversation', b'binary\0payload',
                    'timer-create-idempotency', run_at=run_at)
                assert replay == response
                with pytest.raises(PluginError) as conflict:
                    await runner.context.task_create('timer', 'conversation', b'changed',
                        'timer-create-idempotency', run_at=run_at)
                assert conflict.value.code == common.CONFLICT
                await runner.context.task_create('cancelled', 'conversation', b'never',
                    'timer-cancel-create-idempotency', run_at=run_at)
                assert (await runner.context.task_cancel('cancelled', 'timer-cancel-idempotency')).state == plugins.SCHEDULED_TASK_CANCELLED
                assert (await runner.context.task_get('timer')).state == plugins.SCHEDULED_TASK_PENDING
                stop.set()
                await asyncio.wait_for(task, 3)
                task = None
                process.kill()  # Crash with durable PENDING; recovery must not lose it.
                await process.wait()
            else:
                event = await asyncio.wait_for(received.get(), 5)
                assert event.timer_triggered.task_id == 'timer'
                assert event.timer_triggered.owner_plugin_id == policy.id
                assert event.timer_triggered.payload == b'binary\0payload'
                assert event.subject.conversation_id == 'conversation'
                assert event.timer_triggered.scheduled_at.ToDatetime(tzinfo=timezone.utc) == run_at
                assert (await runner.context.task_get('timer')).state == plugins.SCHEDULED_TASK_COMPLETED
                assert (await runner.context.task_get('cancelled')).state == plugins.SCHEDULED_TASK_CANCELLED
                with pytest.raises(PluginError) as terminal:
                    await runner.context.task_cancel('timer', 'cancel-completed-idempotency')
                assert terminal.value.code == common.CONFLICT
                await asyncio.sleep(.2)
                assert received.empty()
        finally:
            stop.set()
            if task:
                await asyncio.wait_for(task, 3)
            if process.returncode is None:
                process.send_signal(signal.SIGTERM)
                await asyncio.wait_for(process.communicate(), 5)
            await cleanup(process)


async def test_task_sdk_requires_feature_and_unambiguous_time():
    async with FakeHost() as host:
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
    async with FakeHost() as host:
        host.features = ['host.tasks.v1']
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
            await host.outgoing.put(plugins.CoreToPlugin(host_response=plugins.HostResponse(
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
