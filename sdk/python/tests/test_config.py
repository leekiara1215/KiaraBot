import asyncio

import pytest
from kiarabot.v1 import common_pb2 as common, events_pb2 as events, plugins_pb2 as plugins
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk.testing import FakeTargetRuntime
from test_runtime import manifest, get, options


class Configurable(Plugin):
    def __init__(self):
        self.calls = []

    async def on_config(self, config, context):
        self.calls.append((config.version, config.config_version))
        if config.version == 'bad':
            raise PluginError(common.INVALID_ARGUMENT)
        if config.version == 'exception':
            raise RuntimeError('private exception data')
        if config.version == 'timeout':
            await asyncio.Event().wait()
        # Changing the callback input must not mutate the SDK's saved snapshot.
        config.values['mutated'] = True


def policy():
    result = manifest()
    return result


async def test_config_ack_binds_fence_and_config_version():
    plugin, stop = Configurable(), asyncio.Event()
    async with FakeTargetRuntime(features=['plugin.config.v1']) as host:
        runner = Runner(policy(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_config('good', {'number': 1}, config_version=1)
            ack = await get(host.config_acknowledgements)
            assert ack.accepted and ack.version == 'good'
            assert ack.config_version == 1 and ack.HasField('fence')
            assert ack.fence == host.fence
            assert plugin.calls == [('good', 1)]
            assert runner.context.config.version == 'good'
            assert 'mutated' not in runner.context.config.values
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_stale_config_version_never_replaces_applied_evidence():
    plugin, stop = Configurable(), asyncio.Event()
    async with FakeTargetRuntime(features=['plugin.config.v1']) as host:
        runner = Runner(policy(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_config('v2', {'n': 2}, config_version=2)
            assert (await get(host.config_acknowledgements)).accepted
            await host.send_config('v1', {'n': 1}, config_version=1,
                                   delivery_id='stale-delivery')
            ack = await get(host.config_acknowledgements)
            assert ack.accepted and ack.config_version == 1
            # The lower version is acknowledged idempotently but never applied.
            assert plugin.calls == [('v2', 2)]
            assert runner.context.config.config_version == 2
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


@pytest.mark.parametrize('version,mode,code', [
    ('bad', 'reject', common.INVALID_ARGUMENT),
    ('exception', 'exception', common.INTERNAL),
    ('timeout', 'timeout', common.TIMEOUT),
])
async def test_config_failure_is_reported_and_recoverable(version, mode, code):
    plugin, stop = Configurable(), asyncio.Event()
    async with FakeTargetRuntime(features=['plugin.config.v1']) as host:
        runner = Runner(policy(), plugin, options(host, hook_timeout=.03))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_config(version, config_version=1,
                                   delivery_id='failure-' + version)
            failed = await get(host.config_acknowledgements)
            assert not failed.accepted and failed.error.code == code
            assert failed.error.message == common.ErrorCode.Name(code)
            assert runner.context.config is None
            await host.send_config('recovered', config_version=2)
            assert (await get(host.config_acknowledgements)).accepted
            assert runner.context.config.version == 'recovered'
            assert plugin.calls[-1] == ('recovered', 2)
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_duplicate_delivery_replays_the_original_ack_without_recalling_hook():
    plugin, stop = Configurable(), asyncio.Event()
    async with FakeTargetRuntime(features=['plugin.config.v1']) as host:
        runner = Runner(policy(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_config('once', config_version=1, delivery_id='same-delivery')
            assert (await get(host.config_acknowledgements)).accepted
            await host.send_config('once', config_version=1, delivery_id='same-delivery')
            assert (await get(host.config_acknowledgements)).accepted
            assert plugin.calls == [('once', 1)]
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_config_and_event_hooks_are_serial_with_independent_rpc_reader():
    order = []

    class Ordered(Plugin):
        async def on_config(self, config, context):
            order.append('config-start')
            await context.storage_get('key')
            order.append('config-end')

        async def on_event(self, event, context):
            order.append('event-' + context.config.version)

    stop = asyncio.Event()
    async with FakeTargetRuntime(features=['plugin.config.v1', 'host.storage.v1']) as host:
        candidate = manifest()
        candidate.permissions.append('storage.namespace')
        runner = Runner(candidate, Ordered(), options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_config('1', config_version=1)
            request = await get(host.host_requests)
            await host.send_event(events.EventEnvelope(event_id='event'))
            assert order == ['config-start']
            await host.send_frame(plugins.CoreToPlugin(host_response=plugins.HostResponse(
                request_id=request.request_id,
                storage_get=plugins.StorageGetResponse(value=b'value'))))
            assert (await get(host.config_acknowledgements)).accepted
            assert (await get(host.acknowledgements)).accepted
            assert order == ['config-start', 'config-end', 'event-1']
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


@pytest.mark.parametrize('enabled', [False, True])
async def test_config_requires_negotiation_and_valid_deadline(enabled):
    stop = asyncio.Event()
    async with FakeTargetRuntime(features=['plugin.config.v1'] if enabled else []) as host:
        runner = Runner(manifest(), Plugin(), options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_config('version', config_version=1, apply_timeout_ms=0)
            with pytest.raises(PluginError) as failure:
                await asyncio.wait_for(running, 2)
            expected = common.INVALID_ARGUMENT if enabled else common.CAPABILITY_NOT_SUPPORTED
            assert failure.value.code == expected
        finally:
            stop.set()
            if not running.done():
                await asyncio.wait_for(running, 2)
