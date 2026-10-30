import asyncio
import pytest
from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common
from kiarabot_adapter_nonebot.actions import ActionExecutor
from kiarabot_adapter_nonebot.mapper import AdapterError


def request(deadline=2000):
    value = actions.ActionEnvelope(action_id='action', idempotency_key='key', trace_id='trace')
    value.deadline.seconds = deadline
    value.target_source.adapter_id = 'adapter'
    value.target_source.account_id = '123'
    value.target_source.adapter_instance_id = 'instance'
    value.target.conversation_id = '456'
    value.send_message.conversation.id = '456'
    value.send_message.conversation.account_id = '123'
    value.send_message.conversation.type = common.ConversationRef.GROUP
    value.send_message.message.plain_text = '[CQ:image,file=not-an-image]'
    return value


async def test_execute_once_restart_duplicate_and_conflict(tmp_path):
    calls = []
    async def call(api, params):
        calls.append((api, params))
        return {'message_id': 12345}
    for _ in range(2):
        executor = ActionExecutor(tmp_path / 'actions.db', 'adapter', '123', call, now=lambda: 1000)
        try:
            result = await executor.execute(request(), 'instance')
            assert result.status == actions.SUCCEEDED and result.platform_message_id == '12345'
            changed = request()
            changed.send_message.message.plain_text = 'different'
            assert (await executor.execute(changed, 'instance')).error.code == common.CONFLICT
        finally:
            executor.close()
    assert len(calls) == 1
    assert calls[0][0] == 'send_group_msg'
    assert calls[0][1]['message'] == [{'type': 'text', 'data': {'text': '[CQ:image,file=not-an-image]'}}]


@pytest.mark.parametrize('mode', ['timeout', 'transport', 'cancel', 'rejection'])
async def test_unknown_never_reexecutes_or_becomes_failed(tmp_path, mode):
    calls = []
    async def call(api, params):
        calls.append(api)
        if mode == 'transport':
            raise ConnectionError('private body')
        if mode == 'rejection':
            raise AdapterError(common.UPSTREAM_UNAVAILABLE)
        if mode == 'cancel':
            raise asyncio.CancelledError()
        await asyncio.sleep(60)
    executor = ActionExecutor(tmp_path / 'actions.db', 'adapter', '123', call, now=lambda: 1000, timeout=.01)
    try:
        if mode == 'cancel':
            with pytest.raises(asyncio.CancelledError):
                await executor.execute(request(), 'instance')
        else:
            result = await executor.execute(request(), 'instance')
            assert result.status == (actions.FAILED if mode == 'rejection' else actions.ACCEPTED)
        again = await executor.execute(request(), 'instance')
        assert again.status == (actions.FAILED if mode == 'rejection' else actions.ACCEPTED)
        assert len(calls) == 1
    finally:
        executor.close()


@pytest.mark.parametrize('mode,code', [('deadline', common.TIMEOUT), ('owner', common.PERMISSION_DENIED),
    ('instance', common.PERMISSION_DENIED), ('segment', common.CAPABILITY_NOT_SUPPORTED),
    ('payload', common.CAPABILITY_NOT_SUPPORTED), ('invalid', common.INVALID_ARGUMENT)])
async def test_rejection_has_no_platform_side_effect(tmp_path, mode, code):
    async def call(api, params):
        pytest.fail('platform must not be called')
    executor = ActionExecutor(tmp_path / 'actions.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        action = request(999 if mode == 'deadline' else 2000)
        if mode == 'owner':
            action.target_source.account_id = 'other'
        if mode == 'instance':
            action.target_source.adapter_instance_id = 'old'
        if mode == 'segment':
            action.send_message.message.segments.add(type=common.MessageSegment.IMAGE)
        if mode == 'payload':
            action.delete_message.message_id = 'message'
        if mode == 'invalid':
            action.send_message.conversation.id = '-1'
        result = await executor.execute(action, 'instance')
        assert result.error.code == code
        assert result.status == (actions.EXPIRED if mode == 'deadline' else actions.FAILED)
    finally:
        executor.close()


async def test_capacity_and_database_failure_prevent_platform(tmp_path):
    async def call(api, params):
        pytest.fail('platform must not be called')
    executor = ActionExecutor(tmp_path / 'actions.db', 'adapter', '123', call, now=lambda: 1000, limit=0)
    try:
        with pytest.raises(AdapterError) as failure:
            await executor.execute(request(), 'instance')
        assert failure.value.code == common.RATE_LIMITED
        executor.db.execute('PRAGMA query_only=ON')
        with pytest.raises(AdapterError) as failure:
            await executor.execute(request(), 'instance')
        assert failure.value.code == common.UPSTREAM_UNAVAILABLE
    finally:
        executor.close()
