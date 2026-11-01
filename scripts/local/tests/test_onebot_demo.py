"""Local subprocess and real NoneBot WS tests; no Core/QQ/model substitute involved."""
import asyncio
import json
import os
from pathlib import Path
import secrets
import signal
import shutil
import tempfile
import sys

import pytest
from websockets.asyncio.client import connect
from websockets.exceptions import InvalidStatus

SCRIPT = Path(__file__).resolve().parents[1] / 'onebot_demo.py'


@pytest.fixture
async def demo():
    tmp_path = Path(tempfile.mkdtemp(prefix='kb-demo-', dir='/tmp'))
    os.chmod(tmp_path, 0o700)
    token = secrets.token_hex(32)
    token_path = tmp_path / 'token'
    token_path.write_text(token + '\n')
    token_path.chmod(0o600)
    socket = tmp_path / 'demo.sock'
    process = await asyncio.create_subprocess_exec(sys.executable, str(SCRIPT), 'serve',
        '--token-file', str(token_path), '--control-socket', str(socket), '--port', '0',
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    state = json.loads(await asyncio.wait_for(process.stdout.readline(), 5))
    if state.get('event') != 'ready':
        await process.communicate()
        shutil.rmtree(tmp_path)
        pytest.fail(str(state))
    assert state['host'] == '127.0.0.1'
    assert token not in json.dumps(state) and socket.stat().st_mode & 0o777 == 0o600
    try:
        yield state, token, token_path
    finally:
        if process.returncode is None:
            process.send_signal(signal.SIGTERM)
        output, error = await asyncio.wait_for(process.communicate(), 5)
        assert process.returncode == 0, error
        assert token.encode() not in output + error
        assert not socket.exists()
        shutil.rmtree(tmp_path)


async def command(state, token, **fields):
    reader, writer = await asyncio.open_unix_connection(state['control_socket'])
    writer.write(json.dumps(dict(token=token, **fields)).encode() + b'\n')
    await writer.drain()
    result = json.loads(await asyncio.wait_for(reader.readline(), 5))
    writer.close()
    await writer.wait_closed()
    return result


async def test_protocol_auth_shapes_and_timeout(demo):
    state, token, _ = demo
    uri = f"ws://127.0.0.1:{state['port']}"
    with pytest.raises(InvalidStatus) as rejected:
        async with connect(uri, additional_headers={'Authorization': 'Bearer wrong'}):
            pass
    assert rejected.value.response.status_code == 401
    assert (await command(state, 'wrong', command='status'))['error'] == 'unauthorized'
    assert (await command(state, token, command='status'))['connected'] is False
    request = dict(command='message', message_type='group', user_id=789, group_id=456, text='hello')
    assert (await command(state, token, **request))['error'] == 'not_connected'
    async with connect(uri, additional_headers={'Authorization': f'Bearer {token}'}) as socket:
        lifecycle = json.loads(await socket.recv())
        assert lifecycle['self_id'] == 123 and lifecycle['meta_event_type'] == 'lifecycle'
        for action in ('get_login_info', 'get_version_info', 'get_status', 'delete_msg', 'send_group_msg'):
            await socket.send(json.dumps(dict(action=action, params={}, echo=action)))
            result = json.loads(await socket.recv())
            assert result['echo'] == action
            assert result['retcode'] == (1404 if action == 'delete_msg' else 1400 if action == 'send_group_msg' else 0)
        malformed = dict(request, user_id=True)
        assert (await command(state, token, **malformed))['error'] == 'invalid_id'
        pending = asyncio.create_task(command(state, token, **dict(request, timeout=0.05)))
        assert json.loads(await socket.recv())['message_type'] == 'group'
        assert (await pending)['error'] == 'reply_timeout'


async def test_bounded_control_and_unsafe_token(demo):
    state, token, token_path = demo
    reader, writer = await asyncio.open_unix_connection(state['control_socket'])
    writer.write(b'{"token":"x","token":"y","command":"status"}\n')
    await writer.drain()
    assert json.loads(await reader.readline())['error'] == 'invalid_request'
    writer.close()
    await writer.wait_closed()
    reader, writer = await asyncio.open_unix_connection(state['control_socket'])
    writer.write(b'x' * 8193 + b'\n')
    await writer.drain()
    assert json.loads(await reader.readline())['error'] == 'invalid_request'
    writer.close()
    await writer.wait_closed()
    token_path.chmod(0o644)
    cli = await asyncio.create_subprocess_exec(sys.executable, str(SCRIPT), 'status',
        '--token-file', str(token_path), '--control-socket', state['control_socket'],
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    output, error = await cli.communicate()
    token_path.chmod(0o600)
    assert cli.returncode == 1 and json.loads(output)['error'] == 'unsafe_token_file'
    assert token.encode() not in output + error


# An actual NoneBot driver and OneBot adapter, without FakeCore or a fake Bot API. Its test-only
# matcher uses the same concrete send APIs as the repository adapter; event parsing and WS routing are real.
NONEBOT = r'''
import os
from pathlib import Path
import nonebot
from nonebot.adapters.onebot.v11 import Adapter, Bot, MessageEvent
from nonebot.log import logger
logger.remove()
nonebot.init(driver='~websockets', onebot_ws_urls=[os.environ['DEMO_WS']],
             onebot_access_token=Path(os.environ['DEMO_TOKEN_FILE']).read_text().strip(),
             log_level='ERROR')
nonebot.get_driver().register_adapter(Adapter)
matcher = nonebot.on_message(priority=1)
@matcher.handle()
async def reply(bot: Bot, event: MessageEvent):
    kind = event.message_type
    target = {'group_id': event.group_id} if kind == 'group' else {'user_id': event.user_id}
    await bot.call_api('send_' + kind + '_msg', **target,
                       message=[{'type': 'text', 'data': {'text': 'demo reply: ' + event.get_plaintext()}}])
nonebot.run()
'''


async def test_real_nonebot_group_private_and_cli(demo):
    state, token, token_path = demo
    environment = dict(os.environ, DEMO_WS=f"ws://127.0.0.1:{state['port']}",
                       DEMO_TOKEN_FILE=str(token_path))
    bot = await asyncio.create_subprocess_exec(sys.executable, '-c', NONEBOT, env=environment,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    try:
        for _ in range(100):
            if (await command(state, token, command='status'))['connected']:
                break
            assert bot.returncode is None
            await asyncio.sleep(0.05)
        else:
            pytest.fail('real NoneBot did not connect')
        for kind in ('group', 'private'):
            cli = await asyncio.create_subprocess_exec(sys.executable, str(SCRIPT), 'message',
                '--token-file', str(token_path), '--control-socket', state['control_socket'],
                '--type', kind, '--text', 'hello 中文', '--timeout', '3',
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            output, error = await asyncio.wait_for(cli.communicate(), 5)
            assert cli.returncode == 0, (output, error)
            result = json.loads(output)
            assert result['ok'] and result['reply']['action'] == f'send_{kind}_msg'
            assert result['reply']['params']['message'][0]['data']['text'] == 'demo reply: hello 中文'
            assert token.encode() not in output + error
        assert len((await command(state, token, command='status'))['replies']) == 2
    finally:
        if bot.returncode is None:
            bot.send_signal(signal.SIGTERM)
        try:
            await asyncio.wait_for(bot.communicate(), 5)
        except TimeoutError:
            bot.kill()
            await bot.communicate()
