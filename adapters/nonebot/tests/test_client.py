"""Target bridge client tests against the TLS FakeTargetRuntime protocol fixture."""
import asyncio
import copy
import grpc
import time
import uuid
from contextlib import asynccontextmanager, suppress

import pytest
from kiarabot.v1 import actions_pb2 as actions, adapters_pb2 as adapters, common_pb2 as common
from kiarabot_adapter_nonebot.actions import ActionExecutor
from kiarabot_adapter_nonebot.client import BridgeClient, RetryConnection
from kiarabot_adapter_nonebot.mapper import map_message
from kiarabot_adapter_nonebot.outbox import Outbox
from kiarabot_adapter_nonebot.status import AdapterStatusReporter
from fake_target_runtime import FakeTargetRuntime, make_bridge_config, until
from test_mapping_outbox import raw


def make_runtime(server_tls, **kwargs):
    return FakeTargetRuntime(str(server_tls[0]), str(server_tls[1]), **kwargs)


def deploy(tmp_path, runtime):
    entry = runtime.add_bootstrap(FakeTargetRuntime.credential())
    config = make_bridge_config(tmp_path / 'deploy', runtime, entry, retry_base=0.01,
                                idle_interval=0.01, timeout=1.0)
    return entry, config


def event_for(config, message_id=1):
    event = map_message(raw(message_id=message_id), 'nonebot', '123')
    event.source.adapter_id = config.adapter_id
    return event


def start(tmp_path, config, events=(), *, executor=None, status=None, on_fatal=None):
    store = Outbox(tmp_path / 'outbox.db')
    queued = []
    for event in events:
        store.enqueue(event)
        queued.append(event)
    client = BridgeClient(config, store, executor=executor, status=status, on_fatal=on_fatal)
    stop = asyncio.Event()
    task = asyncio.create_task(client.run(stop))
    return client, store, task, stop, queued


async def finish(store, task, stop):
    stop.set()
    task.cancel()
    with suppress(asyncio.CancelledError):
        await asyncio.wait_for(task, 5)
    store.close()


async def test_registration_negotiation_and_event_ack(tmp_path, server_tls):
    async with make_runtime(server_tls, heartbeat_interval=5) as runtime:
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: store.status(event.event_id)[0] == 'acked')
            connection = runtime.connections[0]
            registration = connection.registration
            assert registration.adapter_instance_id == entry.instance_id
            assert registration.adapter_id == config.adapter_id
            assert registration.protocol_version == 'kiarabot.v1'
            assert registration.context.request_id and registration.context.trace_id
            assert registration.context.deadline.seconds > 0
            assert registration.accounts[0].account_id == '123' and registration.accounts[0].enabled
            assert [item.name for item in registration.capabilities] == ['event.message_received']
            assert connection.events[0].event_id == event.event_id
            assert connection.events[0].source.adapter_instance_id == entry.instance_id
            assert client.authorized and client.instance == entry.instance_id
            assert client.effective_capabilities == frozenset({'event.message_received'})
            assert client.fatal == 0 and not client.shutdown_seen
        finally:
            await finish(store, task, stop)


async def test_duplicate_ack_releases_outbox(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        entry.accepted_events.add(event.event_id)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: store.status(event.event_id)[0] == 'acked')
            assert runtime.protocol_violations == []
        finally:
            await finish(store, task, stop)


async def test_permanent_rejection_is_recorded_failed(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        runtime.ack_policy = lambda connection, event, index: {
            'status': adapters.AdapterEventAck.REJECTED, 'code': common.INVALID_ARGUMENT}
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: store.status(event.event_id)[0] != 'pending')
            assert store.status(event.event_id) == ('failed', common.INVALID_ARGUMENT)
        finally:
            await finish(store, task, stop)


async def test_backpressure_retryable_reconnects_and_resends_same_event(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        runtime.ack_policy = lambda connection, event, index: (
            {'status': adapters.AdapterEventAck.REJECTED, 'code': common.RATE_LIMITED,
             'retryable': False, 'after_ack': 'close'}
            if len(runtime.connections) == 1 else None)
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: store.status(event.event_id)[0] == 'acked')
            await until(lambda: len(runtime.connections) >= 2)
            first, second = runtime.connections[0], runtime.connections[1]
            assert first.fence.connection_generation == 1
            assert second.fence.connection_generation == 2
            assert first.events[0].event_id == second.events[0].event_id
            assert first.events[0].sequence == second.events[0].sequence
            assert first.events[0].trace_id == second.events[0].trace_id
        finally:
            await finish(store, task, stop)


async def test_retryable_rejection_keeps_pending_and_honours_reconnect(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        runtime.ack_policy = lambda connection, event, index: (
            {'status': adapters.AdapterEventAck.REJECTED, 'code': common.UPSTREAM_UNAVAILABLE,
             'retryable': True, 'retry_after_ms': 50, 'after_ack': 'close'}
            if len(runtime.connections) == 1 else None)
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: len(runtime.connections) >= 2)
            await until(lambda: store.status(event.event_id)[0] == 'acked')
        finally:
            await finish(store, task, stop)


async def test_refresh_rotates_authorization_material(tmp_path, server_tls):
    async with make_runtime(server_tls, authorization_lifetime=1.2, heartbeat_interval=5) as runtime:
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: store.status(event.event_id)[0] == 'acked')
            connection = runtime.connections[0]
            first_proof = connection.proof
            await until(lambda: len(connection.refreshes) >= 1, 5)
            await until(lambda: connection.refresh_generation >= 2)
            assert connection.proof != first_proof
            assert connection.refreshes[0].refresh_generation == 1
            assert connection.refreshes[0].fence == connection.fence.to_proto()
            # Rotation is continuous (short lifetime); wait for one quiescent frame.
            await until(lambda: client.authorization is not None
                        and client.authorization.proof == connection.proof
                        and client.authorization.refresh_generation
                        == connection.refresh_generation)
            assert client.authorized
            assert len(runtime.connections) == 1
            assert runtime.protocol_violations == []
        finally:
            await finish(store, task, stop)


async def test_refresh_conflict_forces_new_connection_generation(tmp_path, server_tls):
    async with make_runtime(server_tls, authorization_lifetime=1.0) as runtime:
        runtime.refresh_policy = lambda connection, request: ('error', common.CONFLICT)
        entry, config = deploy(tmp_path, runtime)
        client, store, task, stop, _ = start(tmp_path, config)
        try:
            await until(lambda: len(runtime.connections) >= 2, 6)
            await until(lambda: client.authorized)
            assert runtime.connections[1].fence.connection_generation == 2
            assert client.fatal == 0
        finally:
            await finish(store, task, stop)


async def test_lost_refresh_response_reconnects(tmp_path, server_tls):
    async with make_runtime(server_tls, authorization_lifetime=1.0) as runtime:
        runtime.refresh_policy = lambda connection, request: 'drop'
        entry, config = deploy(tmp_path, runtime)
        config = copy.copy(config)
        client, store, task, stop, _ = start(tmp_path, config)
        try:
            await until(lambda: len(runtime.connections) >= 2, 8)
            assert client.fatal == 0
        finally:
            await finish(store, task, stop)


async def test_refresh_permission_denied_is_terminal(tmp_path, server_tls):
    async with make_runtime(server_tls, authorization_lifetime=1.0) as runtime:
        runtime.refresh_policy = lambda connection, request: ('error', common.PERMISSION_DENIED)
        entry, config = deploy(tmp_path, runtime)
        fatal_codes = []
        client, store, task, stop, _ = start(tmp_path, config, on_fatal=lambda: fatal_codes.append(1))
        try:
            await asyncio.wait_for(task, 6)
            assert client.fatal == common.PERMISSION_DENIED
            assert fatal_codes == [1]
            assert len(runtime.connections) == 1
        finally:
            stop.set()
            store.close()


async def test_revoked_bootstrap_is_terminal_on_reconnect(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: store.status(event.event_id)[0] == 'acked')
            runtime.revoke(entry)
            await runtime.close_stream()
            await asyncio.wait_for(task, 6)
            assert client.fatal == common.PERMISSION_DENIED
            assert runtime.auth_failures == [grpc.StatusCode.PERMISSION_DENIED]
        finally:
            stop.set()
            store.close()


async def test_rejected_registration_is_terminal(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        runtime.registration_policy = lambda registration: ('reject', common.INVALID_ARGUMENT)
        entry, config = deploy(tmp_path, runtime)
        client, store, task, stop, _ = start(tmp_path, config)
        try:
            await asyncio.wait_for(task, 5)
            assert client.fatal == common.INVALID_ARGUMENT
            assert not client.authorized
        finally:
            stop.set()
            store.close()


async def test_shutdown_notice_stops_new_frames_and_reconnects(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        entry, config = deploy(tmp_path, runtime)
        client, store, task, stop, _ = start(tmp_path, config)
        try:
            await until(lambda: client.authorized)
            await runtime.push_shutdown(drain_timeout_ms=20)
            await until(lambda: client.shutdown_seen)
            await until(lambda: len(runtime.connections) >= 2, 6)
            await until(lambda: client.authorized)
            event = event_for(config)
            store.enqueue(event)
            await until(lambda: store.status(event.event_id)[0] == 'acked')
            assert runtime.connections[0].events == []
        finally:
            await finish(store, task, stop)


async def test_empty_or_unknown_runtime_frame_reconnects_and_keeps_event(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        runtime.ack_policy = lambda connection, event, index: (
            'hold' if len(runtime.connections) == 1 else None)
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: runtime.connections and runtime.connections[0].events)
            await runtime.push_empty_frame()
            await until(lambda: store.status(event.event_id)[0] == 'acked', 6)
            assert len(runtime.connections) >= 2
            assert runtime.protocol_violations == []
        finally:
            await finish(store, task, stop)


async def test_response_fence_mismatch_is_unauthenticated_and_retried(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        runtime.mutate_ack_fence = True
        entry, config = deploy(tmp_path, runtime)
        event = event_for(config)
        client, store, task, stop, _ = start(tmp_path, config, [event])
        try:
            await until(lambda: store.status(event.event_id)[0] == 'acked', 6)
            assert len(runtime.connections) >= 2
            assert runtime.protocol_violations == []
            assert client.fatal == 0
        finally:
            await finish(store, task, stop)


async def test_heartbeat_uses_registration_interval_and_frame_fence(tmp_path, server_tls):
    async with make_runtime(server_tls, heartbeat_interval=1) as runtime:
        entry, config = deploy(tmp_path, runtime)
        client, store, task, stop, _ = start(tmp_path, config)
        try:
            await until(lambda: runtime.connections and len(runtime.connections[0].heartbeats) >= 1,
                        4)
            heartbeat = runtime.connections[0].heartbeats[0]
            assert heartbeat.instance_id == entry.instance_id
            assert heartbeat.sent_at_unix_ms > 0
            assert client.heartbeat_interval == 1.0
        finally:
            await finish(store, task, stop)


async def test_status_event_is_delivered_through_target_frames(tmp_path, server_tls):
    async with make_runtime(server_tls) as runtime:
        entry, config = deploy(tmp_path, runtime)
        store = Outbox(tmp_path / 'status.db')
        status = AdapterStatusReporter(store, config.adapter_id, config.account_id)
        client = BridgeClient(config, store, status=status)
        stop = asyncio.Event()
        task = asyncio.create_task(client.run(stop))
        try:
            await until(lambda: any(event.HasField('adapter_status_changed')
                                    for event in runtime.connections[0].events) if runtime.connections
                        else False)
            status_events = [event.adapter_status_changed
                             for event in runtime.connections[0].events
                             if event.HasField('adapter_status_changed')]
            assert status_events and status_events[0].adapter_id == config.adapter_id
            assert status_events[0].state == common.HEALTHY
            assert status_events[0].reason == 'runtime_connected'
        finally:
            await finish(store, task, stop)


async def test_action_execution_requires_effective_capability(tmp_path, server_tls):
    calls = []

    async def call_platform(api, params):
        calls.append((api, params))
        return {'message_id': 4242}

    executor = None
    async with make_runtime(server_tls) as runtime:
        runtime.effective_capability_filter = lambda names: [
            name for name in names if name != 'action.approve_request.v1']
        entry, config = deploy(tmp_path, runtime)
        executor = ActionExecutor(tmp_path / 'actions.db', config.adapter_id, config.account_id,
                                  call_platform)
        client, store, task, stop, _ = start(tmp_path, config, executor=executor)
        try:
            await until(lambda: runtime.active is not None)
            approve = action_request(config, entry.instance_id, payload='approve_request')
            send = action_request(config, entry.instance_id, payload='send_message')
            await runtime.push_action(approve)
            await runtime.push_action(send)
            await until(lambda: len(runtime.connections[0].action_results) >= 2, 6)
            results = runtime.connections[0].action_results
            assert results[0].status == actions.FAILED
            assert results[0].error.code == common.CAPABILITY_NOT_SUPPORTED
            assert results[1].status == actions.SUCCEEDED
            assert results[1].platform_message_id == '4242'
            assert calls == [('send_group_msg', {'group_id': 456, 'message': [
                {'type': 'text', 'data': {'text': 'hello'}}]})]
        finally:
            executor.close()
            await finish(store, task, stop)


def action_request(config, instance, *, payload='send_message'):
    value = actions.ActionEnvelope(action_id=str(uuid.uuid4()), idempotency_key=str(uuid.uuid4()),
                                   trace_id=str(uuid.uuid4()))
    value.deadline.seconds = int(time.time()) + 30
    value.target_source.adapter_id = config.adapter_id
    value.target_source.account_id = config.account_id
    value.target_source.adapter_instance_id = instance
    value.target.conversation_id = '456'
    if payload == 'approve_request':
        value.approve_request.request_id = 'request-token'
        value.approve_request.approve = True
    else:
        value.send_message.conversation.id = '456'
        value.send_message.conversation.account_id = config.account_id
        value.send_message.conversation.type = common.ConversationRef.GROUP
        value.send_message.message.plain_text = 'hello'
    return value


async def test_no_secret_material_in_logs(tmp_path, server_tls, caplog):
    async with make_runtime(server_tls, authorization_lifetime=1.0) as runtime:
        entry, config = deploy(tmp_path, runtime)
        runtime.refresh_policy = lambda connection, request: ('error', common.CONFLICT)
        client, store, task, stop, _ = start(tmp_path, config)
        try:
            await until(lambda: len(runtime.connections) >= 2, 6)
            secrets = [entry.credential]
            for connection in runtime.connections:
                secrets.extend([connection.proof.hex(), connection.refresh_credential.hex()])
            text = caplog.text
            for secret in secrets:
                assert secret not in text
                assert secret not in repr(config)
        finally:
            await finish(store, task, stop)


async def test_cancelled_transport_is_not_retried(tmp_path, server_tls):
    class CancelRace(BridgeClient):
        async def connection(self, stop):
            try:
                await asyncio.sleep(60)
            except asyncio.CancelledError:
                raise RetryConnection()

    async with make_runtime(server_tls) as runtime:
        entry, config = deploy(tmp_path, runtime)
        store = Outbox(tmp_path / 'cancel.db')
        try:
            task = asyncio.create_task(CancelRace(config, store).run(asyncio.Event()))
            await asyncio.sleep(0.02)
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await asyncio.wait_for(task, 1)
        finally:
            store.close()
