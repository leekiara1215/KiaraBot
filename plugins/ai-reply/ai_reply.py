"""SDK-only example. An uncertain model call needs manual recovery, never replay."""
import hashlib
from datetime import timedelta, timezone

from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common
from kiarabot_sdk import Plugin, PluginError


class AiReply(Plugin):
    async def on_event(self, event, context):
        if not event.HasField('message_received'):
            return
        text = event.message_received.message.plain_text
        if not text.startswith('/ai ') or not text[4:].strip():
            return
        key = 'ai:' + hashlib.sha256(event.event_id.encode()).hexdigest()
        try:
            saved = await context.storage_get(key)
        except PluginError as error:
            if error.code != common.NOT_FOUND:
                raise
            await context.storage_set(key, b'', key + ':start')
            response = await context.model_chat('default', [{'role': 'user', 'text': text[4:]}], timeout=2)
            saved = response.text.encode()
            if not saved:
                return
            await context.storage_set(key, saved, key + ':answer')
        if not saved:
            return  # Prior request is ambiguous; do not bill the model twice.
        action = actions.ActionEnvelope(action_id=key, idempotency_key=key + ':reply',
            trace_id=event.trace_id, requested_by='ai-reply', target_source=event.source,
            target=event.subject)
        action.deadline.FromDatetime(event.occurred_at.ToDatetime(tzinfo=timezone.utc) + timedelta(minutes=5))
        action.send_message.conversation.CopyFrom(event.message_received.conversation)
        action.send_message.message.plain_text = saved.decode()
        await context.submit_action(action)


def create_plugin():
    return AiReply()
