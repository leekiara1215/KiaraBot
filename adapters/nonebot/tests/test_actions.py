import asyncio
import sqlite3
import uuid

import pytest
from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common, platform_pb2 as platform
from kiarabot_adapter_nonebot.actions import ActionExecutor, terminal
from kiarabot_adapter_nonebot.mapper import AdapterError, map_message
from test_mapping_outbox import raw


def dispatch(value, attempt=None, sequence=1):
    """Runtime-filled P07 dispatch identity (new attempt per dispatch is the normal case)."""
    value.dispatch_attempt_id = attempt or str(uuid.uuid4())
    value.dispatch_sequence = sequence
    return value


def request(deadline=2000):
    value = dispatch(actions.ActionEnvelope(action_id='action', idempotency_key='key',
                                            trace_id='trace'))
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
            result = (await executor.execute(request(), 'instance')).result
            assert result.status == actions.SUCCEEDED and result.platform_message_id == '12345'
            changed = request()
            changed.send_message.message.plain_text = 'different'
            assert (await executor.execute(changed, 'instance')).result.error.code == common.CONFLICT
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

    executor = ActionExecutor(tmp_path / 'actions.db', 'adapter', '123', call,
                              now=lambda: 1000, timeout=.01)
    try:
        first = request()
        first_attempt = first.dispatch_attempt_id
        if mode == 'cancel':
            with pytest.raises(asyncio.CancelledError):
                await executor.execute(first, 'instance')
            pending = executor.pending_reports()
            assert [(item.dispatch_attempt_id, item.evidence.execution_outcome) for item in pending]                 == [(first_attempt, platform.EXECUTION_UNKNOWN)]
        else:
            report = await executor.execute(first, 'instance')
            if mode == 'rejection':
                # An explicit platform rejection is the only known failure.
                assert report.kind == 'result'
                assert report.result.status == actions.FAILED
                assert report.result.execution_outcome == platform.EXECUTION_KNOWN_FAILED
            else:
                assert report.kind == 'evidence'
                assert report.evidence.execution_outcome == platform.EXECUTION_UNKNOWN
        again = await executor.execute(request(), 'instance')
        if mode == 'rejection':
            assert again.kind == 'result' and again.result.status == actions.FAILED
        else:
            # A new dispatch attempt reports the same unknown fact and never replays the effect.
            assert again.kind == 'evidence'
            assert again.evidence.execution_outcome == platform.EXECUTION_UNKNOWN
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
        report = await executor.execute(action, 'instance')
        if mode == 'deadline':
            # The adapter never self-reports EXPIRED: a not-started attempt is evidence only.
            assert report.kind == 'evidence'
            assert report.evidence.execution_outcome == platform.EXECUTION_NOT_STARTED
            assert report.evidence.last_error.code == common.TIMEOUT
        else:
            assert report.kind == 'result'
            assert report.result.error.code == code
            assert report.result.status == actions.FAILED
            assert report.result.execution_outcome == platform.EXECUTION_KNOWN_FAILED
    finally:
        executor.close()


@pytest.mark.parametrize('kind,api,field,number', [
    ('group', 'send_group_msg', 'group_id', 789),
    ('private', 'send_private_msg', 'user_id', 456),
])
async def test_event_conversation_roundtrip(tmp_path, kind, api, field, number):
    """The inbound MessageReceived conversation is directly usable for SendMessage."""
    event = map_message(raw(kind), 'adapter', '123')
    action = dispatch(actions.ActionEnvelope(action_id='action-' + kind,
                                             idempotency_key='key-' + kind,
                                             trace_id=event.trace_id))
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
        result = (await executor.execute(action, 'instance')).result
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
    value = dispatch(actions.ActionEnvelope(action_id='react', idempotency_key='react-key',
                                            trace_id='trace'))
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
        result = (await executor.execute(react_request(), 'instance')).result
        assert result.status == actions.SUCCEEDED and result.platform_message_id == ''
        again = (await executor.execute(react_request(), 'instance')).result
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
        result = (await executor.execute(value, 'instance')).result
    finally:
        executor.close()
    assert result.status == actions.FAILED and result.error.code == code


def delete_request(deadline=2000, message_id='12345',
                   kind=common.ConversationRef.GROUP, conversation='456', action_id='delete'):
    value = dispatch(actions.ActionEnvelope(action_id=action_id, idempotency_key=action_id + '-key',
                                                 trace_id='trace'))
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
        sent = (await executor.execute(request(), 'instance')).result
        assert sent.status == actions.SUCCEEDED
        # A message the adapter knows it sent can be recalled.
        deleted = (await executor.execute(delete_request(), 'instance')).result
        assert deleted.status == actions.SUCCEEDED and deleted.platform_message_id == ''
        # Unknown message IDs are never accepted as recall targets.
        unknown = (await executor.execute(delete_request(message_id='99999', action_id='unknown'),
                                          'instance')).result
        assert unknown.status == actions.FAILED and unknown.error.code == common.PERMISSION_DENIED
        # The same ID in another conversation is not authorized either.
        other = (await executor.execute(delete_request(conversation='789', action_id='other'),
                                        'instance')).result
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
        result = (await executor.execute(value, 'instance')).result
    finally:
        executor.close()
    assert result.status == actions.FAILED and result.error.code == code


def mute_request(duration=600, group_id='456', user_id='789', action_id='mute'):
    value = dispatch(actions.ActionEnvelope(action_id=action_id, idempotency_key=action_id + '-key',
                                                 trace_id='trace'))
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
    value = dispatch(actions.ActionEnvelope(action_id=action_id, idempotency_key=action_id + '-key',
                                                 trace_id='trace'))
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
        muted = (await executor.execute(mute_request(), 'instance')).result
        assert muted.status == actions.SUCCEEDED and muted.platform_message_id == ''
        kicked = (await executor.execute(kick_request(), 'instance')).result
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
        result = (await executor.execute(value, 'instance')).result
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
        result = (await executor.execute(value, 'instance')).result
    finally:
        executor.close()
    assert result.status == actions.FAILED and result.error.code == common.INVALID_ARGUMENT


def approve_request(action_id='approve', request_id='req-1', approve=True, reason='ok'):
    value = dispatch(actions.ActionEnvelope(action_id=action_id, idempotency_key=action_id + '-key',
                                                 trace_id='trace'))
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
        accepted = (await executor.execute(approve_request(), 'instance')).result
        assert accepted.status == actions.SUCCEEDED and accepted.platform_message_id == ''
        rejected = (await executor.execute(approve_request(action_id='reject', request_id='req-1',
                                                            approve=False, reason='no'),
                                           'instance')).result
        assert rejected.status == actions.SUCCEEDED
        # Replays come from the action log.
        await executor.execute(approve_request(), 'instance')
        # Unknown request tokens never reach the platform.
        unknown = (await executor.execute(approve_request(action_id='unknown',
                                                           request_id='missing'),
                                          'instance')).result
        assert unknown.status == actions.FAILED and unknown.error.code == common.NOT_FOUND
    finally:
        executor.close()
    assert calls == [
        ('set_friend_add_request', {'flag': 'flag-1', 'approve': True}),
        ('set_friend_add_request', {'flag': 'flag-1', 'approve': False}),
    ]


def receipt(action_id, attempt, disposition, code=0):
    value = actions.ActionReceipt(action_id=action_id, dispatch_attempt_id=attempt,
                                  disposition=disposition)
    if code:
        value.error.CopyFrom(common.Error(code=code, message=common.ErrorCode.Name(code)))
    return value


async def test_dispatch_identity_is_required_and_never_executes(tmp_path):
    async def call(api, params):
        pytest.fail('platform must not be called')
    executor = ActionExecutor(tmp_path / 'dispatch.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        for mutate in ('missing_attempt', 'bad_attempt', 'zero_sequence'):
            action = request()
            if mutate == 'missing_attempt':
                action.dispatch_attempt_id = ''
            elif mutate == 'bad_attempt':
                action.dispatch_attempt_id = 'not-a-uuid'
            else:
                action.dispatch_sequence = 0
            with pytest.raises(AdapterError) as failure:
                await executor.execute(action, 'instance')
            assert failure.value.code == common.INTERNAL
    finally:
        executor.close()


async def test_terminal_result_correlation_and_receipt_dispositions(tmp_path):
    calls = []

    async def call(api, params):
        calls.append(api)
        return {'message_id': 7}

    executor = ActionExecutor(tmp_path / 'receipt.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        action = request()
        report = await executor.execute(action, 'instance')
        assert report.kind == 'result'
        assert report.result.dispatch_attempt_id == action.dispatch_attempt_id
        assert report.result.execution_outcome == platform.EXECUTION_KNOWN_SUCCEEDED
        assert report.result.platform_message_id == '7'
        # Until a receipt arrives the report stays retransmittable.
        assert [item.key for item in executor.pending_reports()] == [report.key]
        assert executor.acknowledge(receipt(action.action_id, action.dispatch_attempt_id,
                                            actions.ACTION_RECEIPT_RECORDED)) == 'reported'
        assert executor.pending_reports() == []
        # Duplicate and evidence-only receipts are idempotent acknowledgements.
        assert executor.acknowledge(receipt(action.action_id, action.dispatch_attempt_id,
                                            actions.ACTION_RECEIPT_DUPLICATE)) == 'reported'
        assert executor.acknowledge(receipt('unknown', str(uuid.uuid4()),
                                            actions.ACTION_RECEIPT_RECORDED)) == 'unknown'

        conflict = request()
        conflict.action_id = 'conflict-action'
        conflict.trace_id = 'conflict-trace'
        report = await executor.execute(conflict, 'instance')
        assert executor.acknowledge(receipt(conflict.action_id, conflict.dispatch_attempt_id,
                                            actions.ACTION_RECEIPT_CONFLICT,
                                            common.CONFLICT)) == 'conflict'
        # A conflicted attempt is not retransmitted forever.
        assert executor.pending_reports() == []
        assert len(calls) == 2
    finally:
        executor.close()


async def test_redispatch_reports_known_terminal_result_without_executing(tmp_path):
    calls = []

    async def call(api, params):
        calls.append(api)
        return {'message_id': 11}

    executor = ActionExecutor(tmp_path / 'redispatch.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        first = request()
        original = await executor.execute(first, 'instance')
        redispatch = request()
        assert redispatch.dispatch_attempt_id != first.dispatch_attempt_id
        replay = await executor.execute(redispatch, 'instance')
        assert replay.kind == 'result'
        assert replay.dispatch_attempt_id == redispatch.dispatch_attempt_id
        assert replay.result.status == original.result.status
        assert replay.result.platform_message_id == original.result.platform_message_id
        assert replay.result.execution_outcome == original.result.execution_outcome
        assert len(calls) == 1
        pending = executor.pending_reports()
        assert len(pending) == 1 and pending[0].dispatch_attempt_id == redispatch.dispatch_attempt_id
    finally:
        executor.close()


async def test_crash_window_claim_recovers_as_unknown_evidence(tmp_path):
    path = tmp_path / 'crash.db'
    action = request()
    attempt, sequence = ActionExecutor.dispatch_fields(action)
    executor = ActionExecutor(path, 'adapter', '123', lambda *_: None, now=lambda: 1000)
    try:
        # Simulate a process exit after the pre-platform claim was persisted.
        executor._claim(action, ActionExecutor.fingerprint(action), attempt, sequence)
    finally:
        executor.close()
    recovered = ActionExecutor(path, 'adapter', '123',
                               lambda *_: pytest.fail('platform must not be called'),
                               now=lambda: 1000)
    try:
        pending = recovered.pending_reports()
        assert len(pending) == 1
        assert pending[0].dispatch_attempt_id == attempt
        assert pending[0].evidence.execution_outcome == platform.EXECUTION_UNKNOWN
    finally:
        recovered.close()


async def test_late_result_only_evidence_keeps_local_terminal(tmp_path):
    calls = []

    async def call(api, params):
        calls.append(api)
        return {'message_id': 21}

    executor = ActionExecutor(tmp_path / 'late.db', 'adapter', '123', call, now=lambda: 1000)
    try:
        action = request()
        report = await executor.execute(action, 'instance')
        authoritative = actions.ActionResult(action_id=action.action_id, status=actions.EXPIRED,
                                             dispatch_attempt_id=action.dispatch_attempt_id,
                                             execution_outcome=platform.EXECUTION_UNKNOWN)
        late = receipt(action.action_id, action.dispatch_attempt_id,
                       actions.ACTION_RECEIPT_EVIDENCE_ONLY, common.TIMEOUT)
        late.result.CopyFrom(authoritative)
        assert executor.acknowledge(late) == 'reported'
        assert executor.pending_reports() == []
        # The adapter's local evidence of successful execution is not overwritten by the receipt.
        row = executor.db.execute('SELECT state FROM actions WHERE id=?',
                                  (action.action_id,)).fetchone()
        assert row == ('terminal',)
        replay = await executor.execute(request(), 'instance')
        assert replay.result.status == actions.SUCCEEDED
        assert len(calls) == 1
    finally:
        executor.close()


async def test_v3_action_log_migrates_and_replays_terminal_result(tmp_path):
    path = tmp_path / 'legacy.db'
    action = request()
    fingerprint = ActionExecutor.fingerprint(action)
    result = terminal(action.action_id, actions.SUCCEEDED, platform_id='31')
    db = sqlite3.connect(path)
    try:
        db.execute('CREATE TABLE actions (id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, '
                   'result BLOB NOT NULL, terminal INTEGER NOT NULL, updated REAL NOT NULL)')
        db.execute('INSERT INTO actions VALUES(?,?,?,1,?)',
                   (action.action_id, fingerprint, result.SerializeToString(), 1000.0))
        db.execute('PRAGMA user_version=3')
        db.commit()
    finally:
        db.close()
    executor = ActionExecutor(path, 'adapter', '123',
                              lambda *_: pytest.fail('platform must not be called'),
                              now=lambda: 1000)
    try:
        assert executor.db.execute('PRAGMA user_version').fetchone()[0] == 4
        report = await executor.execute(action, 'instance')
        assert report.kind == 'result' and report.result.status == actions.SUCCEEDED
        assert report.result.platform_message_id == '31'
    finally:
        executor.close()
