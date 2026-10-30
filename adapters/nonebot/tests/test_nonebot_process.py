import asyncio
import json
import os
from pathlib import Path
import signal
import sqlite3
import sys

import grpc
from websockets.asyncio.server import serve
from kiarabot.v1 import services_pb2_grpc as services
from kiarabot_adapter_nonebot.mapper import map_message
from test_client import FakeCore, until
from test_mapping_outbox import raw


async def test_nonebot_process_with_fake_platform(tmp_path):
    core = FakeCore('action')
    platform_calls = []
    server = grpc.aio.server()
    services.add_AdapterBridgeServicer_to_server(core, server)
    port = server.add_insecure_port('127.0.0.1:0')
    await server.start()
    source = raw()
    source.update(sub_type='normal', raw_message='hello', font=0, sender={})
    expected_id = map_message(source, 'adapter', '123').event_id
    outbox = tmp_path / 'outbox.db'
    process = None

    async def platform(socket):
        assert socket.request.headers['Authorization'] == 'Bearer platform-test-token'
        await socket.send(json.dumps(dict(time=1700000000, self_id=123, post_type='meta_event',
                                          meta_event_type='lifecycle', sub_type='connect')))
        await socket.send(json.dumps(source))
        async for encoded in socket:
            call = json.loads(encoded)
            platform_calls.append(call)
            assert call['action'] == 'send_group_msg'
            assert call['params']['group_id'] == 456
            assert call['params']['message'][0]['type'] == 'text'
            await socket.send(json.dumps(dict(status='ok', retcode=0, data={'message_id': 987}, echo=call['echo'])))

    try:
        async with serve(platform, '127.0.0.1', 0) as websocket:
            ws_port = websocket.sockets[0].getsockname()[1]
            root = Path(__file__).resolve().parents[1]
            environment = dict(os.environ, CORE_GRPC_ENDPOINT=f'127.0.0.1:{port}',
                CORE_AUTH_TOKEN='x' * 32, ADAPTER_ID='adapter', ACCOUNT_ID='123',
                ONEBOT_WS_URL=f'ws://127.0.0.1:{ws_port}', ONEBOT_ACCESS_TOKEN='platform-test-token',
                OUTBOX_PATH=str(outbox), PYTHONPATH=os.pathsep.join([str(root/'src'), str(root/'generated')]))
            process = await asyncio.create_subprocess_exec(sys.executable, '-m', 'kiarabot_adapter_nonebot',
                cwd=root, env=environment, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            await until(lambda: len(core.events) == 1 or process.returncode is not None)
            assert len(core.events) == 1, (await process.communicate()) if process.returncode is not None else ''
            assert core.events[0].event_id == expected_id

            def acknowledged():
                with sqlite3.connect(outbox) as connection:
                    return connection.execute('SELECT status FROM outbox WHERE id=?', (expected_id,)).fetchone() == ('acked',)
            await until(acknowledged)
            await until(lambda: len(core.results) == 1)
            assert core.results[0].status == 2 and core.results[0].platform_message_id == '987'
            assert len(platform_calls) == 1
            process.send_signal(signal.SIGTERM)
            output, error = await asyncio.wait_for(process.communicate(), 5)
            assert process.returncode == 0, error.decode()
            assert b'platform-test-token' not in output + error and b'x' * 32 not in output + error
    finally:
        if process and process.returncode is None:
            process.kill()
            await process.wait()
        await server.stop(0)
