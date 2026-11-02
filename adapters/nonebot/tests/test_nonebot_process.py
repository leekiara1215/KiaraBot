"""Real NoneBot adapter process against the TLS FakeTargetRuntime and a loopback OneBot platform.

Only the platform websocket and the target Runtime bridge are local fixtures; the adapter runs
as a real child process with the target bootstrap/TLS configuration, and its fenced event frames
and Action execution are observed through those two fixtures.
"""
import asyncio
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import sys
import time
import uuid

from websockets.asyncio.server import serve
from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common
from kiarabot.v1 import platform_pb2 as platform_schema
from kiarabot_adapter_nonebot.mapper import map_message
from fake_target_runtime import FakeTargetRuntime, until
from test_mapping_outbox import raw

ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def adapter_environment(runtime, entry, bootstrap_file, *, health_port, onebot_url, outbox):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('BRIDGE_', 'ADAPTER_', 'ACCOUNT_', 'OUTBOX_',
                                          'HEALTH_', 'ONEBOT_', 'PYTHONPATH'))}
    environment.update(
        BRIDGE_ENDPOINT=runtime.endpoint,
        BRIDGE_CA_FILE=str(runtime.certificate),
        BRIDGE_BOOTSTRAP_CREDENTIAL_FILE=str(bootstrap_file),
        ADAPTER_INSTANCE_ID=entry.instance_id,
        ACCOUNT_ID='123',
        OUTBOX_PATH=str(outbox),
        HEALTH_PORT=str(health_port),
        ONEBOT_WS_URL=onebot_url,
        ONEBOT_ACCESS_TOKEN='platform-test-token',
        PYTHONPATH=os.pathsep.join([str(ROOT / 'src'), str(ROOT / 'generated')]),
    )
    return environment


def probe(path, port):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=2)
    try:
        connection.request('GET', path)
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


async def wait_probe(port, expected, timeout=10):
    async with asyncio.timeout(timeout):
        while True:
            try:
                status, body = probe('/health/ready', port)
                if status == expected:
                    return json.loads(body)
            except (OSError, ValueError):
                pass
            await asyncio.sleep(0.02)


def action_request(adapter_id, instance, account_id='123'):
    value = actions.ActionEnvelope(action_id=str(uuid.uuid4()), idempotency_key=str(uuid.uuid4()),
                                   trace_id=str(uuid.uuid4()))
    value.deadline.seconds = int(time.time()) + 30
    value.target_source.adapter_id = adapter_id
    value.target_source.account_id = account_id
    value.target_source.adapter_instance_id = instance
    value.target.conversation_id = '456'
    value.send_message.conversation.id = '456'
    value.send_message.conversation.account_id = account_id
    value.send_message.conversation.type = common.ConversationRef.GROUP
    value.send_message.message.plain_text = 'hello'
    return value


def outbox_acked(outbox, event_id):
    try:
        with sqlite3.connect(outbox) as connection:
            row = connection.execute('SELECT status FROM outbox WHERE id=?',
                                     (event_id,)).fetchone()
            return row == ('acked',)
    except sqlite3.Error:
        return False


async def test_nonebot_process_target_bridge_and_platform_action(tmp_path, server_tls):
    from tls_fixture import write_bootstrap_secret

    platform_calls = []
    source = raw()
    source.update(sub_type='normal', raw_message='hello', font=0, sender={})
    async with FakeTargetRuntime(str(server_tls[0]), str(server_tls[1]),
                                 heartbeat_interval=1) as runtime:
        entry = runtime.add_bootstrap(FakeTargetRuntime.credential())
        bootstrap_file = write_bootstrap_secret(tmp_path, entry.credential)
        health_port = free_port()
        process = None
        failure = []

        async def platform(socket):
            try:
                assert socket.request.headers['Authorization'] == 'Bearer platform-test-token'
                await socket.send(json.dumps(dict(time=1700000000, self_id=123,
                                                  post_type='meta_event',
                                                  meta_event_type='lifecycle',
                                                  sub_type='connect')))
                await socket.send(json.dumps(source))
                async for encoded in socket:
                    call = json.loads(encoded)
                    platform_calls.append(call)
                    assert call['action'] == 'send_group_msg'
                    assert call['params'] == {'group_id': 456, 'message': [
                        {'type': 'text', 'data': {'text': 'hello'}}]}
                    await socket.send(json.dumps(dict(status='ok', retcode=0, data={
                        'message_id': 987}, echo=call['echo'])))
            except Exception as error:  # noqa: BLE001 - surfaced through ``failure``
                failure.append(error)

        async with serve(platform, '127.0.0.1', 0) as websocket:
            port = websocket.sockets[0].getsockname()[1]
            outbox = tmp_path / 'outbox.db'
            environment = adapter_environment(
                runtime, entry, bootstrap_file, health_port=health_port,
                onebot_url=f'ws://127.0.0.1:{port}', outbox=outbox)
            process = await asyncio.create_subprocess_exec(
                sys.executable, '-m', 'kiarabot_adapter_nonebot', cwd=ROOT, env=environment,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            output = error = b''
            try:
                await until(lambda: runtime.connections and any(
                    event.HasField('message_received')
                    for event in runtime.connections[0].events), 20)
                connection = runtime.connections[0]
                message = next(event for event in connection.events
                               if event.HasField('message_received'))
                assert connection.registration.adapter_instance_id == entry.instance_id
                assert connection.registration.adapter_id == entry.instance_id
                assert connection.registration.accounts[0].account_id == '123'
                expected = map_message(source, entry.instance_id, '123').event_id
                assert message.event_id == expected
                await until(lambda: outbox.exists() and outbox_acked(outbox, expected), 10,
                            interval=0.02)
                envelope = await runtime.dispatch_action(
                    action_request(entry.instance_id, entry.instance_id))
                await until(lambda: runtime.connections[0].action_results, 10)
                result = runtime.connections[0].action_results[0]
                assert result.dispatch_attempt_id == envelope.dispatch_attempt_id
                assert result.status == actions.SUCCEEDED
                assert result.execution_outcome == platform_schema.EXECUTION_KNOWN_SUCCEEDED
                assert result.platform_message_id == '987'
                await until(lambda: runtime.connections[0].receipts, 10)
                assert runtime.connections[0].receipts[0].dispatch_attempt_id == \
                    envelope.dispatch_attempt_id
                await until(lambda: platform_calls, 10, interval=0.02)
                assert not failure, failure
                ready = await wait_probe(health_port, 200)
                assert ready == {'state': 'READY', 'components': {
                    'authorization': 'HEALTHY', 'core': 'HEALTHY', 'platform': 'HEALTHY'}}
                if process.returncode is not None:
                    raise AssertionError('adapter exited early')
                process.send_signal(signal.SIGTERM)
                output, error = await asyncio.wait_for(process.communicate(), 10)
                assert process.returncode == 0, error.decode()
            finally:
                if process is not None and process.returncode is None:
                    process.kill()
                    await process.wait()
            assert entry.credential.encode() not in output + error
            assert b'platform-test-token' not in output + error


async def test_terminal_bridge_failure_exits_non_zero(tmp_path, server_tls):
    from tls_fixture import write_bootstrap_secret

    async with FakeTargetRuntime(str(server_tls[0]), str(server_tls[1])) as runtime:
        runtime.registration_policy = lambda registration: ('reject', common.INVALID_ARGUMENT)
        entry = runtime.add_bootstrap(FakeTargetRuntime.credential())
        bootstrap_file = write_bootstrap_secret(tmp_path, entry.credential)
        environment = adapter_environment(
            runtime, entry, bootstrap_file, health_port=free_port(),
            onebot_url='ws://127.0.0.1:1', outbox=tmp_path / 'outbox.db')
        process = await asyncio.create_subprocess_exec(
            sys.executable, '-m', 'kiarabot_adapter_nonebot', cwd=ROOT, env=environment,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        try:
            output, error = await asyncio.wait_for(process.communicate(), 30)
        finally:
            if process.returncode is None:
                process.kill()
                await process.wait()
        assert process.returncode == 1, (output, error)
        assert b'INVALID_ARGUMENT' in output + error
        assert entry.credential.encode() not in output + error
