import asyncio
from datetime import datetime, timedelta, timezone

import pytest
from kiarabot.v1 import common_pb2 as common, plugins_pb2 as plugins
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk.testing import FakeTargetRuntime
from test_runtime import manifest, options, get
from test_actions import ready


@pytest.mark.parametrize('mode,code', [('legacy', common.CAPABILITY_NOT_SUPPORTED), ('timeout', common.TIMEOUT),
    ('wrong_result', common.UPSTREAM_UNAVAILABLE), ('both', common.UPSTREAM_UNAVAILABLE), ('denied', common.PERMISSION_DENIED)])
async def test_host_rejection_and_malformed_response(mode, code):
    async with FakeTargetRuntime(features=[] if mode == 'legacy' else ['host.storage.v1']) as host:
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
                    await host.send_frame(plugins.CoreToPlugin(host_response=response))
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


async def test_storage_expiration_is_forwarded_unchanged():
    async with FakeTargetRuntime(features=['host.storage.v1']) as host:
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
            await host.send_frame(plugins.CoreToPlugin(host_response=plugins.HostResponse(
                request_id=request.request_id, storage_set=plugins.StorageSetResponse())))
            await pending
        finally:
            stop.set()
            await asyncio.wait_for(task, 2)
