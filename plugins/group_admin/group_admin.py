"""Group administration example using only frozen v1 capabilities.

Reads ``event.message.received``, keeps per-group rules in the plugin namespace
storage and answers with the plain-text ``send_message`` Action. Moderation
payloads (MuteMember / KickMember / ApproveRequest) are marked
``CONTRACT_STATUS: not_implemented`` in ``proto/kiarabot/v1/actions.proto``; this
example never constructs them and says so in its replies.
"""
import hashlib
import json
from datetime import timedelta, timezone

from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common
from kiarabot_sdk import Plugin, PluginError

PLUGIN_ID = 'group-admin'
DEFAULT_RULE_LIMIT = 20
HELP = (
    '/group help — show this help\n'
    '/group info — show conversation and applied settings\n'
    '/group rules — list stored rules\n'
    '/group rule add <text> — append a rule for this group\n'
    '/group welcome — show the welcome setting\n'
    'Mute/Kick/Approve are not implemented by the v1 contract and are never sent.'
)


def _key(event_id, suffix):
    return 'ga:' + hashlib.sha256(event_id.encode()).hexdigest() + ':' + suffix


def _rules_key(conversation_id):
    return 'rules:' + conversation_id


class GroupAdmin(Plugin):
    """Per-group rule storage and read-only status commands."""

    def __init__(self):
        self.settings = {}

    async def on_config(self, config, context):
        for name in ('rule_limit', 'welcome_enabled'):
            if name in config.values:
                self.settings[name] = config.values[name]

    async def on_event(self, event, context):
        if not event.HasField('message_received'):
            return
        text = event.message_received.message.plain_text.strip()
        if not text.startswith('/group'):
            return
        reply = await self._handle(event, context, text.split()[1:] or ['help'])
        if reply:
            await self._reply(event, context, reply)

    async def _handle(self, event, context, parts):
        conversation = event.message_received.conversation
        if conversation.type != common.ConversationRef.GROUP:
            return 'group-admin only responds in group conversations.'
        name = parts[0]
        if name == 'help':
            return HELP
        if name in ('mute', 'kick', 'approve'):
            return ('Mute/Kick/Approve are not implemented by the v1 contract; '
                    'this example never sends those Action payloads.')
        if name == 'info':
            rules = await self._load(context, conversation.id)
            return (f'conversation={conversation.id} rules={len(rules)} '
                    f"welcome_enabled={bool(self.settings.get('welcome_enabled', False))}")
        if name == 'welcome':
            return (f"welcome_enabled={bool(self.settings.get('welcome_enabled', False))}; "
                    'change it through the control API configuration, not chat commands.')
        if name == 'rules':
            rules = await self._load(context, conversation.id)
            if not rules:
                return 'no rules stored for this conversation.'
            return '\n'.join(f'{index + 1}. {rule}' for index, rule in enumerate(rules))
        if name == 'rule' and len(parts) >= 3 and parts[1] == 'add':
            rule = ' '.join(parts[2:]).strip()
            if not rule:
                return 'usage: /group rule add <text>'
            return await self._add_rule(event, context, conversation.id, rule)
        return 'unknown command; send /group help'

    async def _load(self, context, conversation_id):
        try:
            raw = await context.storage_get(_rules_key(conversation_id))
        except PluginError as error:
            if error.code != common.NOT_FOUND:
                raise
            return []
        return json.loads(raw.decode('utf-8') or '[]')

    async def _add_rule(self, event, context, conversation_id, rule):
        rules = await self._load(context, conversation_id)
        limit = int(self.settings.get('rule_limit', DEFAULT_RULE_LIMIT))
        if len(rules) >= limit:
            return f'rule limit ({limit}) reached; delete rules through the control API.'
        rules.append(rule)
        await context.storage_set(_rules_key(conversation_id),
                                  json.dumps(rules, ensure_ascii=False).encode('utf-8'),
                                  _key(event.event_id, 'rule'))
        return f'rule added ({len(rules)}/{limit}).'

    async def _reply(self, event, context, text):
        reply = event.message_received
        action = actions.ActionEnvelope(
            action_id=_key(event.event_id, 'reply'),
            idempotency_key=_key(event.event_id, 'reply'),
            trace_id=event.trace_id,
            requested_by=PLUGIN_ID,
            target_source=event.source,
            target=event.subject)
        action.deadline.FromDatetime(
            event.occurred_at.ToDatetime(tzinfo=timezone.utc) + timedelta(minutes=5))
        action.send_message.conversation.CopyFrom(reply.conversation)
        action.send_message.message.plain_text = text
        await context.submit_action(action)


def create_plugin():
    return GroupAdmin()
