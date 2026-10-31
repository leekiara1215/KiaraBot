from __future__ import annotations

import time
import uuid

from kiarabot.v1 import common_pb2 as common, events_pb2 as events


class AdapterStatusReporter:
    """Report adapter/platform state transitions through the durable event outbox.

    Only transitions are emitted, so a flapping platform does not flood Core. Events
    are at-least-once like every other adapter event and are dropped only if the
    outbox is full (RATE_LIMITED), which is logged by the caller.
    """

    def __init__(self, outbox, adapter_id: str, account_id: str, now=time.time):
        self.outbox, self.adapter_id, self.account_id = outbox, adapter_id, account_id
        self.now = now
        self.last_state = None

    def report(self, state: int, reason: str = '') -> str | None:
        if state == self.last_state:
            return None
        event_id = str(uuid.uuid4())
        event = events.EventEnvelope(event_id=event_id, trace_id=event_id,
                                     schema_version='kiarabot.v1',
                                     delivery_mode=common.AT_LEAST_ONCE)
        event.occurred_at.seconds = int(self.now())
        event.source.adapter_id = self.adapter_id
        event.source.account_id = self.account_id
        event.adapter_status_changed.adapter_id = self.adapter_id
        event.adapter_status_changed.state = state
        event.adapter_status_changed.reason = reason
        self.outbox.enqueue(event)
        self.last_state = state
        return event_id
