import asyncio
import copy
from contextlib import suppress

import grpc
import pytest
from kiarabot.v1 import adapters_pb2 as adapters, common_pb2 as common, services_pb2_grpc as services
from kiarabot_adapter_nonebot.client import ClientConfig, CoreClient
from kiarabot_adapter_nonebot.mapper import AdapterError, map_message
from kiarabot_adapter_nonebot.outbox import Outbox
from test_mapping_outbox import raw


class FakeCore(services.AdapterBridgeServicer):
    def __init__(self, mode):
        self.mode, self.registrations, self.events = mode, [], []
        self.results = []
        self.received, self.release = asyncio.Event(), asyncio.Event()

    async def Connect(self, requests, context):
        registration = await anext(requests)
        assert registration.WhichOneof('body') == 'register'
        assert dict(context.invocation_metadata())['authorization'] == 'Bearer ' + 'x' * 32
        self.registrations.append(copy.deepcopy(registration.register))
        instance = f'instance-{len(self.registrations)}'
        yield adapters.CoreToAdapter(register_result=adapters.RegisterAdapterResponse(
            accepted=True, assigned_instance_id=instance, required_protocol_version='kiarabot.v1',
            heartbeat_interval_seconds=30))
        async for request in requests:
            if request.WhichOneof('body') == 'heartbeat':
                yield adapters.CoreToAdapter(heartbeat_ack=adapters.HeartbeatAck(received_at_unix_ms=1))
                continue
            if request.WhichOneof('body') == 'action_result':
                self.results.append(copy.deepcopy(request.action_result))
                continue
            event = request.event
            assert event.source.adapter_instance_id == instance
            self.events.append(copy.deepcopy(event))
            self.received.set()
            if self.mode == 'disconnect' and len(self.events) == 1:
                await context.abort(grpc.StatusCode.UNAVAILABLE, 'secret-upstream-details')
            if self.mode == 'hold':
                await self.release.wait()
            ack = adapters.AdapterEventAck(event_id=event.event_id, trace_id=event.trace_id,
                                            status=adapters.AdapterEventAck.PERSISTED)
            if self.mode == 'reject':
                ack.status = adapters.AdapterEventAck.REJECTED
                ack.error.code = common.INVALID_ARGUMENT
            if self.mode == 'retry' and len(self.events) == 1:
                ack.status = adapters.AdapterEventAck.REJECTED
                ack.error.code, ack.error.retryable = common.RATE_LIMITED, True
                ack.retry_after_ms = 1
            if self.mode == 'wrong-id':
                ack.event_id = 'not-the-event'
            yield adapters.CoreToAdapter(event_ack=ack)
            if self.mode == 'action':
                import time
                from test_actions import request as action_request
                action = action_request(int(time.time()) + 30)
                action.target_source.adapter_instance_id = instance
                yield adapters.CoreToAdapter(action=action)


async def until(predicate):
    async with asyncio.timeout(5):
        while not predicate():
            await asyncio.sleep(0.005)


@pytest.mark.parametrize('mode', ['hold', 'disconnect', 'reject', 'retry', 'wrong-id'])
async def test_confirmation_and_reconnect(tmp_path, caplog, mode):
    core = FakeCore(mode)
    server = grpc.aio.server()
    services.add_AdapterBridgeServicer_to_server(core, server)
    port = server.add_insecure_port('127.0.0.1:0')
    await server.start()
    store = Outbox(tmp_path / 'outbox.db')
    event = map_message(raw(), 'adapter', '123')
    store.enqueue(event)
    config = ClientConfig(f'127.0.0.1:{port}', 'x' * 32, 'adapter', '123', retry_base=0.01, idle_interval=0.01)
    stop = asyncio.Event()
    task = asyncio.create_task(CoreClient(config, store).run(stop))
    try:
        await asyncio.wait_for(core.received.wait(), 5)
        if mode == 'hold':
            assert store.status(event.event_id) == ('pending', 0)
            core.release.set()
        if mode == 'wrong-id':
            await until(lambda: len(core.events) >= 2)
            assert store.status(event.event_id) == ('pending', 0)
        else:
            await until(lambda: store.status(event.event_id)[0] != 'pending')
            assert store.status(event.event_id) == (('failed', common.INVALID_ARGUMENT) if mode == 'reject' else ('acked', 0))
        assert core.registrations[0].capabilities[0].name == 'event.message_received'
        if mode in ('disconnect', 'retry'):
            assert len(core.registrations) >= 2 and len(core.events) == 2
            first, second = core.events
            assert first.source.adapter_instance_id != second.source.adapter_instance_id
            first.source.adapter_instance_id = second.source.adapter_instance_id
            assert first == second
        assert 'secret-upstream-details' not in caplog.text and 'x' * 32 not in caplog.text
    finally:
        stop.set()
        task.cancel()
        try:
            with suppress(asyncio.CancelledError):
                await asyncio.wait_for(task, 5)
        finally:
            await server.stop(0)
            store.close()


def test_invalid_config_and_no_token_repr():
    with pytest.raises(AdapterError):
        ClientConfig('external.example:50051', 'x' * 32, 'adapter', '123')
    config = ClientConfig('127.0.0.1:50051', 'x' * 32, 'adapter', '123')
    assert 'x' * 32 not in repr(config)


async def test_cancelled_transport_is_not_retried(tmp_path):
    class CancelRace(CoreClient):
        async def connection(self, stop):
            try:
                await asyncio.sleep(60)
            except asyncio.CancelledError:
                # gRPC can report transport failure while a cancellation races.
                from kiarabot_adapter_nonebot.client import RetryConnection
                raise RetryConnection()
    store = Outbox(tmp_path / 'cancel.db')
    try:
        task = asyncio.create_task(CancelRace(ClientConfig('127.0.0.1:1234', 'x' * 32, 'adapter', '123'), store).run(asyncio.Event()))
        await asyncio.sleep(.01)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(task, 1)
    finally:
        store.close()
