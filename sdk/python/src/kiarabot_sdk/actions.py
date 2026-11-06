"""P07 Action client helpers: submission preflight and result interpretation.

The SDK never decides authorization; it only enforces the frozen wire preconditions that a
plugin must satisfy before sending, and interprets ``PluginActionResponse`` snapshots without
turning unknown outcomes into failures. Adapter-side ``action_receipt`` dispositions are not
visible to plugins; they surface as equivalent result shapes (see docs/architecture.md):

- fresh submit  -> ``ACCEPTED`` + ``EXECUTION_NOT_STARTED`` (Runtime RECORDED)
- repeat submit -> the original ``ActionResult`` snapshots (Runtime DUPLICATE)
- late result   -> ``EXPIRED`` + ``EXECUTION_UNKNOWN`` + ``TIMEOUT`` (adapter EVIDENCE_ONLY)
- key/content conflict -> stable ``Error`` ``CONFLICT`` (Runtime CONFLICT)
"""
from __future__ import annotations

import re
import time
import uuid
from datetime import datetime, timedelta, timezone

from google.protobuf.timestamp_pb2 import Timestamp
from kiarabot.v1 import actions_pb2 as actions, common_pb2 as common, platform_pb2 as platform

from .errors import PluginError

ACTION_FEATURE = 'action.result.v1'
MAX_DEADLINE_SECONDS = 300.0
IDENTIFIER = re.compile(r'[A-Za-z0-9._:@-]{1,128}\Z')
UUID4 = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z')

#: Payload oneof name -> P07 ActionKind projection. ``schedule_task`` is legacy and rejected.
ACTION_KINDS = {
    'send_message': actions.ACTION_KIND_SEND_MESSAGE,
    'react_message': actions.ACTION_KIND_REACT_MESSAGE,
    'delete_message': actions.ACTION_KIND_DELETE_MESSAGE,
    'mute_member': actions.ACTION_KIND_MUTE_MEMBER,
    'kick_member': actions.ACTION_KIND_KICK_MEMBER,
    'approve_request': actions.ACTION_KIND_APPROVE_REQUEST,
}
REJECTED_PAYLOADS = frozenset({'schedule_task'})
TERMINAL_STATUSES = frozenset({actions.SUCCEEDED, actions.FAILED, actions.EXPIRED,
                               actions.CANCELLED})
#: status -> allowed execution_outcome (frozen matrix in docs/architecture.md-contracts P07 section).
STATUS_OUTCOMES = {
    actions.ACCEPTED: frozenset({platform.EXECUTION_NOT_STARTED, platform.EXECUTION_UNKNOWN}),
    actions.SUCCEEDED: frozenset({platform.EXECUTION_KNOWN_SUCCEEDED}),
    actions.FAILED: frozenset({platform.EXECUTION_KNOWN_FAILED}),
    actions.EXPIRED: frozenset({platform.EXECUTION_NOT_STARTED, platform.EXECUTION_UNKNOWN}),
    actions.CANCELLED: frozenset({platform.EXECUTION_NOT_STARTED}),
}
#: Outcomes that prove the Action was dispatched to an adapter and must carry its attempt.
DISPATCHED_OUTCOMES = frozenset({platform.EXECUTION_UNKNOWN, platform.EXECUTION_KNOWN_SUCCEEDED,
                                 platform.EXECUTION_KNOWN_FAILED})


def action_kind(action) -> int:
    """ActionKind projection for the envelope payload; legacy/unknown payloads are rejected.

    The payload oneof remains the wire discriminator; this helper only exposes the frozen
    per-kind vocabulary and rejects ``schedule_task`` before any frame is sent.
    """
    payload = action.WhichOneof('payload')
    if payload in REJECTED_PAYLOADS or payload not in ACTION_KINDS:
        raise PluginError(common.CAPABILITY_NOT_SUPPORTED)
    return ACTION_KINDS[payload]


def validate_identifier(value) -> None:
    """Bounded identifier shape used by Action/query/cancel correlation fields."""
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value):
        raise PluginError(common.INVALID_ARGUMENT)


def _timestamp(value: datetime) -> Timestamp:
    if not isinstance(value, datetime) or value.tzinfo is None:
        raise PluginError(common.INVALID_ARGUMENT)
    try:
        result = Timestamp()
        result.FromDatetime(value.astimezone(timezone.utc))
    except (ValueError, OverflowError):
        raise PluginError(common.INVALID_ARGUMENT) from None
    return result


def _validate_deadline(deadline: Timestamp, now: float) -> None:
    remaining = deadline.seconds + deadline.nanos / 1e9 - now
    if not 0 < remaining <= MAX_DEADLINE_SECONDS:
        # Target rule: submit deadline must be future UTC and at most 300 seconds out.
        raise PluginError(common.INVALID_ARGUMENT)


def prepare_submission(action, *, idempotency_key=None, deadline=None, now=None):
    """Validate and normalize a plugin Action submission without mutating the caller's message.

    ``action_id``/``trace_id`` are generated when the caller left them empty, and
    ``idempotency_key``/``deadline`` may be supplied as convenience keywords. Everything the
    frozen contract requires is validated locally: known payload, no Runtime-assigned dispatch
    fields, identifier shapes and a future deadline no more than 300 seconds away.
    """
    value = actions.ActionEnvelope()
    value.CopyFrom(action)
    action_kind(value)  # rejects schedule_task and missing payload
    if value.dispatch_attempt_id or value.dispatch_sequence:
        # dispatch_attempt_id/dispatch_sequence are assigned by Runtime before adapter dispatch.
        raise PluginError(common.INVALID_ARGUMENT)
    if not value.action_id:
        value.action_id = str(uuid.uuid4())
    if not value.trace_id:
        value.trace_id = str(uuid.uuid4())
    if idempotency_key is not None:
        if value.idempotency_key and value.idempotency_key != idempotency_key:
            raise PluginError(common.INVALID_ARGUMENT)
        value.idempotency_key = idempotency_key
    validate_identifier(value.action_id)
    validate_identifier(value.idempotency_key)
    validate_identifier(value.trace_id)
    if value.requested_by:
        validate_identifier(value.requested_by)
    if deadline is not None:
        timestamp = _timestamp(deadline)
        value.deadline.CopyFrom(timestamp)
    if not value.HasField('deadline'):
        # The SDK never invents a business deadline: the contract requires a submitter deadline.
        raise PluginError(common.INVALID_ARGUMENT)
    _validate_deadline(value.deadline, time.time() if now is None else now)
    return value


def _protocol_error() -> PluginError:
    # A malformed or internally inconsistent ActionResult stays an unknown outcome: it must never
    # be surfaced as Action FAILED, because that would fabricate a platform result.
    return PluginError(common.UPSTREAM_UNAVAILABLE, True)


def validate_result(result, *, expected_action_id=None) -> None:
    """Validate the status/execution-outcome/attempt invariants of one ActionResult snapshot."""
    if not isinstance(result, actions.ActionResult):
        raise _protocol_error()
    if not IDENTIFIER.fullmatch(result.action_id or ''):
        raise _protocol_error()
    if expected_action_id is not None and result.action_id != expected_action_id:
        # A response correlated to the wrong Action makes the call outcome unknown.
        raise _protocol_error()
    status, outcome = result.status, int(result.execution_outcome)
    if status not in STATUS_OUTCOMES or outcome not in STATUS_OUTCOMES[status]:
        raise _protocol_error()
    if status == actions.FAILED:
        if not result.HasField('error') or not result.error.code:
            raise _protocol_error()
    elif status == actions.EXPIRED:
        # target rule: EXPIRED carries the stable TIMEOUT error code.
        if not result.HasField('error') or result.error.code != common.TIMEOUT:
            raise _protocol_error()
    elif result.HasField('error'):
        raise _protocol_error()
    if status in TERMINAL_STATUSES and not result.HasField('completed_at'):
        raise _protocol_error()
    attempt = result.dispatch_attempt_id
    if attempt:
        if not UUID4.fullmatch(attempt):
            raise _protocol_error()
    elif outcome in DISPATCHED_OUTCOMES:
        # UNKNOWN/KNOWN_* implies an adapter dispatch attempt; without it the snapshot is not
        # trustworthy enough to expose as a known execution fact.
        raise _protocol_error()


def validate_response(response, expected) -> None:
    """Validate one PluginActionResponse against the expected call kind/action correlation."""
    kind = expected[0] if isinstance(expected, tuple) else None
    expected_action_id = expected[1] if isinstance(expected, tuple) else None
    outcome = response.WhichOneof('outcome')
    if outcome == 'error':
        if not response.error.code:
            raise _protocol_error()
        return
    if outcome != 'result':
        raise _protocol_error()
    if kind == 'submit':
        # Duplicate submissions may return the original action_id; only validate the snapshot.
        validate_result(response.result)
    elif kind in ('query', 'cancel'):
        validate_result(response.result, expected_action_id=expected_action_id)
    else:
        validate_result(response.result)


def is_terminal(result) -> bool:
    return result.status in TERMINAL_STATUSES


def is_unknown(result) -> bool:
    """True when the platform outcome is unknown (ACCEPTED/EXPIRED + EXECUTION_UNKNOWN)."""
    return result.execution_outcome == platform.EXECUTION_UNKNOWN


def may_have_executed(result) -> bool:
    """Unknown after dispatch may have side effects; callers must query, never resubmit blindly."""
    return is_unknown(result)
