from __future__ import annotations

import asyncio
import hashlib
import sqlite3
import time

from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common
from .mapper import AdapterError, identifier, platform_id

# Platform-neutral reaction tokens mapped to NapCat's emoji IDs. NapCat exposes
# message reactions through the set_msg_emoji_like extension API, not through
# OneBot V11's standard action set; the mapping must be re-verified on a real
# account (T07b) before being treated as verified platform behavior.
REACTIONS = {
    'like': '128077',
    'laugh': '128514',
    'surprise': '128558',
    'sad': '128546',
    'angry': '128545',
}


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
            if self.db.execute('PRAGMA user_version').fetchone()[0] not in (0, 1, 2, 3):
                raise AdapterError(common.INTERNAL)
            self.db.execute('PRAGMA synchronous=FULL')
            with self.db:
                self.db.execute('CREATE TABLE IF NOT EXISTS actions (id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, '
                                'result BLOB NOT NULL, terminal INTEGER NOT NULL, updated REAL NOT NULL)')
                # Only messages this bot sent are eligible for DeleteMessage; the
                # platform message ID alone is never authorization.
                self.db.execute('CREATE TABLE IF NOT EXISTS own_messages (message_id TEXT PRIMARY KEY, '
                                'conversation TEXT NOT NULL, updated REAL NOT NULL)')
                # Friend-request tokens are adapter-scoped; the raw platform flag
                # stays local and ApproveRequest requests the token.
                self.db.execute('CREATE TABLE IF NOT EXISTS requests (request_id TEXT PRIMARY KEY, '
                                'flag TEXT NOT NULL, updated REAL NOT NULL)')
                self.db.execute('PRAGMA user_version=3')
        except Exception:
            self.db.close()
            raise

    def remember_request(self, request_id, flag):
        """Records the platform flag for an emitted FriendRequest token."""
        identifier(request_id)
        identifier(flag)
        with self.db:
            self.db.execute('DELETE FROM requests WHERE updated < ?', (self.now() - 7 * 86400,))
            self.db.execute('INSERT OR REPLACE INTO requests VALUES(?,?,?)',
                            (request_id, flag, self.now()))

    def validate(self, action, instance):
        identifier(action.action_id)
        identifier(action.idempotency_key)
        if (action.target_source.adapter_id != self.adapter_id or action.target_source.account_id != self.account_id
                or action.target_source.adapter_instance_id != instance):
            raise AdapterError(common.PERMISSION_DENIED)
        if not 0 < action.deadline.seconds <= 9223372035 or not 0 <= action.deadline.nanos < 10**9:
            raise AdapterError(common.INVALID_ARGUMENT)
        payload = action.WhichOneof('payload')
        if payload == 'send_message':
            return self._send_message(action)
        if payload == 'react_message':
            return self._react_message(action)
        if payload == 'delete_message':
            return self._delete_message(action)
        if payload == 'mute_member':
            return self._mute_member(action)
        if payload == 'kick_member':
            return self._kick_member(action)
        if payload == 'approve_request':
            return self._approve_request(action)
        raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)

    def _send_message(self, action):
        send = action.send_message
        if (send.reply or send.reply_to_message_id or send.message.segments
                or send.conversation.type not in (common.ConversationRef.GROUP, common.ConversationRef.PRIVATE)):
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        # Native OneBot numeric ID, identical to the inbound MessageReceived conversation.
        conversation = platform_id(send.conversation.id)
        if (send.conversation.account_id != self.account_id or action.target.conversation_id != conversation
                or not send.message.plain_text or len(send.message.plain_text.encode()) > 65536):
            raise AdapterError(common.INVALID_ARGUMENT)
        group = send.conversation.type == common.ConversationRef.GROUP
        # Plain text is a OneBot text segment, never CQ-code input.
        return ('send_group_msg' if group else 'send_private_msg',
                {('group_id' if group else 'user_id'): int(conversation),
                 'message': [{'type': 'text', 'data': {'text': send.message.plain_text}}]})

    def _react_message(self, action):
        react = action.react_message
        if react.reaction not in REACTIONS:
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        if react.conversation.type not in (common.ConversationRef.GROUP, common.ConversationRef.PRIVATE):
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        conversation = platform_id(react.conversation.id)
        message_id = platform_id(react.message_id)
        if (react.conversation.account_id != self.account_id or action.target.conversation_id != conversation):
            raise AdapterError(common.INVALID_ARGUMENT)
        return ('set_msg_emoji_like', {'message_id': int(message_id), 'emoji_id': REACTIONS[react.reaction]})

    def _delete_message(self, action):
        deletion = action.delete_message
        if deletion.conversation.type not in (common.ConversationRef.GROUP, common.ConversationRef.PRIVATE):
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        conversation = platform_id(deletion.conversation.id)
        message_id = platform_id(deletion.message_id)
        if deletion.conversation.account_id != self.account_id or action.target.conversation_id != conversation:
            raise AdapterError(common.INVALID_ARGUMENT)
        row = self.db.execute('SELECT conversation FROM own_messages WHERE message_id=?',
                              (message_id,)).fetchone()
        if not row or row[0] != conversation:
            raise AdapterError(common.PERMISSION_DENIED)
        return ('delete_msg', {'message_id': int(message_id)})

    def _mute_member(self, action):
        mute = action.mute_member
        group_id = platform_id(mute.group_id)
        user_id = platform_id(mute.user_id)
        if action.target.conversation_id != group_id:
            raise AdapterError(common.INVALID_ARGUMENT)
        if not 1 <= mute.duration_seconds <= 2592000:
            raise AdapterError(common.INVALID_ARGUMENT)
        return ('set_group_ban', {'group_id': int(group_id), 'user_id': int(user_id),
                                  'duration': mute.duration_seconds})

    def _kick_member(self, action):
        kick = action.kick_member
        group_id = platform_id(kick.group_id)
        user_id = platform_id(kick.user_id)
        if action.target.conversation_id != group_id:
            raise AdapterError(common.INVALID_ARGUMENT)
        # OneBot V11 set_group_kick has no reason parameter; reason stays audit
        # metadata in Core and is never sent to the platform.
        return ('set_group_kick', {'group_id': int(group_id), 'user_id': int(user_id),
                                   'reject_add_request': False})

    def _approve_request(self, action):
        approve = action.approve_request
        request_id = identifier(approve.request_id)
        row = self.db.execute('SELECT flag FROM requests WHERE request_id=?',
                              (request_id,)).fetchone()
        if not row:
            # Unknown/expired token: do not guess the platform request.
            raise AdapterError(common.NOT_FOUND)
        # OneBot V11 set_friend_add_request takes flag + approve; reason stays
        # audit metadata in Core and is never sent to the platform.
        return ('set_friend_add_request', {'flag': row[0], 'approve': bool(approve.approve)})

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
                    platform_message = response.get('message_id') if isinstance(response, dict) else None
                    if not (isinstance(platform_message, (str, int))
                            and not isinstance(platform_message, bool)
                            and 0 < len(str(platform_message)) <= 256):
                        platform_message = ''
                    result = terminal(action.action_id, actions.SUCCEEDED, platform_id=str(platform_message))
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
                    if (result.status == actions.SUCCEEDED
                            and api in ('send_group_msg', 'send_private_msg')
                            and result.platform_message_id):
                        conversation = params.get('group_id', params.get('user_id'))
                        self.db.execute('DELETE FROM own_messages WHERE updated < ?',
                                        (self.now() - 7 * 86400,))
                        self.db.execute('INSERT OR REPLACE INTO own_messages VALUES(?,?,?)',
                                        (result.platform_message_id, str(conversation), self.now()))
            return result
        except sqlite3.Error:
            raise AdapterError(common.UPSTREAM_UNAVAILABLE) from None

    def close(self):
        self.db.close()
