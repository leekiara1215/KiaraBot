import copy
import json
import sqlite3

import pytest
from kiarabot.v1 import common_pb2 as common, events_pb2 as events
from kiarabot_adapter_nonebot.mapper import AdapterError, map_message, map_request
from kiarabot_adapter_nonebot.outbox import Outbox
from kiarabot_adapter_nonebot.status import AdapterStatusReporter


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


def friend_request_raw():
    return dict(post_type='request', request_type='friend', self_id=123, user_id=456,
                comment='hello', flag='flag-1', time=1700000000)


def test_map_friend_request_uses_safe_request_token():
    result = map_request(friend_request_raw(), 'adapter', '123')
    assert result.WhichOneof('payload') == 'friend_request'
    assert result.friend_request.user_id == '456'
    assert result.friend_request.comment == 'hello'
    # The plugin sees a deterministic adapter-scoped token, never the raw flag.
    assert result.friend_request.request_id == result.event_id
    assert 'flag-1' not in result.friend_request.request_id
    assert result.subject.user_id == '456' and result.subject.conversation_id == ''
    assert result == map_request(copy.deepcopy(friend_request_raw()), 'adapter', '123')


@pytest.mark.parametrize('mode,code', [
    ('group', common.CAPABILITY_NOT_SUPPORTED),
    ('self', common.PERMISSION_DENIED),
    ('time', common.INVALID_ARGUMENT),
    ('comment', common.INVALID_ARGUMENT),
])
def test_map_friend_request_rejections(mode, code):
    value = friend_request_raw()
    if mode == 'group':
        value['request_type'] = 'group'
    if mode == 'self':
        value['self_id'] = 999
    if mode == 'time':
        value['time'] = True
    if mode == 'comment':
        value['comment'] = 'x' * 300
    with pytest.raises(AdapterError) as failure:
        map_request(value, 'adapter', '123')
    assert failure.value.code == code


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


V1_SCHEMA = (
    'CREATE TABLE sessions (key TEXT PRIMARY KEY, sequence INTEGER NOT NULL)',
    'CREATE TABLE outbox (position INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE, '
    "fingerprint TEXT NOT NULL, payload BLOB NOT NULL, status TEXT NOT NULL DEFAULT 'pending', "
    'error INTEGER NOT NULL DEFAULT 0, updated REAL NOT NULL)',
)


def conversation_event(kind, number, message_id=1):
    source = raw(kind, message_id)
    source['user_id'] = number
    source['group_id'] = number
    return map_message(source, 'adapter', '123')


def event_sequence(store, event_id):
    payload = store.db.execute('SELECT payload FROM outbox WHERE id=?', (event_id,)).fetchone()[0]
    return events.EventEnvelope.FromString(payload).sequence


def session_rows(store):
    return dict(store.db.execute('SELECT key, sequence FROM sessions').fetchall())


def write_v1_db(path):
    pending = map_message(raw(message_id=9), 'adapter', '123')
    db = sqlite3.connect(path)
    try:
        for statement in V1_SCHEMA:
            db.execute(statement)
        db.execute('INSERT INTO sessions VALUES(?,?)', (json.dumps(['adapter', '123', '42']), 7))
        db.execute('INSERT INTO sessions VALUES(?,?)', (json.dumps(['adapter', '123', '']), 3))
        db.execute('INSERT INTO outbox(id,fingerprint,payload,updated) VALUES(?,?,?,?)',
                   (pending.event_id, 'f' * 64, pending.SerializeToString(), 100.0))
        db.execute('PRAGMA user_version=1')
        db.commit()
    finally:
        db.close()
    return pending.event_id


def test_conversation_type_separates_group_and_private_sequence(tmp_path):
    store = Outbox(tmp_path / 'outbox.db')
    try:
        first = conversation_event('group', 42, message_id=1)
        second = conversation_event('private', 42, message_id=2)
        third = conversation_event('group', 42, message_id=3)
        fourth = conversation_event('private', 42, message_id=4)
        for event in (first, second, third, fourth):
            store.enqueue(event)
        assert [event_sequence(store, event.event_id) for event in (first, second, third, fourth)] == [1, 1, 2, 2]
        status_id = AdapterStatusReporter(store, 'adapter', '123').report(common.HEALTHY)
        assert event_sequence(store, status_id) == 1
        group_key = json.dumps(['adapter', '123', '42', common.ConversationRef.GROUP])
        private_key = json.dumps(['adapter', '123', '42', common.ConversationRef.PRIVATE])
        status_key = json.dumps(['adapter', '123', '', common.ConversationRef.TYPE_UNSPECIFIED])
        assert len({group_key, private_key, status_key}) == 3
        assert session_rows(store) == {group_key: 2, private_key: 2, status_key: 1}
    finally:
        store.close()


def test_v1_outbox_migrates_session_keys_to_v2(tmp_path):
    path = tmp_path / 'outbox.db'
    pending_id = write_v1_db(path)
    store = Outbox(path)
    try:
        assert store.db.execute('PRAGMA user_version').fetchone()[0] == 2
        assert session_rows(store) == {
            json.dumps(['adapter', '123', '42', common.ConversationRef.GROUP]): 7,
            json.dumps(['adapter', '123', '42', common.ConversationRef.PRIVATE]): 7,
            json.dumps(['adapter', '123', '', common.ConversationRef.TYPE_UNSPECIFIED]): 3,
        }
        assert store.status(pending_id) == ('pending', 0)
        assert store.db.execute('SELECT updated FROM outbox WHERE id=?', (pending_id,)).fetchone()[0] == 100.0
        group = conversation_event('group', 42, message_id=10)
        private = conversation_event('private', 42, message_id=11)
        store.enqueue(group)
        store.enqueue(private)
        assert event_sequence(store, group.event_id) == 8
        assert event_sequence(store, private.event_id) == 8
        status_id = AdapterStatusReporter(store, 'adapter', '123').report(common.DEGRADED)
        assert event_sequence(store, status_id) == 4
    finally:
        store.close()


def test_unknown_outbox_version_fails_closed(tmp_path):
    path = tmp_path / 'outbox.db'
    db = sqlite3.connect(path)
    try:
        db.execute('PRAGMA user_version=3')
        db.commit()
    finally:
        db.close()
    with pytest.raises(AdapterError) as failure:
        Outbox(path)
    assert failure.value.code == common.INTERNAL


def test_malformed_v1_session_key_fails_closed_without_partial_write(tmp_path):
    path = tmp_path / 'outbox.db'
    db = sqlite3.connect(path)
    try:
        for statement in V1_SCHEMA:
            db.execute(statement)
        db.execute('INSERT INTO sessions VALUES(?,?)', (json.dumps(['adapter', '123', '42']), 7))
        db.execute('INSERT INTO sessions VALUES(?,?)', (json.dumps(['adapter', '123']), 9))
        db.execute('PRAGMA user_version=1')
        db.commit()
    finally:
        db.close()
    with pytest.raises(AdapterError) as failure:
        Outbox(path)
    assert failure.value.code == common.INTERNAL
    db = sqlite3.connect(path)
    try:
        assert db.execute('PRAGMA user_version').fetchone()[0] == 1
        assert db.execute('SELECT count(*) FROM sessions').fetchone()[0] == 2
    finally:
        db.close()
