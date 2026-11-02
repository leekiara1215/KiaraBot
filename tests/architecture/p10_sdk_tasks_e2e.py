#!/usr/bin/env python3
"""P10j SDK task methods over a real TLS PluginBridge protocol fixture and fake Scheduler.

The shipped SDK Runner connects to the real-TLS P06 ``PluginBridge`` fixture
(``kiarabot_sdk.testing.FakeTargetRuntime``) and exercises ``task_create``/``task_get``/
``task_cancel`` end to end; a fake scheduler owner implements the P10 task state machine,
capacity/DST/ownership errors. This proves the SDK-to-bridge methods and error mapping.

Production ``Runtime`` now consumes the three task frames (P10k), but its Runtime->Scheduler
delegation provider is intentionally fail-closed until the delegation source is confirmed. This
SDK-level fixture therefore remains the SDK/bridge evidence and is not native-stack acceptance.
"""
from __future__ import annotations

import asyncio
from datetime import datetime, timedelta, timezone
import sys
from pathlib import Path
import zoneinfo

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / 'sdk/python/src'), str(REPO / 'sdk/python/build/generated')]
from kiarabot.v1 import common_pb2 as common, plugins_pb2 as plugins  # noqa: E402
from kiarabot_sdk import Options, Plugin, PluginError, Runner  # noqa: E402
from kiarabot_sdk import tasks as task_api  # noqa: E402
from kiarabot_sdk.testing import FakeTargetRuntime  # noqa: E402


class Subject(Plugin):
    pass


def manifest():
    value = plugins.PluginManifest(
        id='p10j.e2e.plugin', name='P10j E2E', version='1.0.0',
        api_version='kiarabot.plugin.v1', runtime='python-rpc', isolation='process',
        entrypoint='p10j_subject:create', ordered=False,
        capabilities=['event.timer.triggered'], permissions=['task.schedule'])
    return value


def response(request, *, task=None, occurrence=None, error=None, retryable=False):
    body = plugins.HostResponse(request_id=request.request_id)
    if error is not None:
        body.error.CopyFrom(common.Error(code=error, message=common.ErrorCode.Name(error),
                                         retryable=retryable))
    elif request.WhichOneof('operation') == 'task_create':
        body.task_create.CopyFrom(task)
    elif request.WhichOneof('operation') == 'task_get':
        body.task_get.CopyFrom(plugins.TaskResponse(task_id=task.task_id, state=task.state,
                                                    version=task.version,
                                                    occurrence_id=task.occurrence_id))
    else:
        body.task_cancel.CopyFrom(plugins.TaskResponse(task_id=task.task_id, state=task.state,
                                                       version=task.version))
    return plugins.CoreToPlugin(host_response=body)


class FakeScheduler:
    def __init__(self, host):
        self.host = host
        self.tasks = {}
        self.capacity = 2
        self.idempotency = {}

    async def run(self):
        while True:
            request = await self.host.host_requests.get()
            operation = request.WhichOneof('operation')
            answer = getattr(self, operation)(request)
            await self.host.send_frame(answer)

    def _task(self, request):
        return plugins.TaskResponse(
            task_id=request.task_create.task_id,
            state=common.SCHEDULED_TASK_PENDING, version=1)

    def task_create(self, request):
        task = request.task_create
        existing = self.tasks.get(task.task_id)
        if existing is not None:
            if self.idempotency.get((task.task_id, request.idempotency_key)) != task.payload:
                return response(request, error=common.CONFLICT)
            return response(request, task=existing)
        if len(self.tasks) >= self.capacity:
            return response(request, error=common.RATE_LIMITED, retryable=True)
        from google.protobuf.timestamp_pb2 import Timestamp
        now = datetime.now(timezone.utc)
        if task.WhichOneof('when') == 'run_at':
            run_at = task.run_at.ToDatetime(timezone.utc)
            if run_at <= now:
                return response(request, error=common.INVALID_ARGUMENT)
        else:
            local = task.local_time
            try:
                zoneinfo.ZoneInfo(local.time_zone)
            except (zoneinfo.ZoneInfoNotFoundError, ValueError):
                return response(request, error=common.INVALID_ARGUMENT)
            civil = datetime(local.year, local.month, local.day, local.hour, local.minute,
                             local.second, tzinfo=zoneinfo.ZoneInfo(local.time_zone))
            # Gap/fold: round-trip through UTC must preserve the civil wall clock. Fold is
            # represented by zoneinfo's fold flag; the fake Scheduler rejects both.
            if civil.astimezone(timezone.utc).astimezone(zoneinfo.ZoneInfo(local.time_zone)) != civil:
                return response(request, error=common.INVALID_ARGUMENT)
        created = self._task(request)
        self.tasks[task.task_id] = created
        self.idempotency[(task.task_id, request.idempotency_key)] = task.payload
        return response(request, task=created)

    def task_get(self, request):
        task = self.tasks.get(request.task_get.task_id)
        if task is None:
            return response(request, error=common.NOT_FOUND)
        return response(request, task=task)

    def task_cancel(self, request):
        current = self.tasks.get(request.task_cancel.task_id)
        if current is None:
            return response(request, error=common.NOT_FOUND)
        if current.state in (common.SCHEDULED_TASK_COMPLETED, common.SCHEDULED_TASK_RUNNING,
                             common.SCHEDULED_TASK_RETRYING):
            return response(request, error=common.CONFLICT)
        updated = plugins.TaskResponse(task_id=current.task_id, version=current.version + 1,
                                       state=common.SCHEDULED_TASK_CANCELLED)
        self.tasks[current.task_id] = updated
        return response(request, task=updated)


def local(**overrides):
    value = {'year': 2027, 'month': 3, 'day': 14, 'hour': 2, 'minute': 30, 'second': 0,
             'time_zone': 'America/New_York'}
    value.update(overrides)
    return value


async def wait_ready(host):
    await asyncio.wait_for(host.registrations.get(), 10)
    # The registration response is written after the registration event; poll the SDK context
    # until its bridge RPC is installed.
    deadline = asyncio.get_running_loop().time() + 5
    while asyncio.get_running_loop().time() < deadline:
        try:
            await asyncio.wait_for(host.host_requests.get(), 0.05)
        except asyncio.TimeoutError:
            continue
        break


async def main():
    async with FakeTargetRuntime(features=['host.tasks.v1']) as host:
        runner = Runner(manifest(), Subject(), Options(
            host.endpoint, host.token, tls_root_cert=host.root_cert,
            retry_delay=0.01, heartbeat_interval=0.05, lease_renew_interval=0.1))
        stop = asyncio.Event()
        running = asyncio.create_task(runner.run(stop))
        scheduler = FakeScheduler(host)
        owner = asyncio.create_task(scheduler.run())
        try:
            await wait_ready(host)
            future = datetime.now(timezone.utc) + timedelta(minutes=5)
            created = await runner.context.task_create(
                'task-1', 'conversation-1', b'payload', 'idem-1', run_at=future)
            assert created.task_id == 'task-1' and created.state == common.SCHEDULED_TASK_PENDING
            fetched = await runner.context.task_get('task-1')
            assert fetched.state == common.SCHEDULED_TASK_PENDING and fetched.version == 1
            # Idempotent replay is the same task; same key with different content conflicts.
            replay = await runner.context.task_create(
                'task-1', 'conversation-1', b'payload', 'idem-1', run_at=future)
            assert replay.state == common.SCHEDULED_TASK_PENDING
            try:
                await runner.context.task_create(
                    'task-1', 'conversation-1', b'changed', 'idem-1', run_at=future)
                raise AssertionError('changed payload under the same key was accepted')
            except PluginError as failure:
                assert failure.code == common.CONFLICT
            cancelled = await runner.context.task_cancel('task-1', 'cancel-1')
            assert cancelled.state == common.SCHEDULED_TASK_CANCELLED
            repeated = await runner.context.task_cancel('task-1', 'cancel-1')
            assert repeated.state == common.SCHEDULED_TASK_CANCELLED

            # Capacity is Scheduler-owned; the SDK surfaces retryable RATE_LIMITED unchanged.
            await runner.context.task_create('task-2', 'conversation-1', b'payload', 'idem-2',
                                             run_at=future)
            try:
                await runner.context.task_create('task-3', 'conversation-1', b'payload', 'idem-3',
                                                 run_at=future)
                raise AssertionError('capacity overflow was accepted')
            except PluginError as failure:
                assert failure.code == common.RATE_LIMITED and failure.retryable
            scheduler.capacity = 10

            # DST gap and unknown zone stay stable INVALID_ARGUMENT; the SDK does not convert.
            for bad in (local(), local(time_zone='No/Such_Zone')):
                try:
                    await runner.context.task_create(
                        'task-dst', 'conversation-1', b'payload', 'idem-dst', local_time=bad)
                    raise AssertionError(f'invalid local time accepted: {bad}')
                except PluginError as failure:
                    assert failure.code == common.INVALID_ARGUMENT
            try:
                await runner.context.task_get('task-other-instance')
                raise AssertionError('cross-instance/unknown task was returned')
            except PluginError as failure:
                assert failure.code == common.NOT_FOUND

            # COMPLETED proves event enqueue only; it is terminal and cancelling it conflicts.
            scheduler.tasks['task-2'] = plugins.TaskResponse(
                task_id='task-2', state=common.SCHEDULED_TASK_COMPLETED, version=2,
                occurrence_id='occurrence-1')
            completed = await runner.context.task_get('task-2')
            assert completed.state == common.SCHEDULED_TASK_COMPLETED
            assert task_api.is_terminal(completed.state)
            assert completed.occurrence_id == 'occurrence-1'
            try:
                await runner.context.task_cancel('task-2', 'cancel-2')
                raise AssertionError('cancel of COMPLETED task was accepted')
            except PluginError as failure:
                assert failure.code == common.CONFLICT
        finally:
            stop.set()
            owner.cancel()
            running.cancel()
            for task in (owner, running):
                try:
                    await asyncio.wait_for(task, 2)
                except (asyncio.CancelledError, asyncio.TimeoutError):
                    pass
    print('P10j SDK task methods over real TLS PluginBridge fixture passed')


if __name__ == '__main__':
    asyncio.run(main())
