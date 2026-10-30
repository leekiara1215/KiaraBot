"""C++ Core + Python SDK + Python adapter, including durable result after restart."""
import asyncio
from contextlib import suppress
import os
from pathlib import Path
import re
import signal
import sys
import time

import pytest
from kiarabot.v1 import actions_pb2 as actions, plugins_pb2 as plugins
from kiarabot_adapter_nonebot.actions import ActionExecutor
from kiarabot_adapter_nonebot.client import ClientConfig, CoreClient
from kiarabot_adapter_nonebot.outbox import Outbox
from kiarabot_adapter_nonebot.mapper import map_message
from test_actions import request
from test_client import until
from test_mapping_outbox import raw


@pytest.mark.skipif(not os.environ.get('CORE_TEST_BINARY'), reason='Set CORE_TEST_BINARY for C++ integration')
async def test_sdk_core_adapter_action_and_restart(tmp_path):
    binary = Path(os.environ['CORE_TEST_BINARY']).resolve()
    sdk = Path(os.environ.get('SDK_SOURCE', binary.parents[3] / 'sdk/python/src'))
    assert sdk.is_dir(), 'SDK_SOURCE must identify the matching Python SDK'
    sys.path.insert(0, str(sdk))
    from kiarabot_sdk import Plugin, Options, Runner
    calls = []
    async def platform(api, params):
        calls.append((api, params))
        return {'message_id': 'pipeline-message'}
    action = request(int(time.time()) + 60)
    action.target_source.adapter_instance_id = ''
    completed = asyncio.Event()
    class Sender(Plugin):
        async def on_event(self, event, context):
            accepted = await context.submit_action(action)
            assert accepted.status in (actions.ACCEPTED, actions.SUCCEEDED)
            async with asyncio.timeout(4):
                while True:
                    result = await context.query_action(idempotency_key='key')
                    if result.status != actions.ACCEPTED:
                        assert result.status == actions.SUCCEEDED and result.platform_message_id == 'pipeline-message'
                        completed.set()
                        return
                    await asyncio.sleep(.01)
    manifest = plugins.PluginManifest(id='plugin', name='plugin', version='1.0', api_version='kiarabot.plugin.v1',
        runtime='python-rpc', entrypoint='fake:create', isolation='process', ordered=True,
        capabilities=['event.message.received'], permissions=['message.read', 'message.send'])
    environment = {key: value for key, value in os.environ.items() if not key.startswith('KIARABOT_')}
    environment.update(KIARABOT_ADAPTER_ID='adapter', KIARABOT_ADAPTER_TOKEN='x' * 32,
        KIARABOT_ADAPTER_ACCOUNTS='123', KIARABOT_RUNTIME_DB=str(tmp_path / 'core.db'),
        KIARABOT_PLUGIN_ID='plugin', KIARABOT_PLUGIN_TOKEN='p' * 32,
        KIARABOT_PLUGIN_PERMISSIONS='message.read,message.send')
    for attempt in range(2):
        process = await asyncio.create_subprocess_exec(str(binary), '--grpc-listen', '127.0.0.1:0', env=environment,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        tasks = []
        outbox = executor = None
        try:
            line = await asyncio.wait_for(process.stdout.readline(), 5)
            assert b'plugin_delivery_ready=true' in line
            endpoint = '127.0.0.1:' + re.search(rb'port=(\d+)', line)[1].decode()
            outbox = Outbox(tmp_path / 'outbox.db')
            executor = ActionExecutor(tmp_path / 'actions.db', 'adapter', '123', platform)
            stop = asyncio.Event()
            client = CoreClient(ClientConfig(endpoint, 'x' * 32, 'adapter', '123', idle_interval=.01, retry_base=.01), outbox, executor)
            tasks.append(asyncio.create_task(client.run(stop)))
            runner = Runner(manifest, Sender(), Options(endpoint, 'p' * 32, heartbeat_interval=.05, retry_delay=.01))
            tasks.append(asyncio.create_task(runner.run(stop)))
            await until(lambda: runner.context._rpc and client.instance)
            if attempt == 0:
                outbox.enqueue(map_message(raw(), 'adapter', '123'))
                await asyncio.wait_for(completed.wait(), 5)
            else:
                result = await runner.context.query_action(action_id='action')
                assert result.status == actions.SUCCEEDED and result.platform_message_id == 'pipeline-message'
                action.action_id = 'new-id-same-key'
                duplicate = await runner.context.submit_action(action)
                assert duplicate.action_id == 'action' and duplicate.status == actions.SUCCEEDED
            assert len(calls) == 1
        finally:
            for task in tasks:
                task.cancel()
            for task in tasks:
                with suppress(asyncio.CancelledError):
                    await task
            if executor:
                executor.close()
            if outbox:
                outbox.close()
            if process.returncode is None:
                process.send_signal(signal.SIGTERM)
                try:
                    await asyncio.wait_for(process.communicate(), 5)
                except asyncio.TimeoutError:
                    process.kill()
                    await process.communicate()
                    raise
            assert process.returncode == 0
