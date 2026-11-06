"""P10c plugin task client helpers: frozen HostRequest/TaskResponse validation.

The frozen ``PluginBridge`` host surface exposes exactly three task operations —
``task_create``, ``task_get`` and ``task_cancel``. ``SchedulerService.ListTasks`` is a
Control/Runtime wire RPC and intentionally has no plugin bridge frame, so the SDK has no
``list_tasks`` method (docs/architecture.md: "list 未实现，协议无列表操作").

Validation is preflight only: the Scheduler remains authoritative for future deadlines,
DST/zone resolution, cancel windows, ownership and capacity. The SDK never converts local
civil time and never retries side-effecting task calls.
"""
from __future__ import annotations

import re
from datetime import datetime, timezone
from typing import Mapping

from google.protobuf.timestamp_pb2 import Timestamp
from kiarabot.v1 import common_pb2 as common, plugins_pb2 as plugins

from .errors import PluginError

TASK_FEATURE = 'host.tasks.v1'
TASK_PERMISSION = 'task.schedule'
TIMER_CAPABILITY = 'event.timer.triggered'
MAX_TASK_ID = 128
MAX_IDEMPOTENCY_KEY = 128
MAX_TARGET_BYTES = 256
MAX_PAYLOAD_BYTES = 64 * 1024
#: Contract target identifiers: ASCII letters/digits plus dot, dash and underscore.
IDENTIFIER = re.compile(r'[A-Za-z0-9._-]{1,128}\Z')
ZONE = re.compile(r'[A-Za-z0-9_+.\-]{1,64}(?:/[A-Za-z0-9_+.\-]{1,64}){0,3}\Z')
LOCAL_TIME_FIELDS = ('year', 'month', 'day', 'hour', 'minute', 'second', 'time_zone')
#: Terminal ScheduledTaskState values. COMPLETED only proves the TimerTriggered event was
#: durably enqueued, not that any plugin callback succeeded.
TERMINAL_STATES = frozenset({common.SCHEDULED_TASK_COMPLETED, common.SCHEDULED_TASK_FAILED,
                             common.SCHEDULED_TASK_CANCELLED})
#: States that prove a trigger intent/occurrence exists, so cancellation must be CONFLICT.
HANDED_OFF_STATES = frozenset({common.SCHEDULED_TASK_RUNNING, common.SCHEDULED_TASK_RETRYING,
                               common.SCHEDULED_TASK_COMPLETED, common.SCHEDULED_TASK_FAILED})


def validate_task_id(task_id) -> str:
    if not isinstance(task_id, str) or not IDENTIFIER.fullmatch(task_id):
        raise PluginError(common.INVALID_ARGUMENT)
    return task_id


def validate_idempotency_key(key) -> str:
    if not isinstance(key, str) or not IDENTIFIER.fullmatch(key):
        raise PluginError(common.INVALID_ARGUMENT)
    return key


def validate_target_conversation(value) -> str:
    if not isinstance(value, str) or not value:
        raise PluginError(common.INVALID_ARGUMENT)
    if len(value.encode('utf-8')) > MAX_TARGET_BYTES:
        raise PluginError(common.INVALID_ARGUMENT)
    return value


def validate_payload(value) -> bytes:
    if not isinstance(value, bytes) or len(value) > MAX_PAYLOAD_BYTES:
        raise PluginError(common.INVALID_ARGUMENT)
    return value


def _utc_timestamp(value) -> Timestamp:
    """Accepted run_at input is an aware UTC ``datetime`` only; the SDK never converts zones."""
    if (not isinstance(value, datetime) or value.tzinfo is None
            or value.utcoffset() != timezone.utc.utcoffset(value)):
        raise PluginError(common.INVALID_ARGUMENT)
    if value <= datetime.now(timezone.utc):
        # The Scheduler re-checks against DB now; this only rejects obviously past callers.
        raise PluginError(common.INVALID_ARGUMENT)
    result = Timestamp()
    result.FromDatetime(value)
    return result


def _field_int(value, minimum, maximum) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise PluginError(common.INVALID_ARGUMENT)
    return value


def _local_time(value) -> common.LocalTaskTime:
    """Validate civil fields/zone shape without resolving DST; the Scheduler decides gap/fold."""
    if isinstance(value, common.LocalTaskTime):
        source = {name: getattr(value, name) for name in LOCAL_TIME_FIELDS}
    elif isinstance(value, Mapping):
        if set(value) - set(LOCAL_TIME_FIELDS) or set(value) != set(LOCAL_TIME_FIELDS):
            raise PluginError(common.INVALID_ARGUMENT)
        source = dict(value)
    else:
        raise PluginError(common.INVALID_ARGUMENT)
    result = common.LocalTaskTime(
        year=_field_int(source['year'], 1970, 2100),
        month=_field_int(source['month'], 1, 12),
        day=_field_int(source['day'], 1, 31),
        hour=_field_int(source['hour'], 0, 23),
        minute=_field_int(source['minute'], 0, 59),
        second=_field_int(source['second'], 0, 59),
    )
    # Civil calendar validity is local-only; DST ambiguity stays Scheduler-owned.
    try:
        datetime(result.year, result.month, result.day)
    except ValueError:
        raise PluginError(common.INVALID_ARGUMENT) from None
    zone = source['time_zone']
    if not isinstance(zone, str) or not ZONE.fullmatch(zone) or '..' in zone:
        raise PluginError(common.INVALID_ARGUMENT)
    result.time_zone = zone
    return result


def create_request(task_id, target_conversation, payload, *, run_at=None, local_time=None):
    """Build a validated TaskCreateRequest with exactly one time form."""
    if (run_at is None) == (local_time is None):
        raise PluginError(common.INVALID_ARGUMENT)
    request = plugins.TaskCreateRequest(
        task_id=validate_task_id(task_id),
        target_conversation=validate_target_conversation(target_conversation),
        payload=validate_payload(payload))
    if run_at is not None:
        request.run_at.CopyFrom(_utc_timestamp(run_at))
    else:
        request.local_time.CopyFrom(_local_time(local_time))
    return request


def get_request(task_id):
    return plugins.TaskGetRequest(task_id=validate_task_id(task_id))


def cancel_request(task_id):
    return plugins.TaskCancelRequest(task_id=validate_task_id(task_id))


def validate_response(response, expected_task_id) -> plugins.TaskResponse:
    """A task response must echo the requested identity and carry a known state."""
    if not isinstance(response, plugins.TaskResponse):
        raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
    if response.task_id != expected_task_id or not IDENTIFIER.fullmatch(response.task_id or ''):
        raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
    if response.state not in (
            common.SCHEDULED_TASK_PENDING, common.SCHEDULED_TASK_RUNNING,
            common.SCHEDULED_TASK_RETRYING, common.SCHEDULED_TASK_COMPLETED,
            common.SCHEDULED_TASK_FAILED, common.SCHEDULED_TASK_CANCELLED):
        raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
    if response.occurrence_id and not IDENTIFIER.fullmatch(response.occurrence_id):
        raise PluginError(common.UPSTREAM_UNAVAILABLE, True)
    return response


def is_terminal(state) -> bool:
    return state in TERMINAL_STATES


def handed_off(state) -> bool:
    """True once a trigger intent exists; cancelling then must surface CONFLICT."""
    return state in HANDED_OFF_STATES
