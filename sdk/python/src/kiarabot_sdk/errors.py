"""Stable SDK error type shared by the transport and runtime modules."""

from __future__ import annotations

from kiarabot.v1 import common_pb2 as common


class PluginError(Exception):
    def __init__(self, code: int, retryable: bool = False):
        self.code = code if code in common.ErrorCode.values() and code else common.INTERNAL
        self.retryable = retryable
        super().__init__(common.ErrorCode.Name(self.code))
