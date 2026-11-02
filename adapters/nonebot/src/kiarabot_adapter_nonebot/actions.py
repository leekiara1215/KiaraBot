from __future__ import annotations

import asyncio
import hashlib
import sqlite3
import time
from dataclasses import dataclass

from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common, platform_pb2 as platform
from .bridge import valid_uuid4
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

# Payload kind to negotiated target capability. The Runtime only accepts an Action kind the
# adapter declared and that registration returned in ``effective_capabilities``.
CAPABILITY_BY_PAYLOAD = {
    'send_message': 'action.send_message.text.v1',
    'react_message': 'action.react_message.v1',
    'delete_message': 'action.delete_message.v1',
    'mute_member': 'action.mute_member.v1',
    'kick_member': 'action.kick_member.v1',
    'approve_request': 'action.approve_request.v1',
}
ACTION_CAPABILITIES = tuple(CAPABILITY_BY_PAYLOAD.values())

# Target P07 execution-knownness pairing. Terminal results only carry KNOWN_*; NOT_STARTED and
# UNKNOWN are non-terminal evidence reported through AdapterToCore.action_evidence.
KNOWN_OUTCOME = {
    actions.SUCCEEDED: platform.EXECUTION_KNOWN_SUCCEEDED,
    actions.FAILED: platform.EXECUTION_KNOWN_FAILED,
}

# Local journal states. ``claimed`` is written before any platform I/O and is converted to
# ``unknown`` on process restart: the journal proves a dispatch attempt existed, not whether the
# platform call happened.
CLAIMED, TERMINAL, UNKNOWN, NOT_STARTED = 'claimed', 'terminal', 'unknown', 'not_started'

# action_reports states: only UNSENT rows are retransmitted. REPORTED means Runtime acknowledged
# the report; CONFLICT means Runtime rejected correlation and the adapter stops resending that
# attempt; SUPERSEDED means a newer dispatch attempt replaced it before acknowledgement.
REPORT_UNSENT, REPORT_REPORTED, REPORT_CONFLICT, REPORT_SUPERSEDED = 0, 1, 2, 3

SCHEMA_VERSION = 4


class ActionReport:
    """One persisted result/evidence report awaiting (or just receiving) a Runtime receipt."""

    __slots__ = ('action_id', 'dispatch_attempt_id', 'dispatch_sequence', 'kind', 'result',
                 'evidence', 'send')

    def __init__(self, action_id, dispatch_attempt_id, dispatch_sequence, kind, *, result=None,
                 evidence=None, send=True):
        self.action_id = action_id
        self.dispatch_attempt_id = dispatch_attempt_id
        self.dispatch_sequence = dispatch_sequence
        self.kind = kind
        self.result = result
        self.evidence = evidence
        self.send = send

    @property
    def key(self):
        return (self.action_id, self.dispatch_attempt_id)

    def __repr__(self):
        return (f'ActionReport(action_id={self.action_id!r}, kind={self.kind!r}, '
                f'attempt={self.dispatch_attempt_id!r}, send={self.send})')


def terminal(action_id, status, code=0, platform_id='', dispatch_attempt_id=''):
    """Terminal ActionResult with the target execution-knownness pairing."""
    if status not in KNOWN_OUTCOME:
        raise AdapterError(common.INTERNAL)
    result = actions.ActionResult(action_id=action_id, status=status,
                                  platform_message_id=platform_id,
                                  dispatch_attempt_id=dispatch_attempt_id,
                                  execution_outcome=KNOWN_OUTCOME[status])
    result.completed_at.GetCurrentTime()
    if code:
        result.error.CopyFrom(common.Error(code=code, message=common.ErrorCode.Name(code),
                                           request_id=action_id))
    return result


def evidence(action_id, dispatch_attempt_id, outcome, code=0, platform_reference=''):
    """Non-terminal evidence: NOT_STARTED (received, not executed) or UNKNOWN (outcome lost)."""
    if outcome not in (platform.EXECUTION_NOT_STARTED, platform.EXECUTION_UNKNOWN):
        raise AdapterError(common.INTERNAL)
    result = actions.ActionExecutionEvidence(action_id=action_id,
                                             dispatch_attempt_id=dispatch_attempt_id,
                                             execution_outcome=outcome,
                                             platform_reference=platform_reference[:256])
    result.observed_at.GetCurrentTime()
    if code:
        result.last_error.CopyFrom(common.Error(code=code, message=common.ErrorCode.Name(code),
                                                request_id=action_id))
    return result


class ActionExecutor:
    """Single event-loop owner of the durable action journal and platform execution.

    Target P07: persist the dispatch attempt before platform I/O, report a terminal
    ``action_result`` only for known outcomes, report ``action_evidence`` for NOT_STARTED/UNKNOWN,
    and retransmit unacknowledged reports after reconnect. A repeated action_id+content never
    calls the platform twice; unknown platform outcomes are never replayed.
    """

    def __init__(self, path, adapter_id, account_id, call_platform, now=time.time, limit=10000,
                 timeout=5.0):
        self.adapter_id, self.account_id, self.call_platform = adapter_id, account_id, call_platform
        self.now, self.limit, self.timeout = now, limit, timeout
        # None means the reference unit tests did not negotiate; the target bridge always
        # configures the registration's effective capability set.
        self.capabilities: frozenset | None = None
        self.instance: str | None = None
        self.db = sqlite3.connect(path)
        try:
            version = self.db.execute('PRAGMA user_version').fetchone()[0]
            if version not in (0, 1, 2, 3, SCHEMA_VERSION):
                raise AdapterError(common.INTERNAL)
            self.db.execute('PRAGMA synchronous=FULL')
            with self.db:
                self._create_schema(version)
            self.recover()
        except Exception:
            self.db.close()
            raise

    def _create_schema(self, version):
        """v3 action log -> v4 dispatch journal + per-attempt report queue; legacy log preserved."""
        if version and version < SCHEMA_VERSION:
            columns = [row[1] for row in self.db.execute('PRAGMA table_info(actions)')]
            if 'terminal' in columns:
                self.db.execute('ALTER TABLE actions RENAME TO actions_legacy')
                self._create_tables()
                self.db.execute(
                    'INSERT INTO actions(id,fingerprint,state,result,evidence,attempt_id,sequence,updated) '
                    "SELECT id,fingerprint,"
                    "CASE WHEN terminal=1 THEN 'terminal' ELSE 'unknown' END,"
                    "CASE WHEN terminal=1 THEN result ELSE NULL END,NULL,'',0,updated "
                    'FROM actions_legacy')
                self.db.execute('DROP TABLE actions_legacy')
        self._create_tables()
        self.db.execute('CREATE TABLE IF NOT EXISTS own_messages (message_id TEXT PRIMARY KEY, '
                        'conversation TEXT NOT NULL, updated REAL NOT NULL)')
        self.db.execute('CREATE TABLE IF NOT EXISTS requests (request_id TEXT PRIMARY KEY, '
                        'flag TEXT NOT NULL, updated REAL NOT NULL)')
        self.db.execute(f'PRAGMA user_version={SCHEMA_VERSION}')

    def _create_tables(self):
        # Only messages this bot sent are eligible for DeleteMessage; the platform message ID
        # alone is never authorization.
        self.db.execute('CREATE TABLE IF NOT EXISTS actions (id TEXT PRIMARY KEY, '
                        'fingerprint TEXT NOT NULL, state TEXT NOT NULL, result BLOB, '
                        'evidence BLOB, attempt_id TEXT NOT NULL DEFAULT "", '
                        'sequence INTEGER NOT NULL DEFAULT 0, updated REAL NOT NULL)')
        # One report row per (action, dispatch attempt): a re-dispatch with a new attempt reports
        # the known result again instead of executing the platform call again.
        self.db.execute('CREATE TABLE IF NOT EXISTS action_reports (action_id TEXT NOT NULL, '
                        'attempt_id TEXT NOT NULL, sequence INTEGER NOT NULL, kind TEXT NOT NULL, '
                        'payload BLOB NOT NULL, state INTEGER NOT NULL, receipt BLOB, '
                        'updated REAL NOT NULL, PRIMARY KEY(action_id, attempt_id))')

    def configure(self, instance, capabilities):
        """Apply target registration negotiation: fence instance and effective capabilities."""
        self.instance = instance
        self.capabilities = frozenset(capabilities)

    def remember_request(self, request_id, flag):
        """Records the platform flag for an emitted FriendRequest token."""
        identifier(request_id)
        identifier(flag)
        with self.db:
            self.db.execute('DELETE FROM requests WHERE updated < ?', (self.now() - 7 * 86400,))
            self.db.execute('INSERT OR REPLACE INTO requests VALUES(?,?,?)',
                            (request_id, flag, self.now()))

    # ---- dispatch correlation / journal -------------------------------------------------
    @staticmethod
    def dispatch_fields(action):
        """Target dispatch identity is Runtime-assigned; absent/garbled fields never execute."""
        attempt = action.dispatch_attempt_id
        sequence = int(action.dispatch_sequence)
        if not valid_uuid4(attempt) or sequence < 1:
            raise AdapterError(common.INTERNAL)
        return attempt, sequence

    @staticmethod
    def fingerprint(action) -> str:
        """Content hash: excludes Runtime-filled attempt/sequence and connection instance hint."""
        canonical = actions.ActionEnvelope()
        canonical.CopyFrom(action)
        canonical.dispatch_attempt_id = ''
        canonical.dispatch_sequence = 0
        canonical.target_source.adapter_instance_id = ''
        return hashlib.sha256(canonical.SerializeToString(deterministic=True)).hexdigest()

    def _lookup(self, action_id):
        return self.db.execute('SELECT fingerprint,state,result,evidence FROM actions WHERE id=?',
                               (action_id,)).fetchone()

    def recover(self):
        """Process restart during a platform attempt: outcome is unknown, never replay it."""
        rows = self.db.execute("SELECT id,attempt_id,sequence FROM actions WHERE state='claimed'"
                               ).fetchall()
        if not rows:
            return
        with self.db:
            for action_id, attempt, sequence in rows:
                item = evidence(action_id, attempt, platform.EXECUTION_UNKNOWN,
                                common.INTERNAL)
                self.db.execute("UPDATE actions SET state='unknown', evidence=?, updated=? "
                                'WHERE id=?', (item.SerializeToString(), self.now(), action_id))
                if attempt and sequence:
                    self._store_report_locked(action_id, attempt, sequence, 'evidence',
                                              item.SerializeToString())

    def _store_report_locked(self, action_id, attempt, sequence, kind, payload):
        # A newer dispatch attempt supersedes unsent reports for older attempts.
        self.db.execute('UPDATE action_reports SET state=? WHERE action_id=? '
                        'AND attempt_id<>? AND state=?',
                        (REPORT_SUPERSEDED, action_id, attempt, REPORT_UNSENT))
        row = self.db.execute('SELECT state FROM action_reports WHERE action_id=? '
                              'AND attempt_id=?', (action_id, attempt)).fetchone()
        if row is not None and row[0] != REPORT_UNSENT:
            # Already acknowledged/conflicted: a duplicate dispatch must not resurrect it.
            self.db.execute('UPDATE action_reports SET sequence=?,kind=?,payload=?,updated=? '
                            'WHERE action_id=? AND attempt_id=?',
                            (sequence, kind, payload, self.now(), action_id, attempt))
            return row[0]
        self.db.execute('INSERT OR REPLACE INTO action_reports '
                        '(action_id,attempt_id,sequence,kind,payload,state,receipt,updated) '
                        'VALUES(?,?,?,?,?,?,NULL,?)',
                        (action_id, attempt, sequence, kind, payload, REPORT_UNSENT, self.now()))
        return REPORT_UNSENT

    def _report(self, action_id, attempt, sequence, kind, *, result=None, item=None,
                send=True):
        if kind == 'result':
            state = self._store_report_locked(action_id, attempt, sequence, kind,
                                              result.SerializeToString())
            return ActionReport(action_id, attempt, sequence, 'result', result=result,
                                send=send and state == REPORT_UNSENT)
        state = self._store_report_locked(action_id, attempt, sequence, kind,
                                          item.SerializeToString())
        return ActionReport(action_id, attempt, sequence, 'evidence', evidence=item,
                            send=send and state == REPORT_UNSENT)

    def _finish(self, action_id, attempt, sequence, state, *, result=None, item=None):
        with self.db:
            self.db.execute('UPDATE actions SET state=?,result=?,evidence=?,attempt_id=?,'
                            'sequence=?,updated=? WHERE id=?',
                            (state, result.SerializeToString() if result is not None else None,
                             item.SerializeToString() if item is not None else None,
                             attempt, sequence, self.now(), action_id))
        if result is not None:
            return self._report(action_id, attempt, sequence, 'result', result=result)
        return self._report(action_id, attempt, sequence, 'evidence', item=item)

    def _known_failed(self, action, fingerprint, attempt, sequence, code):
        """Precheck rejection is a known failure: no platform call and no retry of the effect."""
        result = terminal(action.action_id, actions.FAILED, code, dispatch_attempt_id=attempt)
        with self.db:
            self.db.execute('INSERT INTO actions(id,fingerprint,state,result,evidence,attempt_id,'
                            'sequence,updated) VALUES(?,?,?,?,NULL,?,?,?)',
                            (action.action_id, fingerprint, TERMINAL,
                             result.SerializeToString(), attempt, sequence, self.now()))
        return self._report(action.action_id, attempt, sequence, 'result', result=result)

    def _replay(self, row, action, attempt, sequence):
        fingerprint, state, result_blob, evidence_blob = row
        if state == TERMINAL:
            result = actions.ActionResult.FromString(result_blob)
            # The known outcome is re-correlated with the latest dispatch attempt; the platform
            # is not called again.
            result.dispatch_attempt_id = attempt
            return self._report(action.action_id, attempt, sequence, 'result', result=result)
        if evidence_blob:
            item = actions.ActionExecutionEvidence.FromString(evidence_blob)
            item.dispatch_attempt_id = attempt
        else:
            # Legacy v3 row without a recorded attempt: outcome is still unknown.
            item = evidence(action.action_id, attempt, platform.EXECUTION_UNKNOWN,
                            common.INTERNAL)
        return self._report(action.action_id, attempt, sequence, 'evidence', item=item)

    def _not_started(self, action, attempt, sequence):
        item = evidence(action.action_id, attempt, platform.EXECUTION_NOT_STARTED,
                        common.TIMEOUT)
        with self.db:
            self.db.execute('UPDATE actions SET state=?,evidence=?,attempt_id=?,sequence=?,'
                            'updated=? WHERE id=?',
                            (NOT_STARTED, item.SerializeToString(), attempt, sequence,
                             self.now(), action.action_id))
            return self._report(action.action_id, attempt, sequence, 'evidence', item=item)

    def _unknown(self, action, attempt, sequence, code):
        item = evidence(action.action_id, attempt, platform.EXECUTION_UNKNOWN, code)
        with self.db:
            self.db.execute('UPDATE actions SET state=?,evidence=?,attempt_id=?,sequence=?,'
                            'updated=? WHERE id=?',
                            (UNKNOWN, item.SerializeToString(), attempt, sequence, self.now(),
                             action.action_id))
            return self._report(action.action_id, attempt, sequence, 'evidence', item=item)

    async def execute(self, action, instance):
        """Execute one dispatched Action, returning the report to send for this attempt."""
        attempt, sequence = self.dispatch_fields(action)
        fingerprint = self.fingerprint(action)
        row = self._lookup(action.action_id)
        if row:
            if row[0] != fingerprint:
                # Same action id with different content is a contract conflict; never execute it.
                conflict = terminal(action.action_id, actions.FAILED, common.CONFLICT,
                                    dispatch_attempt_id=attempt)
                return self._report(action.action_id, attempt, sequence, 'result',
                                    result=conflict)
            if row[1] == CLAIMED:
                # In-process claim cannot be concurrent (single event-loop owner); treat as a
                # recovered unknown outcome rather than executing again.
                return self._unknown(action, attempt, sequence, common.INTERNAL)
            return self._replay(row, action, attempt, sequence)
        try:
            api, params = self.validate(action, instance)
        except AdapterError as failure:
            return self._known_failed(action, fingerprint, attempt, sequence, failure.code)
        self._claim(action, fingerprint, attempt, sequence)
        remaining = action.deadline.seconds + action.deadline.nanos / 1e9 - self.now()
        if remaining <= 0:
            return self._not_started(action, attempt, sequence)
        try:
            response = await asyncio.wait_for(self.call_platform(api, params),
                                              min(remaining, self.timeout))
            platform_message = response.get('message_id') if isinstance(response, dict) else None
            if not (isinstance(platform_message, (str, int))
                    and not isinstance(platform_message, bool)
                    and 0 < len(str(platform_message)) <= 256):
                platform_message = ''
            result = terminal(action.action_id, actions.SUCCEEDED, platform_id=str(platform_message),
                              dispatch_attempt_id=attempt)
            report = self._finish(action.action_id, attempt, sequence, TERMINAL, result=result)
            with self.db:
                if (api in ('send_group_msg', 'send_private_msg')
                        and result.platform_message_id):
                    conversation = params.get('group_id', params.get('user_id'))
                    self.db.execute('DELETE FROM own_messages WHERE updated < ?',
                                    (self.now() - 7 * 86400,))
                    self.db.execute('INSERT OR REPLACE INTO own_messages VALUES(?,?,?)',
                                    (result.platform_message_id, str(conversation), self.now()))
            return report
        except AdapterError as failure:
            # Only an explicit platform rejection is a known failure.
            result = terminal(action.action_id, actions.FAILED, failure.code,
                              dispatch_attempt_id=attempt)
            return self._finish(action.action_id, attempt, sequence, TERMINAL, result=result)
        except asyncio.CancelledError:
            # Shutdown/transport cancellation after the platform call started: persist the
            # unknown report, then propagate so the caller can stop promptly.
            self._unknown(action, attempt, sequence, common.UPSTREAM_UNAVAILABLE)
            raise
        except Exception:
            # Timeout/disconnect may have happened after the platform effect: never retry it.
            return self._unknown(action, attempt, sequence, common.UPSTREAM_UNAVAILABLE)

    def _claim(self, action, fingerprint, attempt, sequence):
        try:
            with self.db:
                self.db.execute('DELETE FROM actions WHERE state=? AND updated < ?',
                                (TERMINAL, self.now() - 7 * 86400))
                count, size = self.db.execute(
                    'SELECT count(*),coalesce(sum(length(result)),0) FROM actions').fetchone()
                if count >= self.limit or size > 64 * 1024 * 1024:
                    raise AdapterError(common.RATE_LIMITED)
                self.db.execute('INSERT INTO actions(id,fingerprint,state,result,evidence,'
                                'attempt_id,sequence,updated) VALUES(?,?,?,NULL,NULL,?,?,?)',
                                (action.action_id, fingerprint, CLAIMED, attempt, sequence,
                                 self.now()))
        except sqlite3.Error:
            raise AdapterError(common.UPSTREAM_UNAVAILABLE) from None

    # ---- reports / receipts -------------------------------------------------------------
    @staticmethod
    def _report_from_row(action_id, attempt, sequence, kind, payload):
        if kind == 'result':
            return ActionReport(action_id, attempt, sequence, 'result',
                                result=actions.ActionResult.FromString(payload))
        return ActionReport(action_id, attempt, sequence, 'evidence',
                            evidence=actions.ActionExecutionEvidence.FromString(payload))

    def pending_reports(self, limit=256, offset=0):
        """Unacknowledged reports in journal order; resent after reconnect and on startup."""
        rows = self.db.execute(
            'SELECT action_id,attempt_id,sequence,kind,payload FROM action_reports '
            'WHERE state=? ORDER BY updated, rowid LIMIT ? OFFSET ?',
            (REPORT_UNSENT, limit, offset)).fetchall()
        return [self._report_from_row(*row) for row in rows]

    def acknowledge(self, receipt) -> str:
        """Apply one Runtime receipt to a stored report; unknown/stale receipts are ignored."""
        disposition = receipt.disposition
        if disposition not in (actions.ACTION_RECEIPT_RECORDED, actions.ACTION_RECEIPT_DUPLICATE,
                               actions.ACTION_RECEIPT_EVIDENCE_ONLY,
                               actions.ACTION_RECEIPT_CONFLICT):
            return 'invalid'
        row = self.db.execute('SELECT state FROM action_reports WHERE action_id=? AND attempt_id=?',
                              (receipt.action_id, receipt.dispatch_attempt_id)).fetchone()
        if not row:
            return 'unknown'
        if disposition == actions.ACTION_RECEIPT_CONFLICT:
            with self.db:
                self.db.execute('UPDATE action_reports SET state=?,receipt=?,updated=? '
                                'WHERE action_id=? AND attempt_id=?',
                                (REPORT_CONFLICT, receipt.SerializeToString(), self.now(),
                                 receipt.action_id, receipt.dispatch_attempt_id))
            return 'conflict'
        with self.db:
            self.db.execute('UPDATE action_reports SET state=?,receipt=?,updated=? '
                            'WHERE action_id=? AND attempt_id=?',
                            (REPORT_REPORTED, receipt.SerializeToString(), self.now(),
                             receipt.action_id, receipt.dispatch_attempt_id))
        return 'reported'

    # ---- payload validation / platform mapping ------------------------------------------
    def validate(self, action, instance):
        identifier(action.action_id)
        identifier(action.idempotency_key)
        if (action.target_source.adapter_id != self.adapter_id
                or action.target_source.account_id != self.account_id
                or (action.target_source.adapter_instance_id
                    and action.target_source.adapter_instance_id != instance)):
            # Target fencing is carried by the frame Fence; a non-empty instance hint must still
            # agree with the registered adapter instance.
            raise AdapterError(common.PERMISSION_DENIED)
        if not 0 < action.deadline.seconds <= 9223372035 or not 0 <= action.deadline.nanos < 10**9:
            raise AdapterError(common.INVALID_ARGUMENT)
        payload = action.WhichOneof('payload')
        if self.capabilities is not None:
            capability = CAPABILITY_BY_PAYLOAD.get(payload)
            if capability is None or capability not in self.capabilities:
                raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
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
                or send.conversation.type not in (common.ConversationRef.GROUP,
                                                  common.ConversationRef.PRIVATE)):
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        # Native OneBot numeric ID, identical to the inbound MessageReceived conversation.
        conversation = platform_id(send.conversation.id)
        if (send.conversation.account_id != self.account_id
                or action.target.conversation_id != conversation
                or not send.message.plain_text
                or len(send.message.plain_text.encode()) > 65536):
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
        if react.conversation.type not in (common.ConversationRef.GROUP,
                                           common.ConversationRef.PRIVATE):
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        conversation = platform_id(react.conversation.id)
        message_id = platform_id(react.message_id)
        if (react.conversation.account_id != self.account_id
                or action.target.conversation_id != conversation):
            raise AdapterError(common.INVALID_ARGUMENT)
        return ('set_msg_emoji_like', {'message_id': int(message_id),
                                       'emoji_id': REACTIONS[react.reaction]})

    def _delete_message(self, action):
        deletion = action.delete_message
        if deletion.conversation.type not in (common.ConversationRef.GROUP,
                                              common.ConversationRef.PRIVATE):
            raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
        conversation = platform_id(deletion.conversation.id)
        message_id = platform_id(deletion.message_id)
        if (deletion.conversation.account_id != self.account_id
                or action.target.conversation_id != conversation):
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

    def close(self):
        self.db.close()
