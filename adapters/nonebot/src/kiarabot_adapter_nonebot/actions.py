from __future__ import annotations

import asyncio
import hashlib
import sqlite3
import time

from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common
from .mapper import AdapterError, identifier


def terminal(action_id, status, code=0, platform_id=''):
    result = actions.ActionResult(action_id=action_id, status=status, platform_message_id=platform_id)
    result.completed_at.GetCurrentTime()
    if code:
        result.error.CopyFrom(common.Error(code=code, message=common.ErrorCode.Name(code), request_id=action_id))
    return result


class ActionExecutor:
    """Single event-loop owner. Claim before external I/O; never retry unknown effects."""
    def __init__(self, path, adapter_id, account_id, call_platform, now=time.time, limit=10000, timeout=5.0):
        self.adapter_id, self.account_id, self.call_platform = adapter_id, account_id, call_platform
        self.now, self.limit, self.timeout = now, limit, timeout
        self.db = sqlite3.connect(path)
        try:
            if self.db.execute('PRAGMA user_version').fetchone()[0] not in (0, 1):
                raise AdapterError(common.INTERNAL)
            self.db.execute('PRAGMA synchronous=FULL')
            with self.db:
                self.db.execute('CREATE TABLE IF NOT EXISTS actions (id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, '
                                'result BLOB NOT NULL, terminal INTEGER NOT NULL, updated REAL NOT NULL)')
                self.db.execute('PRAGMA user_version=1')
        except Exception:
            self.db.close()
            raise

    def validate(self, action, instance):
        identifier(action.action_id)
        identifier(action.idempotency_key)
        if (action.target_source.adapter_id != self.adapter_id or action.target_source.account_id != self.account_id
                or action.target_source.adapter_instance_id != instance):
            raise AdapterError(common.PERMISSION_DENIED)
        if action.WhichOneof('payload') != 'send_message':
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        send = action.send_message
        if (send.reply or send.reply_to_message_id or send.message.segments
                or send.conversation.type not in (common.ConversationRef.GROUP, common.ConversationRef.PRIVATE)):
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        if (not send.conversation.id.isascii() or not send.conversation.id.isdigit()
                or len(send.conversation.id) > 20 or int(send.conversation.id) <= 0
                or send.conversation.account_id != self.account_id or action.target.conversation_id != send.conversation.id
                or not send.message.plain_text or len(send.message.plain_text.encode()) > 65536
                or not 0 < action.deadline.seconds <= 9223372035 or not 0 <= action.deadline.nanos < 10**9):
            raise AdapterError(common.INVALID_ARGUMENT)
        group = send.conversation.type == common.ConversationRef.GROUP
        # Plain text is a OneBot text segment, never CQ-code input.
        return ('send_group_msg' if group else 'send_private_msg',
                {('group_id' if group else 'user_id'): int(send.conversation.id),
                 'message': [{'type': 'text', 'data': {'text': send.message.plain_text}}]})

    async def execute(self, action, instance):
        try:
            api, params = self.validate(action, instance)
        except AdapterError as failure:
            return terminal(action.action_id, actions.FAILED, failure.code)
        canonical = actions.ActionEnvelope()
        canonical.CopyFrom(action)
        canonical.target_source.adapter_instance_id = ''
        fingerprint = hashlib.sha256(canonical.SerializeToString(deterministic=True)).hexdigest()
        try:
            with self.db:
                self.db.execute('BEGIN IMMEDIATE')
                old = self.db.execute('SELECT fingerprint,result FROM actions WHERE id=?', (action.action_id,)).fetchone()
                if old:
                    if old[0] != fingerprint:
                        return terminal(action.action_id, actions.FAILED, common.CONFLICT)
                    return actions.ActionResult.FromString(old[1])
                self.db.execute('DELETE FROM actions WHERE terminal=1 AND updated < ?', (self.now() - 7 * 86400,))
                count, size = self.db.execute('SELECT count(*),coalesce(sum(length(result)),0) FROM actions').fetchone()
                if count >= self.limit or size > 64 * 1024 * 1024:
                    raise AdapterError(common.RATE_LIMITED)
                result = actions.ActionResult(action_id=action.action_id, status=actions.ACCEPTED)
                self.db.execute('INSERT INTO actions VALUES(?,?,?,0,?)',
                                (action.action_id, fingerprint, result.SerializeToString(), self.now()))
            remaining = action.deadline.seconds + action.deadline.nanos / 1e9 - self.now()
            if remaining <= 0:
                result = terminal(action.action_id, actions.EXPIRED, common.TIMEOUT)
            else:
                try:
                    response = await asyncio.wait_for(self.call_platform(api, params), min(remaining, self.timeout))
                    platform_id = response.get('message_id') if isinstance(response, dict) else None
                    if isinstance(platform_id, (str, int)) and not isinstance(platform_id, bool) and 0 < len(str(platform_id)) <= 256:
                        result = terminal(action.action_id, actions.SUCCEEDED, platform_id=str(platform_id))
                except AdapterError as failure:
                    # Only an explicit platform rejection is a known failure.
                    result = terminal(action.action_id, actions.FAILED, failure.code)
                except asyncio.CancelledError:
                    raise
                except Exception:
                    # Timeout/disconnect may have happened after the platform effect.
                    pass
            if result.status != actions.ACCEPTED:
                with self.db:
                    self.db.execute('UPDATE actions SET result=?,terminal=1,updated=? WHERE id=?',
                                    (result.SerializeToString(), self.now(), action.action_id))
            return result
        except sqlite3.Error:
            raise AdapterError(common.UPSTREAM_UNAVAILABLE) from None

    def close(self):
        self.db.close()
