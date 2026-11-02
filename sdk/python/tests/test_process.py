"""Managed subprocess chain: real Python plugin process -> fake target PluginBridge."""
import asyncio
import json
import os
from pathlib import Path
import signal
import sys

import pytest
from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common, events_pb2 as events
from kiarabot.v1 import platform_pb2 as platform
from kiarabot_sdk.testing import FakeTargetRuntime

ROOT = Path(__file__).resolve().parents[3]


async def wait_file(path, timeout=5):
    async with asyncio.timeout(timeout):
        while not path.exists():
            await asyncio.sleep(.01)
    return path.read_text()


def write_subject(tmp_path, mode):
    marker, stopped, seen = tmp_path / 'marker.json', tmp_path / 'stopped', tmp_path / 'seen'
    result = tmp_path / 'action-result.json'
    (tmp_path / 'subject.py').write_text(f'''
import asyncio, json, os
from datetime import datetime, timedelta, timezone
from pathlib import Path
from kiarabot.v1 import actions_pb2 as actions
from kiarabot_sdk import Plugin
class Subject(Plugin):
    async def on_start(self, context):
        Path({str(marker)!r}).write_text(json.dumps(dict(pid=os.getpid(), environment=dict(os.environ))))
        if {mode!r} == 'crash':
            async def crash():
                await asyncio.sleep(.3)
                os._exit(12)
            self.crasher = asyncio.create_task(crash())
    async def on_event(self, event, context):
        Path({str(seen)!r}).write_text(event.event_id)
        if {mode!r} == 'action':
            action = actions.ActionEnvelope(action_id='managed-action',
                idempotency_key='managed-key', trace_id=event.trace_id, requested_by='subject')
            action.deadline.FromDatetime(datetime.now(timezone.utc) + timedelta(seconds=30))
            action.send_message.message.plain_text = 'managed action'
            result = await context.submit_action(action)
            Path({str(result)!r}).write_text(json.dumps(dict(
                action_id=result.action_id, status=result.status,
                outcome=result.execution_outcome, attempt=result.dispatch_attempt_id)))
    async def on_stop(self, reason):
        Path({str(stopped)!r}).write_text(reason)
def create():
    return Subject()
''')
    manifest = json.loads((ROOT / 'plugins/event-observer/plugin.json').read_text())
    manifest['entrypoint'] = 'subject:create'
    manifest_path = tmp_path / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    return manifest_path


def environment(tmp_path, host, manifest_path):
    ca = tmp_path / 'ca.pem'
    ca.write_bytes(host.root_cert)
    # A Host supplies a minimal allow-listed environment; runtime/adaptor
    # secrets are never inherited by the managed plugin process.
    base = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
            'HOME': str(tmp_path),
            'LANG': os.environ.get('LANG', 'C.UTF-8'),
            'PYTHONPATH': os.pathsep.join(map(str, [ROOT / 'sdk/python/src',
                ROOT / 'sdk/python/build/generated', tmp_path]))}
    base.update(PLUGIN_CORE_ENDPOINT=host.endpoint, PLUGIN_AUTH_TOKEN=host.token,
                PLUGIN_TLS_ROOT=str(ca), PLUGIN_INSTANCE_ID='managed-instance-1',
                PLUGIN_IO_TIMEOUT='3', PLUGIN_HEARTBEAT_INTERVAL='0.2',
                PLUGIN_RETRY_DELAY='0.1')
    return base


async def launch(env):
    return await asyncio.create_subprocess_exec(
        sys.executable, '-m', 'kiarabot_sdk', '--manifest', env.pop('PLUGIN_MANIFEST'),
        env=env, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)


async def cleanup(process):
    if process.returncode is None:
        process.terminate()
        try:
            await asyncio.wait_for(process.wait(), 3)
        except asyncio.TimeoutError:
            process.kill()
    await process.communicate()


@pytest.mark.parametrize('mode', ['graceful', 'crash'])
async def test_managed_plugin_process_against_fake_target(tmp_path, mode):
    manifest_path = write_subject(tmp_path, mode)
    async with FakeTargetRuntime() as host:
        env = environment(tmp_path, host, manifest_path)
        env['PLUGIN_MANIFEST'] = str(manifest_path)
        process = await launch(env)
        try:
            marker = json.loads(await wait_file(tmp_path / 'marker.json'))
            child_environment = marker['environment']
            # Only the bootstrap material explicitly provided by deployment is
            # visible to the managed plugin; runtime secrets never leak.
            assert 'PRIVATE_SECRET' not in child_environment
            assert 'KIARABOT_ADAPTER_TOKEN' not in child_environment
            registration = await asyncio.wait_for(host.registrations.get(), 5)
            assert registration.instance_id == 'managed-instance-1'
            await host.send_event(events.EventEnvelope(event_id='child-event', trace_id='trace'))
            assert await wait_file(tmp_path / 'seen') == 'child-event'
            ack = await asyncio.wait_for(host.acknowledgements.get(), 3)
            assert ack.accepted and ack.HasField('attempt')
            if mode == 'crash':
                await asyncio.wait_for(process.wait(), 5)
                assert process.returncode == 12
            else:
                process.send_signal(signal.SIGTERM)
                assert await asyncio.wait_for(process.wait(), 5) == 0
                assert (tmp_path / 'stopped').read_text() == 'shutdown'
            output, error = await process.communicate()
            assert b'must-not-reach-child' not in output + error
        finally:
            await cleanup(process)


async def test_missing_tls_configuration_fails_closed(tmp_path):
    manifest_path = write_subject(tmp_path, 'graceful')
    async with FakeTargetRuntime() as host:
        env = environment(tmp_path, host, manifest_path)
        env.pop('PLUGIN_TLS_ROOT')
        env['PLUGIN_MANIFEST'] = str(manifest_path)
        process = await launch(env)
        try:
            await asyncio.wait_for(process.communicate(), 5)
            assert process.returncode == 1
        finally:
            await cleanup(process)


async def test_invalid_managed_configuration_fails_closed(tmp_path):
    manifest_path = write_subject(tmp_path, 'graceful')
    async with FakeTargetRuntime() as host:
        env = environment(tmp_path, host, manifest_path)
        env['PLUGIN_AUTH_TOKEN'] = 'short'
        env['PLUGIN_MANIFEST'] = str(manifest_path)
        process = await launch(env)
        try:
            await asyncio.wait_for(process.communicate(), 5)
            assert process.returncode == 1
        finally:
            await cleanup(process)


async def test_managed_plugin_action_roundtrip(tmp_path):
    manifest_path = write_subject(tmp_path, 'action')
    accepted = actions.ActionResult(status=actions.ACCEPTED,
                                   execution_outcome=platform.EXECUTION_NOT_STARTED)

    async with FakeTargetRuntime() as host:
        host.features = ['action.result.v1']
        def policy(kind, request, sequence):
            if kind != 'action':
                return None
            result = actions.ActionResult()
            result.CopyFrom(accepted)
            result.action_id = request.action.action.action_id
            return {'result': result}
        host.action_policy = policy
        env = environment(tmp_path, host, manifest_path)
        env['PLUGIN_MANIFEST'] = str(manifest_path)
        process = await launch(env)
        try:
            await asyncio.wait_for(host.registrations.get(), 5)
            await host.send_event(events.EventEnvelope(event_id='action-event',
                                                       trace_id='trace-action'))
            action_request = (await asyncio.wait_for(host.actions.get(), 5)).action
            assert action_request.action.action_id == 'managed-action'
            assert action_request.action.idempotency_key == 'managed-key'
            assert action_request.action.WhichOneof('payload') == 'send_message'
            assert action_request.action.send_message.message.plain_text == 'managed action'
            assert action_request.action.HasField('deadline')
            async with asyncio.timeout(5):
                while not (tmp_path / 'action-result.json').exists():
                    await asyncio.sleep(.01)
            result = json.loads((tmp_path / 'action-result.json').read_text())
            assert result == {'action_id': 'managed-action', 'status': actions.ACCEPTED,
                              'outcome': platform.EXECUTION_NOT_STARTED, 'attempt': ''}
            await asyncio.wait_for(host.acknowledgements.get(), 3)
            process.send_signal(signal.SIGTERM)
            assert await asyncio.wait_for(process.wait(), 5) == 0
        finally:
            await cleanup(process)


async def test_managed_plugin_uses_host_injected_bootstrap_credential(tmp_path):
    """P08c: a Host-spawned process authenticates with KIARABOT_PLUGIN_BOOTSTRAP only."""
    manifest_path = write_subject(tmp_path, 'graceful')
    async with FakeTargetRuntime() as host:
        env = environment(tmp_path, host, manifest_path)
        env.pop('PLUGIN_AUTH_TOKEN')
        env['KIARABOT_PLUGIN_BOOTSTRAP'] = host.token
        env['PLUGIN_MANIFEST'] = str(manifest_path)
        process = await launch(env)
        try:
            marker = json.loads(await wait_file(tmp_path / 'marker.json'))
            assert marker['environment']['KIARABOT_PLUGIN_BOOTSTRAP'] == host.token
            assert 'PLUGIN_AUTH_TOKEN' not in marker['environment']
            registration = await asyncio.wait_for(host.registrations.get(), 5)
            assert registration.instance_id == 'managed-instance-1'
            await host.send_event(events.EventEnvelope(event_id='bootstrap-event',
                                                       trace_id='trace'))
            assert await wait_file(tmp_path / 'seen') == 'bootstrap-event'
            await asyncio.wait_for(host.acknowledgements.get(), 3)
            process.send_signal(signal.SIGTERM)
            assert await asyncio.wait_for(process.wait(), 5) == 0
        finally:
            await cleanup(process)


def write_config_subject(tmp_path):
    """Managed plugin that records every on_config application, like a Host-spawned package."""
    applied = tmp_path / 'config-applied.jsonl'
    (tmp_path / 'config_subject.py').write_text(f'''
import json
from pathlib import Path
from kiarabot_sdk import Plugin
class ConfigSubject(Plugin):
    async def on_config(self, config, context):
        Path({str(applied)!r}).open('a').write(json.dumps(dict(
            version=config.version, config_version=config.config_version,
            delivery_id=config.delivery_id, values=dict(config.values))) + '\\n')
def create():
    return ConfigSubject()
''')
    manifest = json.loads((ROOT / 'plugins/event-observer/plugin.json').read_text())
    manifest['entrypoint'] = 'config_subject:create'
    manifest_path = tmp_path / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    return manifest_path, applied


async def wait_applied(path, count, timeout=5):
    async with asyncio.timeout(timeout):
        while not path.exists() or len(path.read_text().strip().splitlines()) < count:
            await asyncio.sleep(.01)
    return [json.loads(line) for line in path.read_text().strip().splitlines()]


async def test_managed_process_config_apply_ack_and_restart_replay(tmp_path):
    """P08c managed chain: Host-style env -> register -> desired config -> ACK -> restart replay."""
    manifest_path, applied = write_config_subject(tmp_path)
    async with FakeTargetRuntime(features=['plugin.config.v1'], desired_config_version=1) as host:
        async def launch_plugin():
            env = environment(tmp_path, host, manifest_path)
            env.pop('PLUGIN_AUTH_TOKEN')
            env['KIARABOT_PLUGIN_BOOTSTRAP'] = host.token
            env['PLUGIN_MANIFEST'] = str(manifest_path)
            return await launch(env)

        process = await launch_plugin()
        try:
            await asyncio.wait_for(host.registrations.get(), 5)
            await host.send_config('v1', {'greeting': 'hello'}, config_version=1,
                                   delivery_id='first-delivery')
            ack = await asyncio.wait_for(host.config_acknowledgements.get(), 5)
            assert ack.accepted and ack.config_version == 1
            assert ack.delivery_id == 'first-delivery' and ack.HasField('fence')
            assert (await wait_applied(applied, 1))[0]['values'] == {'greeting': 'hello'}
            process.send_signal(signal.SIGTERM)
            assert await asyncio.wait_for(process.wait(), 5) == 0
        finally:
            await cleanup(process)

        # Restart: the durable desired version is re-delivered with a new delivery id and the
        # (idempotent) callback applies it again before a fresh ACK.
        process = await launch_plugin()
        try:
            await asyncio.wait_for(host.registrations.get(), 5)
            await host.send_config('v1', {'greeting': 'hello'}, config_version=1,
                                   delivery_id='replay-delivery')
            replay = await asyncio.wait_for(host.config_acknowledgements.get(), 5)
            assert replay.accepted and replay.delivery_id == 'replay-delivery'
            applications = await wait_applied(applied, 2)
            assert [item['delivery_id'] for item in applications] == [
                'first-delivery', 'replay-delivery']
            process.send_signal(signal.SIGTERM)
            assert await asyncio.wait_for(process.wait(), 5) == 0
        finally:
            await cleanup(process)
