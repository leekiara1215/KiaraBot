import asyncio
from pathlib import Path

import pytest
from kiarabot.v1 import common_pb2 as common, events_pb2 as events, platform_pb2 as platform
from kiarabot.v1 import plugins_pb2 as plugins
from kiarabot_sdk import Options, Plugin, PluginError, Runner, load_manifest
from kiarabot_sdk.runtime import validate_manifest
from kiarabot_sdk.testing import FakeTargetRuntime
from observer import create_plugin

MANIFEST = Path(__file__).resolve().parents[3] / 'plugins/event-observer/plugin.json'


def manifest():
    return load_manifest(MANIFEST)


async def get(queue):
    return await asyncio.wait_for(queue.get(), 3)


async def wait_disposition(runner, expected):
    async with asyncio.timeout(3):
        while not runner.last_dispositions or runner.last_dispositions[-1] != expected:
            await asyncio.sleep(.01)
    return runner.last_dispositions[-1]


def options(host, **kwargs):
    return Options(host.endpoint, host.token, tls_root_cert=host.root_cert,
                   retry_delay=.01, heartbeat_interval=.05, lease_renew_interval=.1,
                   **kwargs)


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
    secured = Options('127.0.0.1:1234', 'secret' * 8, tls_root_cert=b'dummy-pem')
    assert 'secret' not in repr(secured) and 'dummy-pem' not in repr(secured)


def test_transport_fails_closed_without_server_verified_tls():
    with pytest.raises(PluginError) as missing:
        Options('127.0.0.1:1234', 't' * 32)
    assert missing.value.code == common.INVALID_ARGUMENT
    with pytest.raises(PluginError):
        Options('127.0.0.1:1234', 't' * 32, tls_root_cert='/missing/ca.pem')
    with pytest.raises(PluginError):
        Options('not-an-endpoint', 't' * 32, tls_root_cert=b'pem')
    with pytest.raises(PluginError):
        Options('127.0.0.1:1234', 'short', tls_root_cert=b'pem')


async def test_revoked_bootstrap_fails_without_retry():
    plugin, stop = create_plugin(), asyncio.Event()
    async with FakeTargetRuntime() as host:
        runner = Runner(manifest(), plugin, Options(
            host.endpoint, 'x' * 32, tls_root_cert=host.root_cert,
            retry_delay=.01, heartbeat_interval=.05))
        with pytest.raises(PluginError) as failure:
            await asyncio.wait_for(runner.run(stop), 3)
        assert failure.value.code == common.PERMISSION_DENIED


async def test_registration_returns_fence_and_acks_carry_attempt():
    plugin, stop = create_plugin(), asyncio.Event()
    async with FakeTargetRuntime() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            registration = await get(host.registrations)
            assert registration.instance_id
            await host.send_event(events.EventEnvelope(event_id='first', trace_id='trace'))
            ack = await get(host.acknowledgements)
            assert ack.accepted and ack.event_id == 'first'
            # The target ACK carries the current delivery attempt.
            assert ack.HasField('attempt') and ack.attempt.event_id == 'first'
            assert host.attempts['first'].attempt_id == ack.attempt.attempt_id
            assert await wait_disposition(runner, platform.DELIVERY_ACKNOWLEDGED)
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)
    assert not runner.context.granted_permissions


async def test_delivery_renewed_then_stale_is_not_treated_as_success():
    release = asyncio.Event()

    class Slow(Plugin):
        async def on_event(self, event, context):
            await release.wait()

    plugin, stop = Slow(), asyncio.Event()
    async with FakeTargetRuntime() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_event(events.EventEnvelope(event_id='slow', trace_id='trace'))
            attempt, disposition = await get(host.renewals)
            assert disposition == platform.DELIVERY_RENEWED
            host.expire_attempt('slow')
            release.set()
            ack = await get(host.acknowledgements)
            assert ack.event_id == 'slow'
            assert await wait_disposition(runner, platform.DELIVERY_STALE)
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_rejected_hook_sends_error_ack_and_receipt():
    class Rejecting(Plugin):
        async def on_event(self, event, context):
            raise PluginError(common.INVALID_ARGUMENT, retryable=False)

    plugin, stop = Rejecting(), asyncio.Event()
    async with FakeTargetRuntime() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_event(events.EventEnvelope(event_id='bad', trace_id='trace'))
            ack = await get(host.acknowledgements)
            assert not ack.accepted and ack.error.code == common.INVALID_ARGUMENT
            assert await wait_disposition(runner, platform.DELIVERY_REJECTED)
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_single_flight_refresh_rotates_proof_and_keeps_stream():
    plugin, stop = create_plugin(), asyncio.Event()
    async with FakeTargetRuntime(authorization_lifetime=2.0) as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            first = await get(host.refreshes)
            assert first.refresh_generation == 1
            # After rotation the SDK must use the new proof; the fake validates
            # the connection proof on every post-registration frame.
            await asyncio.sleep(.3)
            await host.send_event(events.EventEnvelope(event_id='after-refresh'))
            ack = await get(host.acknowledgements)
            assert ack.accepted and ack.event_id == 'after-refresh'
            assert host.refresh_generation >= 2
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_refresh_failure_reestablishes_connection_with_bootstrap():
    plugin, stop = create_plugin(), asyncio.Event()
    async with FakeTargetRuntime(authorization_lifetime=1.0) as host:
        host.refresh_failure = 'abort'
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            first = await get(host.registrations)
            second = await get(host.registrations)
            assert second.instance_id == first.instance_id
            assert host.connection_generation >= 2
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_reconnect_rechecks_grants_before_events():
    plugin, stop = create_plugin(), asyncio.Event()
    async with FakeTargetRuntime() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        await get(host.registrations)
        await host.send_event(events.EventEnvelope(event_id='ok'))
        assert (await get(host.acknowledgements)).accepted
        host.grants = []
        await host.disconnect()
        with pytest.raises(PluginError) as failure:
            await asyncio.wait_for(running, 3)
        assert failure.value.code == common.PERMISSION_DENIED
        assert plugin.events == ['ok'] and not runner.context.granted_permissions


async def test_heartbeats_survive_a_silent_server():
    plugin, stop = create_plugin(), asyncio.Event()
    async with FakeTargetRuntime(heartbeat_acks=False) as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await get(host.heartbeats)
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_shutdown_notice_waits_for_inflight_event_before_on_stop():
    order = []

    class Ordered(Plugin):
        async def on_event(self, event, context):
            order.append('event-start')
            await asyncio.sleep(.1)
            order.append('event-end')

        async def on_stop(self, reason):
            order.append(('on_stop', reason))

    plugin, stop = Ordered(), asyncio.Event()
    async with FakeTargetRuntime() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        try:
            await get(host.registrations)
            await host.send_event(events.EventEnvelope(event_id='inflight', trace_id='trace'))
            await asyncio.sleep(.02)
            await host.send_shutdown(reason='upgrade', drain_timeout_ms=500)
            await asyncio.wait_for(running, 3)
            # The queued/in-flight handler finishes (and is ACKed) before on_stop runs; a
            # ShutdownNotice never drops accepted work or reconnects the same session.
            assert order == ['event-start', 'event-end', ('on_stop', 'shutdown')]
            assert host.registrations.empty() and host.connection_generation == 1
        finally:
            stop.set()
            await asyncio.wait_for(running, 2)


async def test_shutdown_notice_drains_and_stops_without_reconnect():
    class Draining(Plugin):
        stopped = False
        async def on_stop(self, reason):
            self.stopped = True

    plugin, stop = Draining(), asyncio.Event()
    async with FakeTargetRuntime() as host:
        runner = Runner(manifest(), plugin, options(host))
        running = asyncio.create_task(runner.run(stop))
        await get(host.registrations)
        await host.send_shutdown(reason='upgrade', drain_timeout_ms=500)
        await asyncio.wait_for(running, 3)
        assert plugin.stopped
        assert host.registrations.empty()
        assert host.connection_generation == 1
