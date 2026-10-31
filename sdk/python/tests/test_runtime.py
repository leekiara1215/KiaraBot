import asyncio
from pathlib import Path

import pytest
from kiarabot.v1 import common_pb2 as common, events_pb2 as events, plugins_pb2 as plugins
from kiarabot_sdk import Options, Plugin, PluginError, Runner, load_manifest
from kiarabot_sdk.runtime import validate_manifest
from kiarabot_sdk.testing import FakeHost
from observer import create_plugin

MANIFEST = Path(__file__).resolve().parents[3] / 'plugins/event-observer/plugin.json'


def manifest():
    return load_manifest(MANIFEST)


async def get(queue):
    return await asyncio.wait_for(queue.get(), 3)


def options(host, **kwargs):
    return Options(host.endpoint, host.token, retry_delay=.01, heartbeat_interval=.05, **kwargs)


@pytest.mark.parametrize('field,value,code', [
    ('id', '', common.INVALID_ARGUMENT), ('api_version', 'v2', common.INVALID_ARGUMENT),
    ('runtime', 'native', common.CAPABILITY_NOT_SUPPORTED),
    ('isolation', 'wasm', common.CAPABILITY_NOT_SUPPORTED),
    ('entrypoint', '../observer', common.INVALID_ARGUMENT),
    ('config_schema', '{"type":"object"}', common.CAPABILITY_NOT_SUPPORTED),
])
def test_manifest_validation(field, value, code):
    candidate = manifest()
    setattr(candidate, field, value)
    with pytest.raises(PluginError) as failure:
        validate_manifest(candidate)
    assert failure.value.code == code


def test_manifest_strict_json_and_permissions(tmp_path):
    path = tmp_path / 'manifest.json'
    path.write_text('{"unrecognized":true}')
    with pytest.raises(PluginError):
        load_manifest(path)
    candidate = manifest()
    candidate.permissions.append(candidate.permissions[0])
    with pytest.raises(PluginError):
        validate_manifest(candidate)
    assert 'secret' not in repr(Options('127.0.0.1:1234', 'secret' * 8))


async def test_example_receives_generated_event_and_reconnects():
    plugin = create_plugin()
    stop = asyncio.Event()
    async with FakeHost() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            first = await get(host.registrations)
            await host.outgoing.put(plugins.CoreToPlugin(event=events.EventEnvelope(event_id='first', trace_id='trace')))
            ack = await get(host.acknowledgements)
            assert ack.accepted and ack.event_id == 'first' and not ack.HasField('error')
            assert plugin.events == ['first']
            assert runner.context.granted_permissions == frozenset(['message.read'])
            await host.outgoing.put(None)
            second = await get(host.registrations)
            assert first.instance_id != second.instance_id
            await host.outgoing.put(plugins.CoreToPlugin(event=events.EventEnvelope(event_id='second')))
            assert (await get(host.acknowledgements)).accepted
            assert plugin.events == ['first', 'second']
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)
        assert not runner.context.granted_permissions


@pytest.mark.parametrize('mode,code', [('exception', common.INTERNAL), ('timeout', common.TIMEOUT)])
async def test_hook_failure_isolated_and_next_event_delivered(mode, code, caplog):
    class Faulty(Plugin):
        stopped = False
        async def on_event(self, event, context):
            if event.event_id == 'bad':
                if mode == 'exception':
                    raise ValueError('private credential never logged')
                await asyncio.sleep(60)
        async def on_stop(self, reason):
            self.stopped = True
    plugin, stop = Faulty(), asyncio.Event()
    async with FakeHost() as host:
        running = asyncio.create_task(Runner(manifest(), plugin, options(host, hook_timeout=.03)).run(stop))
        try:
            await get(host.registrations)
            await host.outgoing.put(plugins.CoreToPlugin(event=events.EventEnvelope(event_id='bad', trace_id='trace')))
            ack = await get(host.acknowledgements)
            assert not ack.accepted and ack.error.code == code and ack.error.trace_id == 'trace'
            assert 'private' not in str(ack) + caplog.text
            await host.outgoing.put(plugins.CoreToPlugin(event=events.EventEnvelope(event_id='good')))
            assert (await get(host.acknowledgements)).accepted
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)
        assert plugin.stopped


async def test_reconnect_rechecks_grants_before_events():
    plugin, stop = create_plugin(), asyncio.Event()
    async with FakeHost() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        await get(host.registrations)
        await host.outgoing.put(plugins.CoreToPlugin(event=events.EventEnvelope(event_id='ok')))
        assert (await get(host.acknowledgements)).accepted
        host.grants = []
        await host.outgoing.put(None)
        with pytest.raises(PluginError) as failure:
            await asyncio.wait_for(running, 2)
        assert failure.value.code == common.PERMISSION_DENIED
        assert plugin.events == ['ok'] and not runner.context.granted_permissions


async def test_rejected_registration_and_bad_credentials_do_not_call_event():
    async with FakeHost() as host:
        host.rejection = common.Error(code=common.PERMISSION_DENIED)
        for token in (host.token, 'w' * 32):
            plugin = create_plugin()
            with pytest.raises(PluginError) as failure:
                await asyncio.wait_for(Runner(manifest(), plugin, Options(host.endpoint, token)).run(asyncio.Event()), 2)
            assert failure.value.code == common.PERMISSION_DENIED
            assert not plugin.events


async def test_start_precedes_registration_and_stop_runs():
    class Ordered(Plugin):
        seen = []
        async def on_start(self, context):
            self.seen.append('start')
        async def on_stop(self, reason):
            self.seen.append('stop')
    plugin, stop = Ordered(), asyncio.Event()
    async with FakeHost() as host:
        running = asyncio.create_task(Runner(manifest(), plugin, options(host)).run(stop))
        await get(host.registrations)
        assert plugin.seen == ['start']
        stop.set()
        await asyncio.wait_for(running, 2)
        assert plugin.seen == ['start', 'stop']


def test_manifest_capabilities_match_host_validation():
    unknown = manifest()
    unknown.capabilities.append('event.not.real')
    with pytest.raises(PluginError) as failure:
        validate_manifest(unknown)
    assert failure.value.code == common.CAPABILITY_NOT_SUPPORTED
    missing = manifest()
    missing.capabilities.append('event.timer.triggered')
    with pytest.raises(PluginError) as failure:
        validate_manifest(missing)
    assert failure.value.code == common.PERMISSION_DENIED
    invalid = manifest()
    invalid.permissions.append('not a permission')
    with pytest.raises(PluginError) as failure:
        validate_manifest(invalid)
    assert failure.value.code == common.INVALID_ARGUMENT


async def test_ordered_manifest_is_forwarded_and_heartbeat_timeout_reconnects():
    plugin, stop = create_plugin(), asyncio.Event()
    async with FakeHost(heartbeat_acks=False) as host:
        runner = Runner(manifest(), plugin,
                        options(host, io_timeout=.05))
        running = asyncio.create_task(runner.run(stop))
        try:
            first = await get(host.registrations)
            assert first.manifest.ordered is True
            second = await get(host.registrations)
            assert second.instance_id != first.instance_id
            assert not (await get(host.heartbeats)).instance_id == ''
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_full_event_queue_rate_limits_and_reconnects():
    class Blocking(Plugin):
        def __init__(self):
            self.release = asyncio.Event()

        async def on_event(self, event, context):
            await self.release.wait()

    plugin, stop = Blocking(), asyncio.Event()
    async with FakeHost() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            # The first event is consumed by the serial hook, the next two fill the
            # queue and the fourth overflows it with RATE_LIMITED, forcing a reconnect.
            for index in range(4):
                await host.outgoing.put(plugins.CoreToPlugin(
                    event=events.EventEnvelope(event_id=f'event-{index}')))
            second = await get(host.registrations)
            assert second.instance_id
        finally:
            plugin.release.set()
            stop.set()
            await asyncio.wait_for(running, 2)
