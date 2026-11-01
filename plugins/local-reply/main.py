"""Fixed SDK handler for the local message MVP; no provider or external I/O."""
import json
import uuid
from datetime import datetime, timezone

from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common
from kiarabot_sdk import Plugin, PluginError

PLUGIN_ID = 'local-reply'
MAX_REPLY_BYTES = 4096
_PREFIX = '收到：'
_KNOWN_STATES = {actions.ACCEPTED, actions.SUCCEEDED, actions.FAILED,
                 actions.EXPIRED, actions.CANCELLED}


class LocalReply(Plugin):
    def __init__(self, now=None):
        self._now = now or (lambda: datetime.now(timezone.utc))

    async def on_event(self, event, context):
        if not event.HasField('message_received'):
            return
        message = event.message_received
        if (not message.message.plain_text.strip() or
                message.sender_id == event.source.account_id or
                message.conversation.type not in
                (common.ConversationRef.GROUP, common.ConversationRef.PRIVATE)):
            return
        if not event.event_id or not event.source.adapter_id or not event.source.account_id:
            raise PluginError(common.INVALID_ARGUMENT)
        # The original event time, including nanoseconds, fixes every replay's deadline.
        action = actions.ActionEnvelope()
        action.deadline.CopyFrom(event.occurred_at)
        action.deadline.seconds += 60
        try:
            deadline = action.deadline.ToDatetime(tzinfo=timezone.utc)
        except (ValueError, OverflowError):
            raise PluginError(common.INVALID_ARGUMENT) from None
        remaining = (deadline - self._now()).total_seconds()
        if remaining <= 0:
            return
        identity = json.dumps([PLUGIN_ID, event.source.adapter_id, event.source.account_id,
                               event.event_id], ensure_ascii=True, separators=(',', ':'))
        identifier = str(uuid.uuid5(uuid.NAMESPACE_URL, identity))
        key = PLUGIN_ID + '-' + identifier
        try:
            result = await context.query_action(idempotency_key=key, timeout=min(5.0, remaining))
        except PluginError as error:
            if error.code != common.NOT_FOUND:
                raise
        else:
            if result.status not in _KNOWN_STATES:
                raise PluginError(common.UPSTREAM_UNAVAILABLE)
            return  # Accepted OR terminal: never submit again or invent another key.
        remaining = (deadline - self._now()).total_seconds()
        if remaining <= 0:
            return
        action.action_id = identifier
        action.idempotency_key = key
        action.trace_id = event.trace_id
        action.requested_by = PLUGIN_ID
        action.target_source.CopyFrom(event.source)
        action.target.CopyFrom(event.subject)
        action.target.conversation_id = message.conversation.id
        action.send_message.conversation.CopyFrom(message.conversation)
        budget = MAX_REPLY_BYTES - len(_PREFIX.encode('utf-8'))
        text = message.message.plain_text.encode('utf-8')[:budget].decode('utf-8', errors='ignore')
        action.send_message.message.plain_text = _PREFIX + text
        await context.submit_action(action, timeout=min(5.0, remaining))


def create():
    return LocalReply()
