from __future__ import annotations

import hashlib
import json
import sqlite3
import time
from pathlib import Path

from kiarabot.v1 import common_pb2 as common, events_pb2 as events
from .mapper import AdapterError

SCHEMA_VERSION = 2


def _conversation_type(event: events.EventEnvelope) -> int:
    """Conversation type carried by the payload, or type 0 when there is none.

    MessageReceived/MessageSent expose an explicit ConversationRef; status and other
    non-conversation payloads have no conversation to distinguish.
    """
    payload = event.WhichOneof('payload')
    if payload == 'message_received':
        return event.message_received.conversation.type
    if payload == 'message_sent':
        return event.message_sent.conversation.type
    return common.ConversationRef.TYPE_UNSPECIFIED


def _session_key(adapter_id: str, account_id: str, conversation_id: str, conversation_type: int) -> str:
    return json.dumps([adapter_id, account_id, conversation_id, conversation_type])


def _migrate_sessions_v1_to_v2(db) -> None:
    """Rewrite 3-element v1 session keys as 4-element keys carrying the type.

    A v1 key counted group and private conversations with the same native ID as one
    session, so its sequence is conservatively copied to both types (the watermark can
    only over-estimate, never report out of order); empty conversations move to type 0.
    Malformed rows abort the surrounding transaction instead of partially migrating.
    """
    migrated: dict[str, int] = {}
    for key, sequence in db.execute('SELECT key, sequence FROM sessions').fetchall():
        try:
            parts = json.loads(key)
        except (TypeError, ValueError):
            raise AdapterError(common.INTERNAL) from None
        if (not isinstance(parts, list) or len(parts) != 3
                or not all(isinstance(part, str) for part in parts)
                or isinstance(sequence, bool) or not isinstance(sequence, int) or sequence < 0):
            raise AdapterError(common.INTERNAL)
        adapter_id, account_id, conversation_id = parts
        types = ((common.ConversationRef.GROUP, common.ConversationRef.PRIVATE) if conversation_id
                 else (common.ConversationRef.TYPE_UNSPECIFIED,))
        for conversation_type in types:
            new_key = _session_key(adapter_id, account_id, conversation_id, conversation_type)
            migrated[new_key] = max(migrated.get(new_key, 0), sequence)
    db.execute('DELETE FROM sessions')
    db.executemany('INSERT INTO sessions(key,sequence) VALUES(?,?)', migrated.items())


class Outbox:
    """Single event-loop owner; separate database from Core, bounded durable spool."""

    def __init__(self, path: str | Path, limit: int = 10000, now=time.time):
        self.limit, self.now = limit, now
        self.db = sqlite3.connect(path)
        try:
            version = self.db.execute('PRAGMA user_version').fetchone()[0]
            if version not in (0, 1, SCHEMA_VERSION):
                raise AdapterError(common.INTERNAL)
            self.db.execute('PRAGMA synchronous=FULL')
            with self.db:
                self.db.execute('CREATE TABLE IF NOT EXISTS sessions (key TEXT PRIMARY KEY, sequence INTEGER NOT NULL)')
                self.db.execute('CREATE TABLE IF NOT EXISTS outbox (position INTEGER PRIMARY KEY AUTOINCREMENT, '
                                'id TEXT NOT NULL UNIQUE, fingerprint TEXT NOT NULL, payload BLOB NOT NULL, '
                                "status TEXT NOT NULL DEFAULT 'pending', error INTEGER NOT NULL DEFAULT 0, updated REAL NOT NULL)")
                if version == 1:
                    _migrate_sessions_v1_to_v2(self.db)
                self.db.execute(f'PRAGMA user_version={SCHEMA_VERSION}')
        except Exception:
            self.db.close()
            raise

    def enqueue(self, event: events.EventEnvelope) -> str:
        canonical = events.EventEnvelope()
        canonical.CopyFrom(event)
        canonical.source.adapter_instance_id = ''
        canonical.sequence = 0
        fingerprint = hashlib.sha256(canonical.SerializeToString(deterministic=True)).hexdigest()
        key = _session_key(event.source.adapter_id, event.source.account_id,
                           event.subject.conversation_id, _conversation_type(event))
        try:
            with self.db:
                self.db.execute('BEGIN IMMEDIATE')
                previous = self.db.execute('SELECT fingerprint FROM outbox WHERE id=?', (event.event_id,)).fetchone()
                if previous:
                    if previous[0] != fingerprint:
                        raise AdapterError(common.CONFLICT)
                    return event.event_id
                # Terminal (acked/failed) records are diagnostics/dedup indexes, not
                # backlog: they expire after seven days and never consume the pending
                # limit. A hard row cap bounds dedup growth under extreme load.
                self.db.execute("DELETE FROM outbox WHERE status IN ('acked','failed') AND updated < ?",
                                (self.now() - 7 * 86400,))
                pending, size = self.db.execute(
                    "SELECT count(*),coalesce(sum(length(payload)),0) FROM outbox WHERE status='pending'").fetchone()
                if pending >= self.limit or size + canonical.ByteSize() > 64 * 1024 * 1024:
                    raise AdapterError(common.RATE_LIMITED)
                total = self.db.execute('SELECT count(*) FROM outbox').fetchone()[0]
                if total >= self.limit * 4:
                    self.db.execute(
                        "DELETE FROM outbox WHERE position IN (SELECT position FROM outbox "
                        "WHERE status IN ('acked','failed') ORDER BY position LIMIT ?)",
                        (total - self.limit * 4 + 1,))
                current = self.db.execute('SELECT sequence FROM sessions WHERE key=?', (key,)).fetchone()
                sequence = (current[0] if current else 0) + 1
                if sequence >= 2**63:
                    raise AdapterError(common.INTERNAL)
                canonical.sequence = sequence
                if canonical.ByteSize() > 1_000_000:
                    raise AdapterError(common.INVALID_ARGUMENT)
                self.db.execute('INSERT INTO sessions VALUES(?,?) ON CONFLICT(key) DO UPDATE SET sequence=excluded.sequence',
                                (key, sequence))
                self.db.execute('INSERT INTO outbox(id,fingerprint,payload,updated) VALUES(?,?,?,?)',
                                (event.event_id, fingerprint, canonical.SerializeToString(), self.now()))
            return event.event_id
        except sqlite3.Error:
            raise AdapterError(common.UPSTREAM_UNAVAILABLE) from None

    def next_pending(self) -> events.EventEnvelope | None:
        try:
            row = self.db.execute("SELECT payload FROM outbox WHERE status='pending' ORDER BY position LIMIT 1").fetchone()
            return events.EventEnvelope.FromString(row[0]) if row else None
        except sqlite3.Error:
            raise AdapterError(common.UPSTREAM_UNAVAILABLE) from None

    def finish(self, event_id: str, error: int = 0) -> None:
        try:
            with self.db:
                # Failed records retain their payload for diagnosis/recovery. ACKed
                # records keep only their fingerprint for bounded duplicate suppression.
                self.db.execute("UPDATE outbox SET status=?,error=?,updated=?,payload=CASE WHEN ?=0 THEN X'' ELSE payload END "
                                "WHERE id=? AND status='pending'",
                                ('failed' if error else 'acked', error, self.now(), error, event_id))
        except sqlite3.Error:
            raise AdapterError(common.UPSTREAM_UNAVAILABLE) from None

    def status(self, event_id: str) -> tuple[str, int] | None:
        row = self.db.execute('SELECT status,error FROM outbox WHERE id=?', (event_id,)).fetchone()
        return tuple(row) if row else None

    def close(self) -> None:
        self.db.close()
