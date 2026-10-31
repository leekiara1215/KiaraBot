"""Unit tests for plugins/reminder; run with the SDK venv (no Core needed)."""
import asyncio
from datetime import datetime, timedelta, timezone

from kiarabot.v1 import common_pb2 as common, events_pb2 as events, plugins_pb2 as plugins
from kiarabot_sdk import PluginError

from reminder import create_plugin


def message_event(text, event_id='event-1', conversation_id='group-1'):
    result = events.EventEnvelope(event_id=event_id, trace_id='trace-1')
    result.occurred_at.GetCurrentTime()
    result.source.adapter_id = 'adapter'
    result.source.account_id = 'account'
    result.subject.conversation_id = conversation_id
    result.message_received.message_id = 'message-1'
    result.message_received.sender_id = 'user-1'
    result.message_received.conversation.id = conversation_id
    result.message_received.conversation.account_id = 'account'
    result.message_received.conversation.type = common.ConversationRef.GROUP
    result.message_received.message.plain_text = text
    return result


def timer_event(task_id='task-1', payload=b'ping', owner='reminder', event_id='timer-1'):
    result = events.EventEnvelope(event_id=event_id, trace_id='trace-1')
    result.occurred_at.GetCurrentTime()
    result.source.adapter_id = 'adapter'
    result.source.account_id = 'account'
    result.subject.conversation_id = 'group-1'
    result.timer_triggered.task_id = task_id
    result.timer_triggered.owner_plugin_id = owner
    result.timer_triggered.payload = payload
    result.timer_triggered.scheduled_at.GetCurrentTime()
    return result


class Context:
    def __init__(self):
        self.created = []
        self.cancelled = []
        self.actions = []
        self.tasks = {}

    async def task_create(self, task_id, target_conversation, payload, idempotency_key, *,
                          run_at=None, local_time=None, timeout=5.0):
        self.created.append(dict(task_id=task_id, target=target_conversation, payload=payload,
                                 idempotency_key=idempotency_key, run_at=run_at,
                                 local_time=local_time))
        self.tasks[task_id] = plugins.TaskResponse(task_id=task_id,
                                                   state=plugins.SCHEDULED_TASK_PENDING)
        return self.tasks[task_id]

    async def task_get(self, task_id, timeout=5.0):
        if task_id not in self.tasks:
            raise PluginError(common.NOT_FOUND)
        return self.tasks[task_id]

    async def task_cancel(self, task_id, idempotency_key, timeout=5.0):
        if task_id not in self.tasks:
            raise PluginError(common.NOT_FOUND)
        self.cancelled.append((task_id, idempotency_key))
        self.tasks[task_id] = plugins.TaskResponse(task_id=task_id,
                                                   state=plugins.SCHEDULED_TASK_CANCELLED)
        return self.tasks[task_id]

    async def submit_action(self, action):
        self.actions.append(action)


def config(**values):
    result = plugins.PluginConfig(version='1', delivery_id='delivery-1', apply_timeout_ms=5000)
    result.values.update(values)
    return result


def run(plugin, event, context):
    asyncio.run(plugin.on_event(event, context))


def reply_text(context):
    assert len(context.actions) == 1
    return context.actions[0].send_message.message.plain_text


def test_schedule_creates_utc_task_and_replies():
    before = datetime.now(timezone.utc)
    context = Context()
    run(create_plugin(), message_event('/remind 30 take a break'), context)
    assert len(context.created) == 1
    created = context.created[0]
    assert created['target'] == 'group-1'
    assert created['payload'] == b'take a break'
    expected = before + timedelta(seconds=30)
    assert abs((created['run_at'] - expected).total_seconds()) < 5
    assert created['run_at'].utcoffset() == timedelta(0)
    text = reply_text(context)
    assert 'scheduled task' in text


def test_same_event_produces_stable_task_and_idempotency_keys():
    first, second = Context(), Context()
    run(create_plugin(), message_event('/remind 10 ping'), first)
    run(create_plugin(), message_event('/remind 10 ping'), second)
    assert first.created[0]['task_id'] == second.created[0]['task_id']
    assert first.created[0]['idempotency_key'] == second.created[0]['idempotency_key']


def test_usage_and_bounds_do_not_schedule():
    for text in ('/remind', '/remind nope hi', '/remind 0 hi', '/remind -3 hi'):
        context = Context()
        run(create_plugin(), message_event(text), context)
        assert context.created == []
        assert 'usage' in reply_text(context) or 'seconds must be' in reply_text(context)


def test_cancel_and_status():
    plugin = create_plugin()
    context = Context()
    run(plugin, message_event('/remind 10 ping'), context)
    task_id = context.created[0]['task_id']
    context.actions.clear()
    run(plugin, message_event('/remind status ' + task_id, event_id='event-2'), context)
    assert 'SCHEDULED_TASK_PENDING' in reply_text(context)
    context.actions.clear()
    run(plugin, message_event('/remind cancel ' + task_id, event_id='event-3'), context)
    assert context.cancelled and context.cancelled[0][0] == task_id
    assert 'SCHEDULED_TASK_CANCELLED' in reply_text(context)
    context.actions.clear()
    run(plugin, message_event('/remind status missing', event_id='event-4'), context)
    assert 'not found' in reply_text(context)


def test_timer_event_replies_idempotently_and_ignores_other_owners():
    first, second = Context(), Context()
    run(create_plugin(), timer_event(), first)
    run(create_plugin(), timer_event(), second)
    assert len(first.actions) == 1
    action = first.actions[0]
    assert action.send_message.conversation.id == 'group-1'
    assert action.send_message.message.plain_text == 'ping'
    assert action.idempotency_key == second.actions[0].idempotency_key
    foreign = Context()
    run(create_plugin(), timer_event(owner='another-plugin'), foreign)
    assert foreign.actions == []
    empty = Context()
    run(create_plugin(), timer_event(payload=b''), empty)
    assert empty.actions == []


def test_config_changes_default_bounds():
    plugin, context = create_plugin(), Context()
    asyncio.run(plugin.on_config(config(max_seconds=5), context))
    run(plugin, message_event('/remind 10 too long'), context)
    assert context.created == []
    assert 'between 1 and 5' in reply_text(context)
    try:
        asyncio.run(plugin.on_config(config(max_seconds='not-a-number'), context))
    except PluginError as error:
        assert error.code == common.INVALID_ARGUMENT
    else:
        raise AssertionError('invalid configuration value must be rejected')


def test_unrelated_events_are_ignored():
    context = Context()
    plugin = create_plugin()
    run(plugin, events.EventEnvelope(event_id='status'), context)
    run(plugin, message_event('hello'), context)
    assert context.created == [] and context.actions == []
