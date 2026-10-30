import asyncio
from contextlib import suppress
import pytest
from kiarabot.v1 import actions_pb2 as actions, plugins_pb2 as plugins, events_pb2 as events, common_pb2 as common
from kiarabot_sdk import Plugin, PluginError, Runner
from kiarabot_sdk.testing import FakeHost
from test_runtime import manifest, options, get


async def ready(runner):
    async with asyncio.timeout(2):
        while not runner.context._rpc:
            await asyncio.sleep(.01)


async def test_action_in_event_callback_and_out_of_order_responses():
    class Sender(Plugin):
        async def on_event(self, event, context):
            result = await context.submit_action(actions.ActionEnvelope(action_id='sent'))
            assert result.status == actions.ACCEPTED
    async with FakeHost() as host:
        host.features = ['action.result.v1']
        stop = asyncio.Event()
        runner = Runner(manifest(), Sender(), options(host))
        task = asyncio.create_task(runner.run(stop))
        pending = []
        try:
            await ready(runner)
            await host.outgoing.put(plugins.CoreToPlugin(event=events.EventEnvelope(event_id='event')))
            request = (await get(host.actions)).action
            await host.outgoing.put(plugins.CoreToPlugin(action_response=plugins.PluginActionResponse(
                request_id=request.request_id, result=actions.ActionResult(action_id='sent', status=actions.ACCEPTED))))
            assert (await get(host.acknowledgements)).accepted
            pending = [asyncio.create_task(runner.context.query_action(action_id='first')),
                       asyncio.create_task(runner.context.query_action(idempotency_key='second'))]
            requests = [(await get(host.actions)).action_query for _ in range(2)]
            for query in reversed(requests):
                key = query.action_id or query.idempotency_key
                await host.outgoing.put(plugins.CoreToPlugin(action_response=plugins.PluginActionResponse(
                    request_id=query.request_id, result=actions.ActionResult(action_id=key, status=actions.SUCCEEDED, completed_at={'seconds': 1}))))
            assert [value.action_id for value in await asyncio.gather(*pending)] == ['first', 'second']
        finally:
            for item in pending:
                item.cancel()
            stop.set()
            await asyncio.wait_for(task, 2)


@pytest.mark.parametrize('mode,code', [('legacy', common.CAPABILITY_NOT_SUPPORTED), ('timeout', common.TIMEOUT),
    ('disconnect', common.UPSTREAM_UNAVAILABLE), ('unknown', common.UPSTREAM_UNAVAILABLE), ('denied', common.PERMISSION_DENIED)])
async def test_unknown_results_never_become_failed(mode, code):
    async with FakeHost() as host:
        if mode != 'legacy':
            host.features = ['action.result.v1']
        stop = asyncio.Event()
        runner = Runner(manifest(), Plugin(), options(host))
        task = asyncio.create_task(runner.run(stop))
        try:
            await ready(runner)
            request_task = asyncio.create_task(runner.context.query_action(action_id='action', timeout=.1))
            if mode != 'legacy':
                request = (await get(host.actions)).action_query
                if mode == 'disconnect':
                    await host.outgoing.put(None)
                elif mode == 'unknown':
                    await host.outgoing.put(plugins.CoreToPlugin(action_response=plugins.PluginActionResponse(
                        request_id=request.request_id, result=actions.ActionResult(status=999))))
                elif mode == 'denied':
                    await host.outgoing.put(plugins.CoreToPlugin(action_response=plugins.PluginActionResponse(
                        request_id=request.request_id, error=common.Error(code=common.PERMISSION_DENIED))))
            with pytest.raises(PluginError) as failure:
                await request_task
            assert failure.value.code == code
            if mode == 'legacy':
                assert host.actions.empty()
        finally:
            stop.set()
            await asyncio.wait_for(task, 2)
