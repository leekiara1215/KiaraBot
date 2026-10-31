import copy

import pytest
from kiarabot.v1 import common_pb2 as common
from kiarabot_adapter_nonebot.mapper import AdapterError, map_message
from kiarabot_adapter_nonebot.outbox import Outbox


def raw(kind='group', message_id=1):
    return dict(post_type='message', message_type=kind, self_id=123, user_id=456,
                group_id=789, message_id=message_id, time=1700000000,
                message=[{'type': 'text', 'data': {'text': 'hello'}}])


@pytest.mark.parametrize('kind,conversation,ctype', [
    ('group', '789', common.ConversationRef.GROUP),
    ('private', '456', common.ConversationRef.PRIVATE),
])
def test_mapping(kind, conversation, ctype):
    result = map_message(raw(kind), 'adapter', '123')
    assert result.subject.conversation_id == conversation
    assert result.message_received.conversation.id == conversation
    assert result.message_received.conversation.type == ctype
    assert result.message_received.message.plain_text == 'hello'
    assert result.source.account_id == '123' and result.schema_version == 'kiarabot.v1'
    assert result == map_message(copy.deepcopy(raw(kind)), 'adapter', '123')


def test_segments_and_no_platform_objects():
    source = raw()
    source['original_message'] = [
        {'type': 'image', 'data': {'url': 'https://example.test/image', 'secret': 'must-not-leak'}},
        {'type': 'at', 'data': {'qq': 'all'}}, {'type': 'reply', 'data': {'id': '99'}},
        {'type': 'unknown', 'data': {'secret': 'must-not-leak'}},
    ]
    event = map_message(source, 'adapter', '123')
    assert [s.type for s in event.message_received.message.segments] == [
        common.MessageSegment.IMAGE, common.MessageSegment.MENTION,
        common.MessageSegment.REPLY, common.MessageSegment.UNKNOWN]
    assert event.message_received.reply_to_message_id == '99'
    assert list(event.message_received.mentions) == ['all']
    assert b'must-not-leak' not in event.SerializeToString()


@pytest.mark.parametrize('change,code', [
    ({'self_id': 999}, common.PERMISSION_DENIED),
    ({'post_type': 'notice'}, common.CAPABILITY_NOT_SUPPORTED),
    ({'time': True}, common.INVALID_ARGUMENT),
    ({'time': -1}, common.INVALID_ARGUMENT),
    ({'user_id': {}}, common.INVALID_ARGUMENT),
    ({'user_id': 'not-a-number'}, common.INVALID_ARGUMENT),
    ({'group_id': 0}, common.INVALID_ARGUMENT),
    ({'group_id': -789}, common.INVALID_ARGUMENT),
    ({'message': 'unsupported CQ string'}, common.INVALID_ARGUMENT),
    ({'message': [{'type': 'text', 'data': {'text': 1}}]}, common.INVALID_ARGUMENT),
])
def test_refusal(change, code):
    source = raw()
    source.update(change)
    with pytest.raises(AdapterError) as failure:
        map_message(source, 'adapter', '123')
    assert failure.value.code == code


def test_outbox_restart_dedup_and_sequence(tmp_path):
    path = tmp_path / 'outbox.db'
    event = map_message(raw(), 'adapter', '123')
    store = Outbox(path)
    store.enqueue(event)
    assert store.next_pending().sequence == 1
    store.enqueue(event)
    changed = copy.deepcopy(event)
    changed.message_received.message.plain_text = 'different'
    with pytest.raises(AdapterError) as failure:
        store.enqueue(changed)
    assert failure.value.code == common.CONFLICT
    store.close()
    store = Outbox(path)
    assert store.next_pending().event_id == event.event_id
    assert store.status(event.event_id) == ('pending', 0)
    store.finish(event.event_id)
    store.enqueue(event)
    assert store.next_pending() is None
    second = map_message(raw(message_id=2), 'adapter', '123')
    store.enqueue(second)
    assert store.next_pending().sequence == 2
    store.finish(second.event_id, common.INVALID_ARGUMENT)
    assert store.status(second.event_id) == ('failed', common.INVALID_ARGUMENT)
    assert store.next_pending() is None
    store.close()


def test_terminal_records_do_not_consume_pending_capacity(tmp_path):
    store = Outbox(tmp_path / 'outbox.db', limit=1, now=lambda: 100)
    try:
        first = map_message(raw(message_id=1), 'adapter', '123')
        store.enqueue(first)
        store.finish(first.event_id)
        second = map_message(raw(message_id=2), 'adapter', '123')
        store.enqueue(second)
        assert store.next_pending().event_id == second.event_id
        store.finish(second.event_id, common.INVALID_ARGUMENT)
        third = map_message(raw(message_id=3), 'adapter', '123')
        store.enqueue(third)
        assert store.status(third.event_id) == ('pending', 0)
        store.finish(third.event_id)
        store.now = lambda: 8 * 86400
        fourth = map_message(raw(message_id=4), 'adapter', '123')
        store.enqueue(fourth)
        statuses = [row[0] for row in store.db.execute('SELECT status FROM outbox').fetchall()]
        assert 'acked' not in statuses and 'failed' not in statuses
    finally:
        store.close()


def test_capacity_retention_and_atomic_sequence(tmp_path):
    store = Outbox(tmp_path / 'outbox.db', limit=1, now=lambda: 100)
    first, second = (map_message(raw(message_id=i), 'adapter', '123') for i in (1, 2))
    store.enqueue(first)
    with pytest.raises(AdapterError) as failure:
        store.enqueue(second)
    assert failure.value.code == common.RATE_LIMITED
    store.finish(first.event_id)
    store.now = lambda: 8 * 86400
    store.enqueue(second)
    assert store.next_pending().sequence == 2
    store.close()
