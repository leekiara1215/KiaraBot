import asyncio
import json
import re
import signal
from contextlib import asynccontextmanager, suppress

import pytest
from kiarabot.v1 import common_pb2 as common
from kiarabot_sdk import Options, Plugin, PluginError, Runner
from test_runtime import manifest
from test_actions import ready
from test_process import environment, launch, cleanup


@asynccontextmanager
async def fake_model(mode='success'):
    calls = []
    active = set()
    async def serve(reader, writer):
        task = asyncio.current_task()
        active.add(task)
        try:
            head = await reader.readuntil(b'\r\n\r\n')
            size = int(re.search(rb'(?im)^content-length: (\d+)', head)[1])
            calls.append((head, json.loads(await reader.readexactly(size))))
            if mode == 'disconnect':
                return
            if mode == 'timeout':
                await asyncio.sleep(.3)
            response = {'choices': [{'message': {'content': '你好'}}],
                        'usage': {'prompt_tokens': 3, 'completion_tokens': 2}}
            status = 200
            if mode == 'rate':
                status, response = 429, {'error': 'secret-key-must-not-leak'}
            elif mode == 'failure':
                status, response = 503, {'error': 'secret-key-must-not-leak'}
            elif mode == 'redirect':
                status, response = 302, {}
            elif mode == 'bad_usage':
                response['usage']['prompt_tokens'] = -1
            elif mode == 'tool':
                response['choices'][0]['message']['tool_calls'] = []
            body = json.dumps(response).encode()
            if mode == 'malformed':
                body = b'not json'
            if mode == 'oversize':
                body = b'x' * 1048577
            writer.write(f'HTTP/1.1 {status} Reply\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n'.encode() + body)
            await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError, asyncio.CancelledError):
            pass
        finally:
            writer.close()
            with suppress(ConnectionError):
                await writer.wait_closed()
            active.discard(task)
    server = await asyncio.start_server(serve, '127.0.0.1', 0)
    try:
        yield f'http://127.0.0.1:{server.sockets[0].getsockname()[1]}/chat/completions', calls
    finally:
        server.close()
        await server.wait_closed()
        for task in list(active):
            task.cancel()
        await asyncio.gather(*list(active), return_exceptions=True)


@pytest.mark.parametrize('mode,code', [('success', None), ('denied', common.PERMISSION_DENIED),
    ('disabled', common.CAPABILITY_NOT_SUPPORTED), ('wrong_model', common.CAPABILITY_NOT_SUPPORTED),
    ('rate', common.RATE_LIMITED), ('timeout', common.TIMEOUT),
    *[(mode, common.UPSTREAM_UNAVAILABLE) for mode in
      ('failure', 'disconnect', 'malformed', 'oversize', 'bad_usage', 'tool', 'redirect')]])
async def test_model_actual_cpp_core(tmp_path, mode, code):
    async with fake_model(mode) as (endpoint, calls):
        env = environment(tmp_path, 'graceful')
        for key in ('KIARABOT_PLUGIN_EXECUTABLE', 'KIARABOT_PLUGIN_MANIFEST', 'KIARABOT_PLUGIN_PYTHONPATH'):
            env.pop(key)
        policy = manifest()
        if mode != 'denied':
            policy.permissions.append('model.chat')
            env['KIARABOT_PLUGIN_PERMISSIONS'] += ',model.chat'
        if mode != 'disabled':
            env.update(KIARABOT_MODEL_ENDPOINT=endpoint, KIARABOT_MODEL_API_KEY='secret-key-must-not-leak',
                       KIARABOT_MODEL_NAME='default')
        process = await launch(env)
        task = None
        stop = asyncio.Event()
        try:
            line = await asyncio.wait_for(process.stdout.readline(), 5)
            endpoint = '127.0.0.1:' + re.search(rb'port=(\d+)', line)[1].decode()
            runner = Runner(policy, Plugin(), Options(endpoint, 'p' * 32, heartbeat_interval=.05))
            task = asyncio.create_task(runner.run(stop))
            await ready(runner)
            request = runner.context.model_chat('wrong' if mode == 'wrong_model' else 'default',
                [{'role': 'user', 'text': '你好'}], timeout=.08 if mode == 'timeout' else 2)
            if code:
                with pytest.raises(PluginError) as failure:
                    await request
                assert failure.value.code == code
                assert 'secret' not in str(failure.value)
                assert failure.value.retryable == (mode == 'rate')
            else:
                response = await request
                assert (response.text, response.input_tokens, response.output_tokens) == ('你好', 3, 2)
            assert len(calls) == (0 if mode in ('denied', 'disabled', 'wrong_model') else 1)
            if calls:
                assert b'Authorization: Bearer secret-key-must-not-leak' in calls[0][0]
                assert calls[0][1] == {'model': 'default', 'stream': False,
                                     'messages': [{'role': 'user', 'content': '你好'}]}
        finally:
            stop.set()
            if task:
                await asyncio.wait_for(task, 3)
            if process.returncode is None:
                process.send_signal(signal.SIGTERM)
            output, error = await asyncio.wait_for(process.communicate(), 5)
            await cleanup(process)
            assert b'secret-key-must-not-leak' not in output + error
            assert process.returncode == 0
        assert b'secret-key-must-not-leak' not in (tmp_path / 'runtime.db').read_bytes()


@pytest.mark.parametrize('url', ['http://example.com/chat', 'https://user:secret@example.com/chat',
                                 'https://example.com/chat?key=secret', 'file:///tmp/model'])
async def test_invalid_provider_configuration(tmp_path, url):
    env = environment(tmp_path, 'graceful')
    env.update(KIARABOT_MODEL_ENDPOINT=url, KIARABOT_MODEL_API_KEY='secret-key-must-not-leak',
               KIARABOT_MODEL_NAME='default')
    process = await launch(env)
    try:
        output, error = await asyncio.wait_for(process.communicate(), 5)
        assert process.returncode == 1 and b'INVALID_ARGUMENT' in error
        assert b'secret' not in output + error
        assert not (tmp_path / 'marker.json').exists()
    finally:
        await cleanup(process)


@pytest.mark.parametrize('timeout', [False, True])
async def test_ai_example_replay_never_repeats_model(timeout):
    from ai_reply import create_plugin
    from datetime import datetime, timezone
    from kiarabot.v1 import plugins_pb2 as plugins, events_pb2 as events
    class Context:
        values = {}
        calls = 0
        actions = []
        async def storage_get(self, key):
            if key not in self.values:
                raise PluginError(common.NOT_FOUND)
            return self.values[key]
        async def storage_set(self, key, value, idempotency_key):
            self.values[key] = value
        async def model_chat(self, model, messages, timeout):
            self.calls += 1
            if fail:
                raise PluginError(common.TIMEOUT)
            return plugins.ModelChatResponse(text='answer')
        async def submit_action(self, action):
            self.actions.append(action)
    fail = timeout
    context = Context()
    event = events.EventEnvelope(event_id='event-1', trace_id='trace')
    event.occurred_at.FromDatetime(datetime.now(timezone.utc))
    event.message_received.message.plain_text = '/ai hello'
    event.message_received.conversation.id = '123'
    event.message_received.conversation.type = common.ConversationRef.GROUP
    if fail:
        with pytest.raises(PluginError):
            await create_plugin().on_event(event, context)
    else:
        await create_plugin().on_event(event, context)
    await create_plugin().on_event(event, context)
    assert context.calls == 1
    assert len(context.actions) == (0 if fail else 2)
    if not fail:
        assert context.actions[0] == context.actions[1]
