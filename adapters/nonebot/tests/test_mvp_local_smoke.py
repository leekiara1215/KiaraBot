"""Local MVP smoke: real NoneBot + reference Core/SQLite + fixed SDK Sender.

Only the OneBot endpoint is a local protocol fixture. No QQ account or model is used.
"""
import asyncio
from contextlib import suppress
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import sys
import time

from websockets.asyncio.server import serve

from kiarabot.v1 import actions_pb2 as actions
from kiarabot_adapter_nonebot.mapper import map_message
from test_mapping_outbox import raw


async def stop_child(process):
    if process is None:
        return
    if process.returncode is None:
        process.send_signal(signal.SIGTERM)
    try:
        await asyncio.wait_for(process.wait(), 5)
    except asyncio.TimeoutError:
        process.kill()
        await asyncio.wait_for(process.wait(), 5)
        raise AssertionError('owned MVP child required forced termination') from None


async def test_real_nonebot_reference_core_fixed_reply_and_duplicate(tmp_path, monkeypatch):
    root = Path(__file__).resolve().parents[3]
    binary = Path(os.environ.get('CORE_TEST_BINARY',
                                root / 'build/full-locked/reference/core/kiarabot-core')).resolve()
    assert binary.is_file() and os.access(binary, os.X_OK), 'Build the reference Core first'
    sdk = Path(os.environ.get('SDK_SOURCE', root / 'sdk/python/src')).resolve()
    assert sdk.is_dir(), 'SDK_SOURCE must identify the matching SDK source'
    monkeypatch.syspath_prepend(str(sdk))
    from kiarabot_sdk import Options, Plugin, Runner, load_manifest

    handler_root = root / 'plugins/local-reply'
    spec = importlib.util.spec_from_file_location('kiarabot_mvp_local_reply_handler', handler_root / 'main.py')
    assert spec and spec.loader, 'Published local handler is required'
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    handler = module.create()
    manifest = load_manifest(handler_root / 'plugin.json')
    assert isinstance(handler, Plugin) and manifest.id == 'local-reply'
    assert manifest.entrypoint == 'main:create'

    reply_text = '收到：hello'
    received = []
    completed = asyncio.Queue()
    platform_calls = []
    socket_ready = asyncio.get_running_loop().create_future()
    failure = asyncio.get_running_loop().create_future()
    core = adapter = runner_task = None
    readers = []
    child_output = []
    stop = asyncio.Event()
    adapter_root = root / 'adapters/nonebot'
    outbox = tmp_path / 'adapter.db'

    submitted = []

    class ObservedContext:
        def __init__(self, context):
            self.context = context
            self.key = None

        def __getattr__(self, name):
            return getattr(self.context, name)

        async def query_action(self, **kwargs):
            self.key = kwargs['idempotency_key']
            return await self.context.query_action(**kwargs)

        async def submit_action(self, action, **kwargs):
            self.key = action.idempotency_key
            submitted.append(action.action_id)
            return await self.context.submit_action(action, **kwargs)

    class ObservedHandler(Plugin):
        async def on_event(self, event, context):
            if not event.HasField('message_received'):
                return
            try:
                received.append(event.event_id)
                observed = ObservedContext(context)
                await handler.on_event(event, observed)
                assert observed.key, 'Published handler did not inspect or submit an Action'
                async with asyncio.timeout(5):
                    while True:
                        result = await context.query_action(idempotency_key=observed.key)
                        if result.status != actions.ACCEPTED:
                            assert result.status == actions.SUCCEEDED
                            assert result.platform_message_id in ('987', '988')
                            await completed.put(event.event_id)
                            return
                        await asyncio.sleep(.01)
            except Exception as error:
                if not failure.done():
                    failure.set_result(error)
                raise

    first = raw()
    first.update(time=int(time.time()), sub_type='normal', raw_message='hello', font=0, sender={})
    second = dict(first, message_id=2)
    expected = [map_message(value, 'adapter', '123').event_id for value in (first, second)]

    async def drain(stream):
        child_output.append(await stream.read())

    async def platform(socket):
        try:
            assert socket.request.headers['Authorization'] == 'Bearer mvp-platform-token'
            assert not socket_ready.done(), 'unexpected platform reconnect'
            socket_ready.set_result(socket)
            await socket.send(json.dumps(dict(time=1700000000, self_id=123,
                post_type='meta_event', meta_event_type='lifecycle', sub_type='connect')))
            await socket.send(json.dumps(first))
            async for encoded in socket:
                call = json.loads(encoded)
                platform_calls.append(call)
                assert call['action'] == 'send_group_msg'
                assert call['params'] == {'group_id': 789,
                    'message': [{'type': 'text', 'data': {'text': reply_text}}]}
                await socket.send(json.dumps(dict(status='ok', retcode=0,
                    data={'message_id': 986 + len(platform_calls)}, echo=call['echo'])))
        except Exception as error:
            if not failure.done():
                failure.set_result(error)
            raise

    async def progress(awaitable):
        task = asyncio.create_task(awaitable)
        try:
            done, _ = await asyncio.wait({task, failure}, timeout=10,
                                         return_when=asyncio.FIRST_COMPLETED)
            assert done, 'MVP local smoke did not make progress'
            if failure in done:
                raise failure.result()
            assert core.returncode is None and adapter.returncode is None, 'MVP child exited early'
            return task.result()
        finally:
            if not task.done():
                task.cancel()
                with suppress(asyncio.CancelledError):
                    await task

    try:
        async with asyncio.timeout(35):
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(('KIARABOT_', 'PLUGIN_', 'ONEBOT_', 'CORE_'))}
            environment.update(KIARABOT_ADAPTER_ID='adapter', KIARABOT_ADAPTER_TOKEN='a' * 32,
                KIARABOT_ADAPTER_ACCOUNTS='123', KIARABOT_RUNTIME_DB=str(tmp_path / 'core.db'),
                KIARABOT_PLUGIN_ID='local-reply', KIARABOT_PLUGIN_TOKEN='p' * 32,
                KIARABOT_PLUGIN_PERMISSIONS='message.read,message.send')
            core = await asyncio.create_subprocess_exec(str(binary), '--grpc-listen', '127.0.0.1:0',
                env=environment, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            readers.append(asyncio.create_task(drain(core.stderr)))
            line = await asyncio.wait_for(core.stdout.readline(), 5)
            assert b'plugin_delivery_ready=true' in line and b'adapter_ingress_ready=true' in line
            endpoint = '127.0.0.1:' + re.search(rb'port=(\d+)', line)[1].decode()
            readers.append(asyncio.create_task(drain(core.stdout)))
            runner = Runner(manifest, ObservedHandler(), Options(endpoint, 'p' * 32,
                heartbeat_interval=.05, retry_delay=.01))
            runner_task = asyncio.create_task(runner.run(stop))
            async with asyncio.timeout(5):
                while not runner.context._rpc:
                    assert not runner_task.done(), 'fixed Sender exited early'
                    await asyncio.sleep(.01)
            async with serve(platform, '127.0.0.1', 0) as server:
                port = server.sockets[0].getsockname()[1]
                adapter_environment = dict(environment, CORE_GRPC_ENDPOINT=endpoint,
                    CORE_AUTH_TOKEN='a' * 32, ADAPTER_ID='adapter', ACCOUNT_ID='123',
                    ONEBOT_WS_URL=f'ws://127.0.0.1:{port}', ONEBOT_ACCESS_TOKEN='mvp-platform-token',
                    OUTBOX_PATH=str(outbox), HEALTH_PORT='0',
                    PYTHONPATH=os.pathsep.join([str(adapter_root / 'src'), str(adapter_root / 'generated')]))
                adapter = await asyncio.create_subprocess_exec(sys.executable, '-m',
                    'kiarabot_adapter_nonebot', cwd=adapter_root, env=adapter_environment,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
                readers.extend(asyncio.create_task(drain(stream)) for stream in (adapter.stdout, adapter.stderr))
                assert await progress(completed.get()) == expected[0]
                socket = await asyncio.wait_for(socket_ready, 2)
                await socket.send(json.dumps(first))
                await socket.send(json.dumps(first))
                # A later distinct message is an ordered processing barrier, not a sleep-only check.
                await socket.send(json.dumps(second))
                assert await progress(completed.get()) == expected[1]
                assert received == expected and len(platform_calls) == 2
                assert len(submitted) == len(set(submitted)) == 2
                with sqlite3.connect(outbox) as connection:
                    assert [connection.execute('SELECT status FROM outbox WHERE id=?', (event_id,)).fetchone()
                            for event_id in expected] == [('acked',), ('acked',)]
                await stop_child(adapter)
                assert adapter.returncode == 0
    finally:
        stop.set()
        runner_results = []
        if runner_task:
            runner_task.cancel()
            runner_results = await asyncio.gather(runner_task, return_exceptions=True)
        # Always reap both owned children, even if the Sender or another shutdown raises.
        results = await asyncio.gather(stop_child(adapter), stop_child(core), return_exceptions=True)
        await asyncio.wait_for(asyncio.gather(*readers, return_exceptions=True), 5)
        if not failure.done():
            failure.cancel()
        for result in [*runner_results, *results]:
            if isinstance(result, BaseException) and not isinstance(result, asyncio.CancelledError):
                raise result
    assert core.returncode == 0
    assert all(token not in output for output in child_output
               for token in (b'mvp-platform-token', b'a' * 32, b'p' * 32))
