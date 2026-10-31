from __future__ import annotations

from kiarabot.v1 import common_pb2 as common

# OneBot V11 retcodes used by NapCat/OneBot implementations. Unknown codes are
# treated as an upstream failure, never as success. Classification exists so Core
# and plugins can see a stable code instead of one opaque UPSTREAM_UNAVAILABLE.
_RATE_LIMITED = frozenset({1200, 1201})
_PERMISSION_DENIED = frozenset({200, 1401, 1403})
_NOT_FOUND = frozenset({1404, 2000})


def classify_retcode(retcode: object) -> int:
    if not isinstance(retcode, int) or isinstance(retcode, bool):
        return common.UPSTREAM_UNAVAILABLE
    if retcode in _RATE_LIMITED:
        return common.RATE_LIMITED
    if retcode in _PERMISSION_DENIED:
        return common.PERMISSION_DENIED
    if retcode in _NOT_FOUND:
        return common.NOT_FOUND
    return common.UPSTREAM_UNAVAILABLE
