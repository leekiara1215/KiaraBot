import asyncio
import json

from kiarabot.v1 import common_pb2 as common
from kiarabot_adapter_nonebot.health import HealthServer
from kiarabot_adapter_nonebot.outbox import Outbox
from kiarabot_adapter_nonebot.status import AdapterStatusReporter


async def get(port, path):
    reader, writer = await asyncio.open_connection('127.0.0.1', port)
    try:
        writer.write(f'GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode())
        await writer.drain()
        data = await asyncio.wait_for(reader.read(), 5)
    finally:
        writer.close()
        await writer.wait_closed()
    head, _, body = data.partition(b'\r\n\r\n')
    return int(head.split()[1]), json.loads(body)


async def test_health_live_is_process_only_and_ready_follows_state():
    snapshot = {'ready': False, 'components': {'core': 'UNHEALTHY', 'platform': 'DEGRADED'}}
    server = await HealthServer(lambda: snapshot, port=0).start()
    try:
        assert server.bound_port != 0
        assert (await get(server.bound_port, '/health/live')) == (200, {'state': 'HEALTHY'})
        assert (await get(server.bound_port, '/health/ready'))[0] == 503
        snapshot['ready'] = True
        snapshot['components'] = {'core': 'HEALTHY', 'platform': 'HEALTHY'}
        status, body = await get(server.bound_port, '/health/ready')
        assert status == 200 and body['components']['platform'] == 'HEALTHY'
        assert (await get(server.bound_port, '/unknown'))[0] == 404
    finally:
        await server.close()


async def test_status_reporter_emits_transitions_only(tmp_path):
    store = Outbox(tmp_path / 'outbox.db', now=lambda: 100)
    try:
        reporter = AdapterStatusReporter(store, 'adapter', '123', now=lambda: 1700000000)
        assert reporter.report(common.HEALTHY, 'core_connected')
        assert reporter.report(common.HEALTHY, 'core_connected') is None
        assert reporter.report(common.DEGRADED, 'platform_disconnected')
        events = []
        while (event := store.next_pending()) is not None:
            events.append(event)
            store.finish(event.event_id)
        assert len(events) == 2
        first, second = events
        assert first.adapter_status_changed.adapter_id == 'adapter'
        assert first.adapter_status_changed.state == common.HEALTHY
        assert first.source.account_id == '123' and first.schema_version == 'kiarabot.v1'
        assert first.occurred_at.seconds == 1700000000
        assert second.adapter_status_changed.state == common.DEGRADED
        assert second.adapter_status_changed.reason == 'platform_disconnected'
        assert [event.sequence for event in events] == [1, 2]
    finally:
        store.close()
