"""Managed subprocess chain: real Python plugin process -> fake target PluginBridge."""
import asyncio
import json
import os
from pathlib import Path
import signal
import sys

import pytest
from kiarabot.v1 import events_pb2 as events
from kiarabot_sdk.testing import FakeTargetRuntime

ROOT = Path(__file__).resolve().parents[3]


async def wait_file(path, timeout=5):
    async with asyncio.timeout(timeout):
        while not path.exists():
            await asyncio.sleep(.01)
    return path.read_text()


def write_subject(tmp_path, mode):
    marker, stopped, seen = tmp_path / 'marker.json', tmp_path / 'stopped', tmp_path / 'seen'
    (tmp_path / 'subject.py').write_text(f'''
import asyncio, json, os
from pathlib import Path
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
