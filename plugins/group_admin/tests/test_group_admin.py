"""Unit tests for plugins/group_admin; run with the SDK venv (no Core needed)."""
import asyncio
import json
from datetime import timezone

from kiarabot.v1 import common_pb2 as common, events_pb2 as events, plugins_pb2 as plugins
from kiarabot_sdk import PluginError

from group_admin import HELP, create_plugin


def event(text, event_id='event-1', conversation_type=common.ConversationRef.GROUP,
          conversation_id='group-1'):
    result = events.EventEnvelope(event_id=event_id, trace_id='trace-1')
    result.occurred_at.GetCurrentTime()
    result.source.adapter_id = 'adapter'
    result.source.account_id = 'account'
    result.subject.conversation_id = conversation_id
    result.message_received.message_id = 'message-1'
    result.message_received.sender_id = 'user-1'
    result.message_received.conversation.id = conversation_id
    result.message_received.conversation.account_id = 'account'
    result.message_received.conversation.type = conversation_type
    result.message_received.message.plain_text = text
    return result


class Context:
    def __init__(self, storage=None, failure=None):
        self.values = dict(storage or {})
        self.failure = failure
        self.actions = []

    async def storage_get(self, key):
        if self.failure:
            raise PluginError(self.failure)
        if key not in self.values:
            raise PluginError(common.NOT_FOUND)
        return self.values[key]

    async def storage_set(self, key, value, idempotency_key):
        if self.failure:
            raise PluginError(self.failure)
        self.values[key] = value

    async def submit_action(self, action):
        self.actions.append(action)


def config(**values):
    result = plugins.PluginConfig(version='1', delivery_id='delivery-1', apply_timeout_ms=5000)
    result.values.update(values)
    return result


def run(plugin, event, context):
    asyncio.run(plugin.on_event(event, context))


def reply(context):
    assert len(context.actions) == 1
    action = context.actions[0]
    assert action.HasField('send_message')
    assert not action.HasField('mute_member')
    assert not action.HasField('kick_member')
    assert not action.HasField('approve_request')
    assert action.deadline.ToDatetime(tzinfo=timezone.utc).timestamp() > 0
    return action


def test_help_and_deterministic_reply_key():
    first, second = Context(), Context()
    run(create_plugin(), event('/group help'), first)
    run(create_plugin(), event('/group help'), second)
    action = reply(first)
    assert action.send_message.message.plain_text == HELP
    assert action.send_message.conversation.id == 'group-1'
    assert action.idempotency_key == second.actions[0].idempotency_key
    assert action.target.conversation_id == 'group-1'


def test_moderation_commands_are_explicitly_not_implemented():
    for command in ('mute 42 60', 'kick 42', 'approve 7 yes'):
        context = Context()
        run(create_plugin(), event('/group ' + command), context)
        action = reply(context)
        assert 'not implemented' in action.send_message.message.plain_text


def test_info_uses_applied_config_and_rules():
    context = Context()
    plugin = create_plugin()
    asyncio.run(plugin.on_config(config(welcome_enabled=True), context))
    run(plugin, event('/group info'), context)
    text = reply(context).send_message.message.plain_text
    assert 'conversation=group-1' in text and 'rules=0' in text and 'welcome_enabled=True' in text


def test_rule_add_list_and_limit():
    context = Context()
    plugin = create_plugin()
    run(plugin, event('/group rule add be nice'), context)
    action = reply(context)
    assert action.send_message.message.plain_text == 'rule added (1/20).'
    stored = json.loads(context.values['rules:group-1'].decode())
    assert stored == ['be nice']
    context.actions.clear()
    run(plugin, event('/group rules', event_id='event-2'), context)
    assert '1. be nice' in reply(context).send_message.message.plain_text
    context.actions.clear()
    run(plugin, event('/group absent', event_id='event-3'), context)
    assert 'unknown command' in reply(context).send_message.message.plain_text


def test_rule_limit_comes_from_config():
    context = Context()
    plugin = create_plugin()
    asyncio.run(plugin.on_config(config(rule_limit=1), context))
    run(plugin, event('/group rule add one'), context)
    context.actions.clear()
    run(plugin, event('/group rule add two', event_id='event-2'), context)
    assert 'limit (1) reached' in reply(context).send_message.message.plain_text


def test_non_group_and_non_command_events_are_ignored():
    context = Context()
    plugin = create_plugin()
    run(plugin, event('/group help', conversation_type=common.ConversationRef.PRIVATE), context)
    assert 'only responds in group conversations' in reply(context).send_message.message.plain_text
    context.actions.clear()
    run(plugin, event('hello'), context)
    run(plugin, events.EventEnvelope(event_id='timer'), context)
    assert context.actions == []


def test_storage_failure_is_not_swallowed():
    context = Context(failure=common.UPSTREAM_UNAVAILABLE)
    try:
        run(create_plugin(), event('/group rules'), context)
    except PluginError as error:
        assert error.code == common.UPSTREAM_UNAVAILABLE
    else:
        raise AssertionError('storage failure must propagate')
    assert context.actions == []
