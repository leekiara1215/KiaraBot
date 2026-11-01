"""SDK public-protocol fixtures only; no QQ/provider/database calls."""
import asyncio
import importlib.util
from datetime import datetime, timezone
from pathlib import Path

import pytest
from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common, events_pb2 as events
from kiarabot_sdk import PluginError

_spec = importlib.util.spec_from_file_location('local_reply_plugin', Path(__file__).parents[1] / 'main.py')
_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_module)


def event(kind=common.ConversationRef.GROUP, text='你好'):
    value = events.EventEnvelope(event_id='event-1', trace_id='trace-1')
    value.occurred_at.seconds = 1700000000
    value.occurred_at.nanos = 123456789
    value.source.adapter_id = 'adapter'
    value.source.account_id = '123'
    value.subject.conversation_id = 'conversation'
    message = value.message_received
    message.sender_id = '456'
    message.conversation.id = 'conversation'
    message.conversation.account_id = '123'
    message.conversation.type = kind
    message.message.plain_text = text
    return value


class Context:
    def __init__(self, status=None, query_error=None, submit_error=None, after_query=None):
        self.status = status
        self.query_error, self.submit_error = query_error, submit_error
        self.after_query = after_query
        self.queries, self.actions = [], []

    async def query_action(self, *, idempotency_key, timeout):
        self.queries.append(idempotency_key)
        assert 0 < timeout <= 5
        if self.after_query:
            self.after_query()
        if self.query_error:
            raise PluginError(self.query_error)
        if self.status is None:
            raise PluginError(common.NOT_FOUND)
        return actions.ActionResult(action_id='existing', status=self.status)

    async def submit_action(self, action, timeout):
        assert 0 < timeout <= 5
        self.actions.append(action)
        if self.submit_error:
            raise PluginError(self.submit_error)
        self.status = actions.ACCEPTED
        return actions.ActionResult(action_id=action.action_id, status=self.status)


def run(value, context, clock=None):
    now = clock or (lambda: datetime.fromtimestamp(1700000010, timezone.utc))
    asyncio.run(_module.LocalReply(now).on_event(value, context))


@pytest.mark.parametrize('kind', [common.ConversationRef.GROUP, common.ConversationRef.PRIVATE])
def test_exact_text_action_and_stable_replay(kind):
    value, context = event(kind), Context()
    run(value, context)
    assert len(context.actions) == 1
    action = context.actions[0]
    assert action.requested_by == 'local-reply'
    assert action.send_message.message.plain_text == '收到：你好'
    assert action.send_message.conversation == value.message_received.conversation
    assert action.target_source == value.source
    assert action.target.conversation_id == 'conversation'
    assert action.deadline.seconds == value.occurred_at.seconds + 60
    assert action.deadline.nanos == value.occurred_at.nanos
    assert action.idempotency_key == context.queries[0]
    value.trace_id = 'replayed-trace'
    run(value, context)
    assert len(context.actions) == 1
    assert context.queries == [action.idempotency_key, action.idempotency_key]
    fresh = Context()
    run(value, fresh)
    assert fresh.actions[0].action_id == action.action_id
    assert fresh.actions[0].deadline == action.deadline


@pytest.mark.parametrize('status', [actions.ACCEPTED, actions.SUCCEEDED, actions.FAILED,
                                    actions.EXPIRED, actions.CANCELLED])
def test_existing_action_never_resubmits(status):
    context = Context(status=status)
    run(event(), context)
    assert not context.actions


def test_self_empty_nonmessage_and_expired_are_ignored():
    values = [event(text=''), event(text=' \t\n'), events.EventEnvelope(), event()]
    values[-1].message_received.sender_id = '123'
    expired = event()
    expired.occurred_at.seconds -= 60
    values.append(expired)
    for value in values:
        context = Context()
        run(value, context)
        assert not context.actions and not context.queries


def test_query_delay_past_fixed_deadline_does_not_send():
    clock = [datetime.fromtimestamp(1700000010, timezone.utc)]
    context = Context(after_query=lambda: clock.__setitem__(
        0, datetime.fromtimestamp(1700000061, timezone.utc)))
    run(event(), context, lambda: clock[0])
    assert len(context.queries) == 1 and not context.actions


@pytest.mark.parametrize('code', [common.UPSTREAM_UNAVAILABLE, common.TIMEOUT, common.PERMISSION_DENIED])
def test_query_dependency_errors_propagate_without_submit(code):
    context = Context(query_error=code)
    with pytest.raises(PluginError) as failure:
        run(event(), context)
    assert failure.value.code == code
    assert not context.actions and len(context.queries) == 1


def test_submit_unknown_propagates_and_does_not_retry_in_hook():
    context = Context(submit_error=common.UPSTREAM_UNAVAILABLE)
    with pytest.raises(PluginError) as failure:
        run(event(), context)
    assert failure.value.code == common.UPSTREAM_UNAVAILABLE
    assert len(context.actions) == len(context.queries) == 1


def test_unknown_query_status_is_not_success_or_new_submit():
    context = Context(status=actions.ACTION_STATUS_UNSPECIFIED)
    with pytest.raises(PluginError) as failure:
        run(event(), context)
    assert failure.value.code == common.UPSTREAM_UNAVAILABLE
    assert not context.actions


def test_utf8_limit_and_cq_remains_literal_plain_text():
    context = Context()
    run(event(text='[CQ:image,file=x]' + '🙂' * 5000), context)
    reply = context.actions[0].send_message.message
    assert reply.plain_text.startswith('收到：[CQ:image,file=x]')
    assert len(reply.plain_text.encode('utf-8')) <= _module.MAX_REPLY_BYTES
    assert not reply.segments
