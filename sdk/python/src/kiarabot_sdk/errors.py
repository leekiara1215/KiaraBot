"""Stable SDK error type shared by the transport and runtime modules."""

from __future__ import annotations

from kiarabot.v1 import common_pb2 as common


class PluginError(Exception):
    """Stable SDK error.

    ``code`` is always a defined ``ErrorCode``; ``retryable`` is the wire
    retryability, never an automatic retry permission. Target streaming errors
    additionally carry the typed delivery facts as keyword-only attributes:
    ``acceptance``/``recovery``/``retry_after_ms`` (from ``RpcFailure``),
    ``partial``/``partial_text``/``usage``/``may_have_billed``,
    ``logical_call_id`` and ``delivery_unknown``. The original two-argument
    constructor stays valid, so existing non-streaming call sites keep working.
    """

    def __init__(self, code: int, retryable: bool = False, *, acceptance=None,
                 recovery=None, retry_after_ms=None, partial: bool = False,
                 partial_text: str | None = None, usage=None, may_have_billed=None,
                 logical_call_id: str | None = None, delivery_unknown: bool = False):
        self.code = code if code in common.ErrorCode.values() and code else common.INTERNAL
        self.retryable = retryable
        self.acceptance = acceptance
        self.recovery = recovery
        self.retry_after_ms = retry_after_ms
        self.partial = partial
        self.partial_text = partial_text
        self.usage = usage
        self.may_have_billed = may_have_billed
        self.logical_call_id = logical_call_id
        self.delivery_unknown = delivery_unknown
        # Filled by stream terminal/rejection paths when available.
        self.terminal = None
        self.termination = None
        self.cause = None
        super().__init__(common.ErrorCode.Name(self.code))
