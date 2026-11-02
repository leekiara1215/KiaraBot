"""Target delivery lease and ACK/Renew receipt semantics for PluginBridge.

Wire facts frozen by P06/P01:

* every fan-out attempt is a :class:`platform_pb2.DeliveryAttempt` carrying the
  full fence; the accompanying ``EventEnvelope.event_id`` must match
  ``attempt.event_id``;
* leases last 30 s, the SDK renews after 10 s and a single attempt can never be
  extended beyond 300 s from ``started_at``;
* ACK/Renew are acknowledged by a ``CoreToPlugin.delivery_receipt`` with the
  disposition ``ACKNOWLEDGED / DUPLICATE / STALE / RENEWED / REJECTED``;
* a stale attempt must not be confirmed or executed again by the SDK; the
  runtime will redeliver under a new attempt.
"""

from __future__ import annotations

from dataclasses import dataclass

from kiarabot.v1 import common_pb2 as common, platform_pb2 as platform

from .errors import PluginError
from .transport import timestamp_seconds, validate_fence

LEASE_SECONDS = 30.0
RENEW_AFTER_SECONDS = 10.0
MAX_LEASE_TOTAL_SECONDS = 300.0


def _disposition(name: str) -> int:
    return getattr(platform, name)


@dataclass(frozen=True)
class DeliveryLease:
    """One validated attempt bound to the current connection fence."""

    attempt: platform.DeliveryAttempt
    started_monotonic: float

    @classmethod
    def validate(cls, attempt: platform.DeliveryAttempt, event_id: str,
                 fence: platform.Fence, received_monotonic: float) -> 'DeliveryLease':
        validate_fence(attempt.fence, expected=fence)
        if (not attempt.event_id or attempt.event_id != event_id or not attempt.attempt_id
                or attempt.attempt_number < 1 or len(attempt.event_id) > 256
                or len(attempt.attempt_id) > 128
                or not attempt.HasField('started_at') or not attempt.HasField('lease_until')):
            raise PluginError(common.INVALID_ARGUMENT)
        started = timestamp_seconds(attempt.started_at)
        lease_until = timestamp_seconds(attempt.lease_until)
        if started <= 0 or lease_until <= started or lease_until - started > LEASE_SECONDS + 4.0:
            raise PluginError(common.INVALID_ARGUMENT)
        return cls(attempt=attempt, started_monotonic=received_monotonic)

    @property
    def attempt_id(self) -> str:
        return self.attempt.attempt_id

    @property
    def event_id(self) -> str:
        return self.attempt.event_id

    def renew_due(self, now_monotonic: float) -> bool:
        return now_monotonic - self.started_monotonic >= RENEW_AFTER_SECONDS

    def total_exhausted(self, now_monotonic: float) -> bool:
        return now_monotonic - self.started_monotonic >= MAX_LEASE_TOTAL_SECONDS


def validate_receipt(receipt: platform.DeliveryReceipt, lease: DeliveryLease,
                     accepted: bool) -> int:
    """Validate a delivery receipt and return the disposition value."""
    attempt = receipt.attempt
    if attempt.event_id != lease.event_id or attempt.attempt_id != lease.attempt_id:
        raise PluginError(common.UPSTREAM_UNAVAILABLE, retryable=True)
    disposition = receipt.disposition
    if accepted:
        allowed = {_disposition('DELIVERY_ACKNOWLEDGED'), _disposition('DELIVERY_DUPLICATE')}
    else:
        allowed = {_disposition('DELIVERY_REJECTED'), _disposition('DELIVERY_STALE')}
    # A stale receipt is always possible for a racing ACK/Renew, whether the
    # hook accepted or rejected the event.
    allowed.add(_disposition('DELIVERY_STALE'))
    if disposition not in allowed:
        raise PluginError(common.UPSTREAM_UNAVAILABLE, retryable=True)
    return disposition


def validate_renew_receipt(receipt: platform.DeliveryReceipt, lease: DeliveryLease) -> int:
    attempt = receipt.attempt
    if attempt.event_id != lease.event_id or attempt.attempt_id != lease.attempt_id:
        raise PluginError(common.UPSTREAM_UNAVAILABLE, retryable=True)
    disposition = receipt.disposition
    if disposition not in {_disposition('DELIVERY_RENEWED'),
                           _disposition('DELIVERY_STALE')}:
        raise PluginError(common.UPSTREAM_UNAVAILABLE, retryable=True)
    return disposition


def disposition_name(disposition: int) -> str:
    try:
        return platform.DeliveryDisposition.Name(disposition)
    except ValueError:
        return 'DELIVERY_DISPOSITION_UNSPECIFIED'
