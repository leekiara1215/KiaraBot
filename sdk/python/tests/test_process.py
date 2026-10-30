"""Actual C++ daemon -> managed Python SDK -> generated gRPC, isolated local ports."""
import asyncio
import json
import os
from pathlib import Path
import re
import signal
import sys

import grpc
import pytest
from kiarabot.v1 import adapters_pb2 as adapters, common_pb2 as common, events_pb2 as events
from kiarabot.v1 import services_pb2_grpc as services

ROOT = Path(__file__).resolve().parents[3]
BINARY = Path(os.environ.get('CORE_TEST_BINARY', ROOT / 'build/proto/core/kiarabot-core'))


async def wait_file(path):
    async with asyncio.timeout(5):
        while not path.exists():
            await asyncio.sleep(.01)
    return path.read_text()


def environment(tmp_path, mode):
    marker = tmp_path / 'marker.json'
    stopped = tmp_path / 'stopped'
    seen = tmp_path / 'seen'
    (tmp_path / 'subject.py').write_text(f'''
import asyncio, json, os, time
from pathlib import Path
from kiarabot_sdk import Plugin
class Subject(Plugin):
    async def on_start(self, context):
        Path({str(marker)!r}).write_text(json.dumps(dict(pid=os.getpid(), environment=dict(os.environ))))
        if {mode!r} == 'start_timeout':
            time.sleep(60)
        if {mode!r} == 'crash':
            async def crash():
                await asyncio.sleep(.5)
                os._exit(12)
            self.crasher = asyncio.create_task(crash())
    async def on_event(self, event, context):
        Path({str(seen)!r}).write_text(event.event_id)
    async def on_stop(self, reason):
        if {mode!r} == 'stop_timeout':
            time.sleep(60)
        Path({str(stopped)!r}).write_text(reason)
def create():
    return Subject()
''')
    manifest = json.loads((ROOT / 'plugins/event-observer/plugin.json').read_text())
    manifest['entrypoint'] = 'subject:create'
    manifest_path = tmp_path / 'plugin.json'
    manifest_path.write_text(json.dumps(manifest))
    base = {key: value for key, value in os.environ.items() if not key.startswith('KIARABOT_')}
    base.update(KIARABOT_ADAPTER_ID='adapter', KIARABOT_ADAPTER_TOKEN='a' * 32,
                KIARABOT_PLUGIN_ID='event-observer', KIARABOT_PLUGIN_TOKEN='p' * 32,
                KIARABOT_PLUGIN_PERMISSIONS='message.read',
                KIARABOT_ADAPTER_ACCOUNTS='account', KIARABOT_RUNTIME_DB=str(tmp_path / 'runtime.db'),
                KIARABOT_PLUGIN_EXECUTABLE=sys.executable, KIARABOT_PLUGIN_MANIFEST=str(manifest_path),
                KIARABOT_PLUGIN_PYTHONPATH=os.pathsep.join(map(str, [ROOT / 'sdk/python/src',
                    ROOT / 'sdk/python/build/generated', tmp_path])),
                KIARABOT_PLUGIN_START_TIMEOUT_MS='500' if mode == 'start_timeout' else '5000',
                KIARABOT_PLUGIN_STOP_TIMEOUT_MS='100', PRIVATE_SECRET='must-not-reach-child')
    return base


async def launch(env):
    assert BINARY.exists(), 'Build the proto + infrastructure Core before SDK acceptance'
    return await asyncio.create_subprocess_exec(str(BINARY), '--grpc-listen', '127.0.0.1:0',
        env=env, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)


async def cleanup(process):
    if process.returncode is None:
        process.terminate()
        try:
            await asyncio.wait_for(process.wait(), 3)
        except asyncio.TimeoutError:
            process.kill()
    await process.communicate()


@pytest.mark.parametrize('mode', ['graceful', 'stop_timeout', 'crash', 'start_timeout'])
async def test_managed_lifecycle_environment_reclaim_and_event(tmp_path, mode):
    process = await launch(environment(tmp_path, mode))
    try:
        marker = json.loads(await wait_file(tmp_path / 'marker.json'))
        child_environment = marker['environment']
        assert 'PRIVATE_SECRET' not in child_environment
        assert 'KIARABOT_ADAPTER_TOKEN' not in child_environment
        assert child_environment['PLUGIN_AUTH_TOKEN'] == 'p' * 32
        if mode == 'start_timeout':
            output, error = await asyncio.wait_for(process.communicate(), 5)
            assert process.returncode == 1 and b'error_code=TIMEOUT' in error
        else:
            async with asyncio.timeout(7):
                lines = []
                while True:
                    line = await process.stdout.readline()
                    assert line, await process.stderr.read()
                    lines.append(line)
                    if b'operation=process_start' in line:
                        break
            port = int(re.search(rb'port=(\d+)', b''.join(lines))[1])
            assert b'operation=plugin_start outcome=registered' in b''.join(lines)
            if mode == 'graceful':
                async with grpc.aio.insecure_channel(f'127.0.0.1:{port}') as channel:
                    call = services.AdapterBridgeStub(channel).Connect(metadata=(('authorization', 'Bearer ' + 'a' * 32),))
                    try:
                        await call.write(adapters.AdapterToCore(register=adapters.RegisterAdapterRequest(
                            adapter_id='adapter', protocol_version='kiarabot.v1', implementation='fake',
                            implementation_version='1.0', accounts=[adapters.Account(account_id='account', enabled=True)])))
                        registered = (await asyncio.wait_for(call.read(), 2)).register_result
                        assert registered.accepted
                        event = events.EventEnvelope(event_id='sdk-process-event', trace_id='trace',
                            schema_version='kiarabot.v1', delivery_mode=common.AT_LEAST_ONCE, sequence=1)
                        event.occurred_at.GetCurrentTime()
                        event.source.adapter_id = 'adapter'
                        event.source.account_id = 'account'
                        event.source.adapter_instance_id = registered.assigned_instance_id
                        event.subject.conversation_id = 'conversation'
                        event.subject.user_id = 'user'
                        event.message_received.message_id = 'message'
                        event.message_received.sender_id = 'user'
                        event.message_received.conversation.id = 'conversation'
                        event.message_received.conversation.account_id = 'account'
                        event.message_received.conversation.type = common.ConversationRef.PRIVATE
                        await call.write(adapters.AdapterToCore(event=event))
                        assert (await asyncio.wait_for(call.read(), 2)).event_ack.status == adapters.AdapterEventAck.PERSISTED
                        assert await wait_file(tmp_path / 'seen') == event.event_id
                    finally:
                        call.cancel()
            if mode == 'crash':
                error_line = await asyncio.wait_for(process.stderr.readline(), 3)
                assert b'operation=plugin_exit' in error_line
                assert process.returncode is None
            process.send_signal(signal.SIGTERM)
            output, error = await asyncio.wait_for(process.communicate(), 5)
            assert process.returncode == 0
            assert (b'forced=true' in output) == (mode == 'stop_timeout')
            if mode == 'graceful':
                assert (tmp_path / 'stopped').read_text() == 'shutdown'
        with pytest.raises(ProcessLookupError):
            os.kill(marker['pid'], 0)
        assert b'p' * 32 not in output + error and b'must-not-reach-child' not in output + error
    finally:
        await cleanup(process)


@pytest.mark.parametrize('change', [
    {'KIARABOT_PLUGIN_EXECUTABLE': '/missing/executable'},
    {'KIARABOT_PLUGIN_EXECUTABLE': 'python'},
    {'KIARABOT_PLUGIN_MANIFEST': '/missing/manifest'},
    {'KIARABOT_PLUGIN_STOP_TIMEOUT_MS': 'invalid'},
    {'KIARABOT_PLUGIN_START_TIMEOUT_MS': '0'},
])
async def test_invalid_process_configuration_fails_closed(tmp_path, change):
    process = await launch(dict(environment(tmp_path, 'graceful'), **change))
    try:
        await asyncio.wait_for(process.communicate(), 7)
        assert process.returncode == 1
    finally:
        await cleanup(process)
