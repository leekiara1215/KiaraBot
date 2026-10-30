"""T07a: real Core/SQLite/SDK, protocol fake platform with durable effect journal."""
import asyncio
from contextlib import suppress
from datetime import datetime, timedelta, timezone
import re
import signal

import grpc
import pytest
from kiarabot.v1 import actions_pb2 as actions, adapters_pb2 as adapters, events_pb2 as events
from kiarabot.v1 import common_pb2 as common, services_pb2_grpc as services
from kiarabot_sdk import Options, Plugin, PluginError, Runner
from test_runtime import manifest
from test_actions import ready
from test_process import environment, launch, cleanup


class FakePlatform:
    def __init__(self):
        self.effects = {}
        self.channel = self.call = None

    async def connect(self, endpoint):
        await self.close()
        self.channel = grpc.aio.insecure_channel(endpoint)
        self.call = services.AdapterBridgeStub(self.channel).Connect(
            metadata=(('authorization', 'Bearer ' + 'a' * 32),))
        await self.call.write(adapters.AdapterToCore(register=adapters.RegisterAdapterRequest(
            adapter_id='adapter', protocol_version='kiarabot.v1', implementation='acceptance-fake',
            implementation_version='1', accounts=[{'account_id': 'account', 'enabled': True}],
            capabilities=[{'name': 'action.send_message.text.v1', 'version': '1', 'supported': True}])))
        registration = (await asyncio.wait_for(self.call.read(), 2)).register_result
        assert registration.accepted
        return registration.assigned_instance_id

    def execute(self, action):
        # Same persistent journal survives connection and Core process replacement.
        if action.action_id not in self.effects:
            result = actions.ActionResult(action_id=action.action_id, status=actions.SUCCEEDED,
                                          platform_message_id='platform-message')
            result.completed_at.GetCurrentTime()
            self.effects[action.action_id] = result
        return self.effects[action.action_id]

    async def close(self):
        if self.call:
            self.call.cancel()
        if self.channel:
            await self.channel.close()
        self.call = self.channel = None


@pytest.mark.parametrize('mode', ['success', 'denied', 'disconnect', 'restart'])
async def test_event_plugin_action_result_recovery(tmp_path, mode):
    env = environment(tmp_path, 'graceful')
    for key in ('KIARABOT_PLUGIN_EXECUTABLE', 'KIARABOT_PLUGIN_MANIFEST', 'KIARABOT_PLUGIN_PYTHONPATH'):
        env.pop(key)
    policy = manifest()
    if mode != 'denied':
        policy.permissions.append('message.send')
        env['KIARABOT_PLUGIN_PERMISSIONS'] += ',message.send'
    event = events.EventEnvelope(event_id='closed-loop-event', trace_id='trace', schema_version='kiarabot.v1',
                                 delivery_mode=common.AT_LEAST_ONCE, sequence=1)
    event.occurred_at.GetCurrentTime()
    event.source.adapter_id = 'adapter'
    event.source.account_id = 'account'
    event.subject.conversation_id = 'conversation'
    event.subject.user_id = 'user'
    event.message_received.message_id = 'message'
    event.message_received.sender_id = 'user'
    event.message_received.conversation.id = 'conversation'
    event.message_received.conversation.account_id = 'account'
    event.message_received.conversation.type = common.ConversationRef.PRIVATE
    event.message_received.message.plain_text = 'input'
    invoked = asyncio.Queue()
    deadline = datetime.now(timezone.utc) + timedelta(seconds=60)
    class Sender(Plugin):
        async def on_event(self, received, context):
            action = actions.ActionEnvelope(action_id='closed-loop-action', idempotency_key='closed-loop-key',
                trace_id=received.trace_id, requested_by=policy.id, target_source=received.source,
                target=received.subject)
            action.send_message.conversation.CopyFrom(received.message_received.conversation)
            action.send_message.message.plain_text = 'reply'
            action.deadline.FromDatetime(deadline)
            try:
                result = await context.submit_action(action)
                await invoked.put(result.status)
            except PluginError as error:
                await invoked.put(error)
    platform = FakePlatform()
    process = runner_task = None
    stop = None
    async def start():
        nonlocal process, runner_task, stop
        process = await launch(env)
        line = await asyncio.wait_for(process.stdout.readline(), 5)
        endpoint = '127.0.0.1:' + re.search(rb'port=(\d+)', line)[1].decode()
        stop = asyncio.Event()
        runner = Runner(policy, Sender(), Options(endpoint, 'p' * 32, heartbeat_interval=.05, retry_delay=.02))
        runner_task = asyncio.create_task(runner.run(stop))
        await ready(runner)
        event.source.adapter_instance_id = await platform.connect(endpoint)
        return runner, endpoint
    async def stop_runner():
        if stop:
            stop.set()
        if runner_task:
            await asyncio.wait_for(runner_task, 3)
    try:
        runner, endpoint = await start()
        await platform.call.write(adapters.AdapterToCore(event=event))
        ack = await asyncio.wait_for(platform.call.read(), 2)
        assert ack.event_ack.status == adapters.AdapterEventAck.PERSISTED
        outcome = await asyncio.wait_for(invoked.get(), 2)
        if mode == 'denied':
            assert isinstance(outcome, PluginError) and outcome.code == common.PERMISSION_DENIED
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(platform.call.read(), .2)
            assert not platform.effects
            return
        assert outcome == actions.ACCEPTED
        frame = await asyncio.wait_for(platform.call.read(), 2)
        assert frame.HasField('action')
        result = platform.execute(frame.action)
        if mode in ('disconnect', 'restart'):
            # Lose result after platform effect. Core must resend the same action ID.
            await platform.close()
            if mode == 'restart':
                await stop_runner()
                process.kill()
                await process.wait()
                await cleanup(process)
                runner, endpoint = await start()
            else:
                event.source.adapter_instance_id = await platform.connect(endpoint)
            await platform.call.write(adapters.AdapterToCore(event=event))
            replay = duplicate = False
            for _ in range(2):
                frame = await asyncio.wait_for(platform.call.read(), 2)
                if frame.HasField('action'):
                    assert frame.action.action_id == result.action_id
                    result = platform.execute(frame.action)
                    replay = True
                else:
                    assert frame.event_ack.status == adapters.AdapterEventAck.DUPLICATE
                    duplicate = True
            assert replay and duplicate
        await platform.call.write(adapters.AdapterToCore(action_result=result))
        async with asyncio.timeout(3):
            while True:
                completed = await runner.context.query_action(idempotency_key='closed-loop-key')
                if completed.status == actions.SUCCEEDED:
                    break
                assert completed.status == actions.ACCEPTED
                await asyncio.sleep(.01)
        assert completed.platform_message_id == 'platform-message'
        assert len(platform.effects) == 1
        await platform.call.write(adapters.AdapterToCore(event=event))
        assert (await asyncio.wait_for(platform.call.read(), 2)).event_ack.status == adapters.AdapterEventAck.DUPLICATE
    finally:
        await platform.close()
        await stop_runner()
        if process and process.returncode is None:
            process.send_signal(signal.SIGTERM)
            await asyncio.wait_for(process.communicate(), 5)
        if process:
            await cleanup(process)
