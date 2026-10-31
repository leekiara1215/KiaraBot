import asyncio
from contextlib import suppress
import re
import signal

import pytest
from kiarabot.v1 import plugins_pb2 as plugins, common_pb2 as common
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk.testing import FakeHost
from test_runtime import manifest, options, get
from test_actions import ready
from test_process import environment, launch, cleanup


@pytest.mark.parametrize('mode,code', [('legacy', common.CAPABILITY_NOT_SUPPORTED), ('timeout', common.TIMEOUT),
    ('wrong_result', common.UPSTREAM_UNAVAILABLE), ('both', common.UPSTREAM_UNAVAILABLE), ('denied', common.PERMISSION_DENIED)])
async def test_host_rejection_and_malformed_response(mode, code):
    async with FakeHost() as host:
        host.features = [] if mode == 'legacy' else ['host.storage.v1']
        runner, stop = Runner(manifest(), Plugin(), options(host)), asyncio.Event()
        task = asyncio.create_task(runner.run(stop))
        pending = None
        try:
            await ready(runner)
            pending = asyncio.create_task(runner.context.storage_get('key', timeout=.1))
            if mode != 'legacy':
                request = await get(host.host_requests)
                response = plugins.HostResponse(request_id=request.request_id)
                if mode == 'wrong_result':
                    response.storage_set.SetInParent()
                if mode in ('both', 'denied'):
                    response.error.code = common.PERMISSION_DENIED
                if mode == 'both':
                    response.storage_get.value = b'not-a-success'
                if mode != 'timeout':
                    await host.outgoing.put(plugins.CoreToPlugin(host_response=response))
            with pytest.raises(PluginError) as failure:
                await pending
            assert failure.value.code == code
            if mode == 'legacy':
                assert host.host_requests.empty()
        finally:
            if pending:
                pending.cancel()
            stop.set()
            await asyncio.wait_for(task, 2)


@pytest.mark.parametrize('allowed', [True, False])
async def test_storage_real_cpp_core_and_restart(tmp_path, allowed):
    env = environment(tmp_path, 'graceful')
    for key in ('KIARABOT_PLUGIN_EXECUTABLE', 'KIARABOT_PLUGIN_MANIFEST', 'KIARABOT_PLUGIN_PYTHONPATH'):
        env.pop(key)
    policy = manifest()
    if allowed:
        policy.permissions.append('storage.namespace')
        env['KIARABOT_PLUGIN_PERMISSIONS'] += ',storage.namespace'
    for attempt in range(2 if allowed else 1):
        process = await launch(env)
        task = None
        stop = asyncio.Event()
        try:
            line = await asyncio.wait_for(process.stdout.readline(), 5)
            assert b'plugin_delivery_ready=true' in line
            endpoint = '127.0.0.1:' + re.search(rb'port=(\d+)', line)[1].decode()
            from kiarabot_sdk import Options
            runner = Runner(policy, Plugin(), Options(endpoint, 'p' * 32, retry_delay=.01, heartbeat_interval=.05))
            task = asyncio.create_task(runner.run(stop))
            await ready(runner)
            if not allowed:
                with pytest.raises(PluginError) as denied:
                    await runner.context.storage_set('key', b'value', 'idempotency-key-1')
                assert denied.value.code == common.PERMISSION_DENIED
                continue
            if attempt == 1:
                assert await runner.context.storage_get('key') == b'binary\0value'
            await runner.context.storage_set('key', b'binary\0value', 'idempotency-key-1')
            assert await runner.context.storage_get('key') == b'binary\0value'
            with pytest.raises(PluginError) as conflict:
                await runner.context.storage_set('key', b'different', 'idempotency-key-1')
            assert conflict.value.code == common.CONFLICT
            await runner.context.storage_delete('missing', 'idempotency-key-delete')
            with pytest.raises(PluginError) as missing:
                await runner.context.storage_get('missing')
            assert missing.value.code == common.NOT_FOUND
        finally:
            stop.set()
            if task:
                await asyncio.wait_for(task, 3)
            if process.returncode is None:
                process.send_signal(signal.SIGTERM)
                await asyncio.wait_for(process.communicate(), 5)
            await cleanup(process)
            assert process.returncode == 0


async def test_storage_expiration_is_forwarded_unchanged():
    from datetime import datetime, timedelta, timezone

    async with FakeHost() as host:
        host.features = ['host.storage.v1']
        policy = manifest()
        policy.permissions.append('storage.namespace')
        stop = asyncio.Event()
        runner = Runner(policy, Plugin(), options(host))
        task = asyncio.create_task(runner.run(stop))
        try:
            await ready(runner)
            expiration = datetime.now(timezone.utc) + timedelta(hours=1)
            pending = asyncio.create_task(runner.context.storage_set(
                'key', b'value', 'idempotency-key-1', expires_at=expiration))
            request = await get(host.host_requests)
            assert request.storage_set.key == 'key' and request.storage_set.value == b'value'
            sent = request.storage_set.expires_at.ToDatetime(tzinfo=timezone.utc)
            assert abs(sent - expiration) < timedelta(seconds=1)
            await host.outgoing.put(plugins.CoreToPlugin(host_response=plugins.HostResponse(
                request_id=request.request_id, storage_set=plugins.StorageSetResponse())))
            await pending
        finally:
            stop.set()
            await asyncio.wait_for(task, 2)
