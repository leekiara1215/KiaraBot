"""Reminder example using only frozen v1 capabilities.

``/remind`` commands create one-shot Host tasks (``host.tasks.v1``); the Core
scheduler later delivers a ``timer_triggered`` event back to the owning plugin,
which replies with the due text. Task ownership, time-zone resolution and
idempotency are Core rules; this plugin only keeps its own ``task_id`` mapping
deterministic and reuses the same Action idempotency key for a replayed timer.
"""
import hashlib
from datetime import datetime, timedelta, timezone

from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common, plugins_pb2 as plugins
from kiarabot_sdk import Plugin, PluginError

PLUGIN_ID = 'reminder'
DEFAULT_SECONDS = 60
DEFAULT_MAX_SECONDS = 86400
USAGE = ('usage: /remind <seconds> <text> | /remind status <task_id> | '
         '/remind cancel <task_id>')


def _key(event_id, suffix):
    return 'rem:' + hashlib.sha256(event_id.encode()).hexdigest() + ':' + suffix


class Reminder(Plugin):
    """Creates Host tasks on message events and answers timer events."""

    def __init__(self):
        self.settings = {'default_seconds': DEFAULT_SECONDS, 'max_seconds': DEFAULT_MAX_SECONDS}

    async def on_config(self, config, context):
        for name in ('default_seconds', 'max_seconds'):
            if name in config.values:
                try:
                    self.settings[name] = int(config.values[name])
                except (TypeError, ValueError):
                    raise PluginError(common.INVALID_ARGUMENT) from None

    async def on_event(self, event, context):
        if event.HasField('timer_triggered'):
            await self._on_timer(event, context)
            return
        if not event.HasField('message_received'):
            return
        text = event.message_received.message.plain_text.strip()
        if not text.startswith('/remind'):
            return
        reply = await self._handle(event, context, text.split()[1:])
        if reply:
            await self._reply(event, context, reply)

    async def _handle(self, event, context, parts):
        conversation = event.message_received.conversation
        if not parts:
            return USAGE
        if parts[0] == 'status' and len(parts) == 2:
            try:
                task = await context.task_get(parts[1])
            except PluginError as error:
                if error.code == common.NOT_FOUND:
                    return f'task {parts[1]} not found.'
                raise
            return f'task {task.task_id} state={plugins.ScheduledTaskState.Name(task.state)}'
        if parts[0] == 'cancel' and len(parts) == 2:
            try:
                task = await context.task_cancel(parts[1], _key(event.event_id, 'cancel'))
            except PluginError as error:
                if error.code == common.NOT_FOUND:
                    return f'task {parts[1]} not found.'
                raise
            return f'task {task.task_id} state={plugins.ScheduledTaskState.Name(task.state)}'
        return await self._schedule(event, context, conversation.id, parts)

    async def _schedule(self, event, context, conversation_id, parts):
        if len(parts) < 2:
            return USAGE
        try:
            seconds = int(parts[0])
        except ValueError:
            return USAGE
        text = ' '.join(parts[1:]).strip()
        if not text:
            return USAGE
        limit = int(self.settings['max_seconds'])
        if not 1 <= seconds <= limit:
            return f'seconds must be between 1 and {limit}.'
        task_id = _key(event.event_id, 'task').replace(':', '-')
        run_at = datetime.now(timezone.utc) + timedelta(seconds=seconds)
        await context.task_create(task_id, conversation_id, text.encode('utf-8'),
                                  _key(event.event_id, 'create'), run_at=run_at)
        return f'scheduled task {task_id} at {run_at.isoformat()}'

    async def _on_timer(self, event, context):
        trigger = event.timer_triggered
        if trigger.owner_plugin_id and trigger.owner_plugin_id != PLUGIN_ID:
            return
        text = trigger.payload.decode('utf-8')
        if not text:
            return
        action = actions.ActionEnvelope(
            action_id='rem-fire-' + trigger.task_id,
            idempotency_key='rem:fire:' + trigger.task_id,
            trace_id=event.trace_id,
            requested_by=PLUGIN_ID,
            target_source=event.source,
            target=event.subject)
        action.deadline.FromDatetime(
            event.occurred_at.ToDatetime(tzinfo=timezone.utc) + timedelta(minutes=5))
        action.send_message.conversation.id = event.subject.conversation_id
        action.send_message.message.plain_text = text
        # Duplicate timers are idempotent at Core; other failures stay visible to
        # the event ACK so the event is retried instead of silently dropped.
        await context.submit_action(action)

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
    return Reminder()
