import asyncio

import pytest
from kiarabot.v1 import common_pb2 as common, events_pb2 as events, plugins_pb2 as plugins
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk.testing import FakeHost
from test_runtime import manifest, get, options


def frame(version, **values):
    result = plugins.PluginConfig(version=version, delivery_id='delivery-' + version, apply_timeout_ms=5000)
    result.values.update(values)
    return plugins.CoreToPlugin(config=result)


@pytest.mark.parametrize('mode,code', [('reject', common.INVALID_ARGUMENT), ('exception', common.INTERNAL), ('timeout', common.TIMEOUT)])
async def test_config_ack_snapshot_isolation_failure_and_recovery(mode, code):
    calls = []
    class Configurable(Plugin):
        async def on_config(self, config, context):
            calls.append(config.version)
            if config.version == 'bad':
                if mode == 'reject':
                    raise PluginError(common.INVALID_ARGUMENT)
                if mode == 'exception':
                    raise RuntimeError('private exception data')
                await asyncio.Event().wait()
            # Changing the callback input must not mutate the SDK's saved snapshot.
            config.values['mutated'] = True
    stop = asyncio.Event()
    async with FakeHost() as host:
        host.features = ['plugin.config.v1']
        runner = Runner(manifest(), Configurable(), options(host, hook_timeout=.03))
        running = asyncio.create_task(runner.run(stop))
        try:
            assert 'plugin.config.v1' in (await get(host.registrations)).supported_features
            initial = frame('good', number=1)
            await host.outgoing.put(initial)
            assert (await get(host.config_acknowledgements)).accepted
            assert 'mutated' not in runner.context.config.values
            runner.context.config.values['number'] = 100
            assert runner.context.config.values['number'] == 1
            await host.outgoing.put(initial)
            assert (await get(host.config_acknowledgements)).accepted
            assert calls == ['good']
            await host.outgoing.put(frame('bad'))
            failed = await get(host.config_acknowledgements)
            assert not failed.accepted and failed.error.code == code
            assert failed.error.message == common.ErrorCode.Name(code)
            assert runner.context.config.version == 'good'
            await host.outgoing.put(frame('recovered'))
            assert (await get(host.config_acknowledgements)).accepted
            assert runner.context.config.version == 'recovered'
            await host.outgoing.put(None)
            await get(host.registrations)
            assert runner.context.config is None
            await host.outgoing.put(frame('recovered'))
            assert (await get(host.config_acknowledgements)).accepted
            assert calls.count('recovered') == 2
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)
        assert runner.context.config is None


async def test_config_and_event_hooks_are_serial_with_independent_rpc_reader():
    order = []
    class Configurable(Plugin):
        async def on_config(self, config, context):
            order.append('config-start')
            await context.storage_get('key')
            order.append('config-end')
        async def on_event(self, event, context):
            order.append('event-' + context.config.version)
    stop = asyncio.Event()
    async with FakeHost() as host:
        host.features = ['plugin.config.v1', 'host.storage.v1']
        policy = manifest(); policy.permissions.append('storage.namespace')
        runner = Runner(policy, Configurable(), options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.outgoing.put(frame('1'))
            request = await get(host.host_requests)
            await host.outgoing.put(plugins.CoreToPlugin(event=events.EventEnvelope(event_id='event')))
            assert order == ['config-start']
            await host.outgoing.put(plugins.CoreToPlugin(host_response=plugins.HostResponse(
                request_id=request.request_id, storage_get=plugins.StorageGetResponse(value=b'value'))))
            assert (await get(host.config_acknowledgements)).accepted
            assert (await get(host.acknowledgements)).accepted
            assert order == ['config-start', 'config-end', 'event-1']
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


@pytest.mark.parametrize('enabled,timeout,code', [(False, 5000, common.CAPABILITY_NOT_SUPPORTED),
                                                (True, 0, common.INVALID_ARGUMENT)])
async def test_config_requires_negotiation_and_valid_deadline(enabled, timeout, code):
    stop = asyncio.Event()
    async with FakeHost() as host:
        host.features = ['plugin.config.v1'] if enabled else []
        runner = Runner(manifest(), Plugin(), options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            invalid = frame('version')
            invalid.config.apply_timeout_ms = timeout
            await host.outgoing.put(invalid)
            with pytest.raises(PluginError) as failure:
                await asyncio.wait_for(running, 2)
            assert failure.value.code == code
            assert host.config_acknowledgements.empty()
            assert runner.context.config is None
        finally:
            stop.set()
            if not running.done():
                await asyncio.wait_for(running, 2)
