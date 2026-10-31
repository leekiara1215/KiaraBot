import asyncio
import pytest
from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common
from kiarabot_adapter_nonebot.actions import ActionExecutor
from kiarabot_adapter_nonebot.mapper import AdapterError, map_message
from test_mapping_outbox import raw


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
    ('payload', common.CAPABILITY_NOT_SUPPORTED), ('invalid', common.INVALID_ARGUMENT),
    ('prefixed', common.INVALID_ARGUMENT)])
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
        if mode == 'prefixed':
            action.send_message.conversation.id = 'group:789'
            action.target.conversation_id = 'group:789'
        result = await executor.execute(action, 'instance')
        assert result.error.code == code
        assert result.status == (actions.EXPIRED if mode == 'deadline' else actions.FAILED)
    finally:
        executor.close()


@pytest.mark.parametrize('kind,api,field,number', [
    ('group', 'send_group_msg', 'group_id', 789),
    ('private', 'send_private_msg', 'user_id', 456),
])
async def test_event_conversation_roundtrip(tmp_path, kind, api, field, number):
    """The inbound MessageReceived conversation is directly usable for SendMessage."""
    event = map_message(raw(kind), 'adapter', '123')
    action = actions.ActionEnvelope(action_id='action-' + kind, idempotency_key='key-' + kind,
                                    trace_id=event.trace_id)
    action.deadline.seconds = 2000
    action.target_source.CopyFrom(event.source)
    action.target_source.adapter_instance_id = 'instance'
    action.target.CopyFrom(event.subject)
    action.send_message.conversation.CopyFrom(event.message_received.conversation)
    action.send_message.message.plain_text = 'hello back'
    calls = []
    async def call(api_name, params):
        calls.append((api_name, params))
        return {'message_id': 99}
    executor = ActionExecutor(tmp_path / (kind + '.db'), 'adapter', '123', call, now=lambda: 1000)
    try:
        result = await executor.execute(action, 'instance')
    finally:
        executor.close()
    assert result.status == actions.SUCCEEDED
    assert calls == [(api, {field: number,
                            'message': [{'type': 'text', 'data': {'text': 'hello back'}}]})]


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


def react_request(deadline=2000, message_id='789', reaction='like',
                  kind=common.ConversationRef.GROUP, conversation='456'):
    value = actions.ActionEnvelope(action_id='react', idempotency_key='react-key', trace_id='trace')
    value.deadline.seconds = deadline
    value.target_source.adapter_id = 'adapter'
    value.target_source.account_id = '123'
    value.target_source.adapter_instance_id = 'instance'
    value.target.conversation_id = conversation
    value.react_message.message_id = message_id
    value.react_message.reaction = reaction
    value.react_message.conversation.id = conversation
    value.react_message.conversation.account_id = '123'
    value.react_message.conversation.type = kind
    return value


async def test_react_executes_extension_api_and_is_idempotent(tmp_path):
    calls = []
    async def call(api, params):
        calls.append((api, params))
        # Reaction APIs report success without a platform message id.
        return {'result': 0}
    executor = ActionExecutor(tmp_path / 'react.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        result = await executor.execute(react_request(), 'instance')
        assert result.status == actions.SUCCEEDED and result.platform_message_id == ''
        again = await executor.execute(react_request(), 'instance')
        assert again.status == actions.SUCCEEDED
    finally:
        executor.close()
    assert calls == [('set_msg_emoji_like', {'message_id': 789, 'emoji_id': '128077'})]


@pytest.mark.parametrize('mode,code', [
    ('reaction', common.CAPABILITY_NOT_SUPPORTED),
    ('channel', common.CAPABILITY_NOT_SUPPORTED),
    ('message', common.INVALID_ARGUMENT),
    ('account', common.PERMISSION_DENIED),
])
async def test_react_rejections_do_not_call_platform(tmp_path, mode, code):
    async def call(api, params):
        pytest.fail('platform must not be called')
    value = react_request()
    if mode == 'reaction':
        value.react_message.reaction = 'party'
    if mode == 'channel':
        value.react_message.conversation.type = common.ConversationRef.CHANNEL
    if mode == 'message':
        value.react_message.message_id = 'not-numeric'
    if mode == 'account':
        value.target_source.account_id = '456'
    executor = ActionExecutor(tmp_path / 'react.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        result = await executor.execute(value, 'instance')
    finally:
        executor.close()
    assert result.status == actions.FAILED and result.error.code == code


def delete_request(deadline=2000, message_id='12345',
                   kind=common.ConversationRef.GROUP, conversation='456', action_id='delete'):
    value = actions.ActionEnvelope(action_id=action_id, idempotency_key=action_id + '-key',
                                   trace_id='trace')
    value.deadline.seconds = deadline
    value.target_source.adapter_id = 'adapter'
    value.target_source.account_id = '123'
    value.target_source.adapter_instance_id = 'instance'
    value.target.conversation_id = conversation
    value.delete_message.message_id = message_id
    value.delete_message.conversation.id = conversation
    value.delete_message.conversation.account_id = '123'
    value.delete_message.conversation.type = kind
    return value


async def test_delete_only_recalls_messages_sent_by_this_bot(tmp_path):
    calls = []
    async def call(api, params):
        calls.append((api, params))
        return {'message_id': 12345} if api == 'send_group_msg' else {'result': 0}
    executor = ActionExecutor(tmp_path / 'delete.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        sent = await executor.execute(request(), 'instance')
        assert sent.status == actions.SUCCEEDED
        # A message the adapter knows it sent can be recalled.
        deleted = await executor.execute(delete_request(), 'instance')
        assert deleted.status == actions.SUCCEEDED and deleted.platform_message_id == ''
        # Unknown message IDs are never accepted as recall targets.
        unknown = await executor.execute(delete_request(message_id='99999', action_id='unknown'),
                                         'instance')
        assert unknown.status == actions.FAILED and unknown.error.code == common.PERMISSION_DENIED
        # The same ID in another conversation is not authorized either.
        other = await executor.execute(delete_request(conversation='789', action_id='other'),
                                       'instance')
        assert other.status == actions.FAILED and other.error.code == common.PERMISSION_DENIED
    finally:
        executor.close()
    assert calls == [
        ('send_group_msg', {'group_id': 456, 'message': [
            {'type': 'text', 'data': {'text': '[CQ:image,file=not-an-image]'}}]}),
        ('delete_msg', {'message_id': 12345}),
    ]


@pytest.mark.parametrize('mode,code', [
    ('channel', common.CAPABILITY_NOT_SUPPORTED),
    ('message', common.INVALID_ARGUMENT),
    ('account', common.PERMISSION_DENIED),
])
async def test_delete_rejections_do_not_call_platform(tmp_path, mode, code):
    async def call(api, params):
        pytest.fail('platform must not be called')
    value = delete_request()
    if mode == 'channel':
        value.delete_message.conversation.type = common.ConversationRef.CHANNEL
    if mode == 'message':
        value.delete_message.message_id = 'not-numeric'
    if mode == 'account':
        value.target_source.account_id = '456'
    executor = ActionExecutor(tmp_path / 'delete.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        result = await executor.execute(value, 'instance')
    finally:
        executor.close()
    assert result.status == actions.FAILED and result.error.code == code


def mute_request(duration=600, group_id='456', user_id='789', action_id='mute'):
    value = actions.ActionEnvelope(action_id=action_id, idempotency_key=action_id + '-key',
                                   trace_id='trace')
    value.deadline.seconds = 2000
    value.target_source.adapter_id = 'adapter'
    value.target_source.account_id = '123'
    value.target_source.adapter_instance_id = 'instance'
    value.target.conversation_id = group_id
    value.mute_member.group_id = group_id
    value.mute_member.user_id = user_id
    value.mute_member.duration_seconds = duration
    return value


def kick_request(group_id='456', user_id='789', reason='spam', action_id='kick'):
    value = actions.ActionEnvelope(action_id=action_id, idempotency_key=action_id + '-key',
                                   trace_id='trace')
    value.deadline.seconds = 2000
    value.target_source.adapter_id = 'adapter'
    value.target_source.account_id = '123'
    value.target_source.adapter_instance_id = 'instance'
    value.target.conversation_id = group_id
    value.kick_member.group_id = group_id
    value.kick_member.user_id = user_id
    value.kick_member.reason = reason
    return value


async def test_mute_and_kick_map_to_onebot_management_apis(tmp_path):
    calls = []
    async def call(api, params):
        calls.append((api, params))
        return {'result': 0}
    executor = ActionExecutor(tmp_path / 'moderation.db', 'adapter', '123', call,
                              now=lambda: 1000)
    try:
        muted = await executor.execute(mute_request(), 'instance')
        assert muted.status == actions.SUCCEEDED and muted.platform_message_id == ''
        kicked = await executor.execute(kick_request(), 'instance')
        assert kicked.status == actions.SUCCEEDED and kicked.platform_message_id == ''
        # Replays are answered from the action log and do not call the platform.
        await executor.execute(mute_request(), 'instance')
    finally:
        executor.close()
    assert calls == [
        ('set_group_ban', {'group_id': 456, 'user_id': 789, 'duration': 600}),
        ('set_group_kick', {'group_id': 456, 'user_id': 789, 'reject_add_request': False}),
    ]


@pytest.mark.parametrize('mode,code', [
    ('duration', common.INVALID_ARGUMENT),
    ('user', common.INVALID_ARGUMENT),
    ('target', common.INVALID_ARGUMENT),
])
async def test_mute_rejections_do_not_call_platform(tmp_path, mode, code):
    async def call(api, params):
        pytest.fail('platform must not be called')
    value = mute_request()
    if mode == 'duration':
        value.mute_member.duration_seconds = 0
    if mode == 'user':
        value.mute_member.user_id = 'not-numeric'
    if mode == 'target':
        value.target.conversation_id = '999'
    executor = ActionExecutor(tmp_path / 'mute.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        result = await executor.execute(value, 'instance')
    finally:
        executor.close()
    assert result.status == actions.FAILED and result.error.code == code


async def test_kick_rejections_do_not_call_platform(tmp_path):
    async def call(api, params):
        pytest.fail('platform must not be called')
    value = kick_request()
    value.kick_member.group_id = 'not-numeric'
    executor = ActionExecutor(tmp_path / 'kick.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        result = await executor.execute(value, 'instance')
    finally:
        executor.close()
    assert result.status == actions.FAILED and result.error.code == common.INVALID_ARGUMENT


def approve_request(action_id='approve', request_id='req-1', approve=True, reason='ok'):
    value = actions.ActionEnvelope(action_id=action_id, idempotency_key=action_id + '-key',
                                   trace_id='trace')
    value.deadline.seconds = 2000
    value.target_source.adapter_id = 'adapter'
    value.target_source.account_id = '123'
    value.target_source.adapter_instance_id = 'instance'
    value.approve_request.request_id = request_id
    value.approve_request.approve = approve
    value.approve_request.reason = reason
    return value


async def test_approve_request_resolves_stored_flag(tmp_path):
    calls = []
    async def call(api, params):
        calls.append((api, params))
        return {'result': 0}
    executor = ActionExecutor(tmp_path / 'approve.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        executor.remember_request('req-1', 'flag-1')
        accepted = await executor.execute(approve_request(), 'instance')
        assert accepted.status == actions.SUCCEEDED and accepted.platform_message_id == ''
        rejected = await executor.execute(approve_request(action_id='reject', request_id='req-1',
                                                          approve=False, reason='no'),
                                          'instance')
        assert rejected.status == actions.SUCCEEDED
        # Replays come from the action log.
        await executor.execute(approve_request(), 'instance')
        # Unknown request tokens never reach the platform.
        unknown = await executor.execute(approve_request(action_id='unknown',
                                                          request_id='missing'), 'instance')
        assert unknown.status == actions.FAILED and unknown.error.code == common.NOT_FOUND
    finally:
        executor.close()
    assert calls == [
        ('set_friend_add_request', {'flag': 'flag-1', 'approve': True}),
        ('set_friend_add_request', {'flag': 'flag-1', 'approve': False}),
    ]
