"""Target transport: server-verified TLS, bootstrap metadata and ConnectionAuthorization.

The target PluginBridge is reachable only over server-verified TLS (no insecure
fallback) and authenticates the stream with a per-instance bootstrap credential
in the ``authorization: Bearer <credential>`` metadata key.  Registration
returns a complete :class:`platform_pb2.ConnectionAuthorization`; every later
frame must carry the current ``Fence`` and ``connection_proof``.  The proof and
refresh material are short lived and are rotated on the same TLS stream by a
single-flight refresh before they expire.
"""

from __future__ import annotations

import hmac
import os
from dataclasses import dataclass
from pathlib import Path

import grpc
from google.protobuf.timestamp_pb2 import Timestamp
from kiarabot.v1 import platform_pb2 as platform, plugins_pb2 as plugins

from .errors import PluginError
from kiarabot.v1 import common_pb2 as common

TARGET_FEATURES = ('action.result.v1', 'host.storage.v1', 'host.model.v1',
                   'host.model.stream.v1', 'host.tasks.v1', 'plugin.config.v1')
#: Upper bound from the frozen contract: a ConnectionAuthorization nominally
#: lives at most 56 seconds; the total budget including skew stays under 60 s.
MAX_AUTHORIZATION_LIFETIME_SECONDS = 56.0
#: Refresh this many seconds before the server issued expiry.
REFRESH_LEAD_SECONDS = 10.0
_PROOF_BYTES = 32


def load_root_certificate(value: bytes | str | Path | None) -> bytes | None:
    """Return PEM bytes for a CA bundle, a path, or ``None`` when unset."""
    if value is None:
        return None
    if isinstance(value, bytes):
        return value
    try:
        return Path(value).read_bytes()
    except OSError:
        raise PluginError(common.INVALID_ARGUMENT) from None


def open_channel(endpoint: str, root_certificate: bytes, server_name: str | None = None) -> grpc.aio.Channel:
    """Open the target PluginBridge channel with mandatory server validation.

    ``root_certificate`` must contain the deployment CA (PEM).  There is no
    insecure fallback: a missing/unreadable CA fails closed with
    ``INVALID_ARGUMENT`` before any connection is attempted.
    """
    if not root_certificate:
        raise PluginError(common.INVALID_ARGUMENT)
    credentials = grpc.ssl_channel_credentials(root_certificates=root_certificate)
    options = [('grpc.max_receive_message_length', 1024 * 1024),
               ('grpc.max_send_message_length', 1024 * 1024)]
    if server_name:
        options.append(('grpc.ssl_target_name_override', server_name))
    return grpc.aio.secure_channel(endpoint, credentials, options=options)


def timestamp_seconds(value: Timestamp) -> float:
    return value.seconds + value.nanos / 1_000_000_000


def _positive(value: int) -> bool:
    return value > 0


def _safe_equal(left: bytes, right: bytes) -> bool:
    return hmac.compare_digest(left, right)


def validate_fence(fence: platform.Fence, expected: platform.Fence | None = None) -> None:
    """Validate a registration/refresh fence; compare with ``expected`` when given."""
    if (not fence.instance_id or not fence.runtime_epoch
            or not fence.HasField('connection_generation') or not _positive(fence.connection_generation)
            or not fence.HasField('execution_generation') or not _positive(fence.execution_generation)):
        raise PluginError(common.INVALID_ARGUMENT)
    if expected is not None and fence != expected:
        raise PluginError(common.UNAUTHENTICATED, retryable=True)


@dataclass(frozen=True)
class ConnectionState:
    """Current connection authorization material bound to this TLS stream."""

    fence: platform.Fence
    proof: bytes
    refresh_credential: bytes
    refresh_generation: int
    authorization_version: int
    credential_version: int
    issued_seconds: float
    expires_seconds: float
    received_monotonic: float

    def attach(self, frame: plugins.PluginToCore) -> plugins.PluginToCore:
        """Attach the complete fence and current proof to an outgoing frame."""
        frame.fence.CopyFrom(self.fence)
        frame.connection_proof = self.proof
        return frame

    def refresh_due(self, now_monotonic: float) -> bool:
        """True when the proof must be rotated before the server-issued expiry."""
        lifetime = max(0.0, self.expires_seconds - self.issued_seconds)
        refresh_at = max(0.0, lifetime - REFRESH_LEAD_SECONDS)
        return (now_monotonic - self.received_monotonic) >= refresh_at

    def seconds_until_expiry(self, now_monotonic: float) -> float:
        lifetime = max(0.0, self.expires_seconds - self.issued_seconds)
        return lifetime - (now_monotonic - self.received_monotonic)


def validate_registration_response(
        response: plugins.RegisterPluginResponse,
        instance_id: str,
        received_monotonic: float) -> ConnectionState:
    """Validate the registration result and extract required fence/authorization."""
    if not response.accepted:
        raise PluginError(response.error.code or common.PERMISSION_DENIED,
                          response.error.retryable)
    if not response.HasField('fence') or not response.HasField('authorization'):
        raise PluginError(common.UPSTREAM_UNAVAILABLE, retryable=True)
    fence = response.fence
    validate_fence(fence)
    if fence.instance_id != instance_id:
        raise PluginError(common.PERMISSION_DENIED)
    authorization = response.authorization
    validate_fence(authorization.fence, expected=fence)
    if (authorization.principal_id != fence.instance_id
            or len(authorization.proof) != _PROOF_BYTES
            or len(authorization.refresh_credential) != _PROOF_BYTES
            or not _positive(authorization.refresh_generation)
            or not _positive(authorization.credential_version)
            or not _positive(authorization.authorization_version)
            or not authorization.HasField('issued_at') or not authorization.HasField('expires_at')):
        raise PluginError(common.UPSTREAM_UNAVAILABLE, retryable=True)
    issued = timestamp_seconds(authorization.issued_at)
    expires = timestamp_seconds(authorization.expires_at)
    if (issued <= 0 or expires <= issued
            or expires - issued > MAX_AUTHORIZATION_LIFETIME_SECONDS + 4.0):
        raise PluginError(common.UPSTREAM_UNAVAILABLE, retryable=True)
    return ConnectionState(fence=fence,
                           proof=bytes(authorization.proof),
                           refresh_credential=bytes(authorization.refresh_credential),
                           refresh_generation=authorization.refresh_generation,
                           authorization_version=authorization.authorization_version,
                           credential_version=authorization.credential_version,
                           issued_seconds=issued,
                           expires_seconds=expires,
                           received_monotonic=received_monotonic)


def validate_frame_authorization(frame: plugins.CoreToPlugin, state: ConnectionState) -> None:
    """Every post-registration server frame must carry the current fence and proof."""
    if not frame.HasField('fence') or frame.fence != state.fence:
        raise PluginError(common.UNAUTHENTICATED, retryable=True)


def build_refresh(state: ConnectionState, request_id: str) -> platform.RefreshConnectionAuthorization:
    return platform.RefreshConnectionAuthorization(
        request_id=request_id,
        fence=state.fence,
        proof=state.proof,
        refresh_credential=state.refresh_credential,
        refresh_generation=state.refresh_generation)


def accept_refresh(state: ConnectionState,
                   request_id: str,
                   result: platform.ConnectionAuthorizationResult,
                   received_monotonic: float) -> ConnectionState:
    """Validate one single-flight refresh result and rotate local material."""
    if result.request_id != request_id:
        raise PluginError(common.UNAUTHENTICATED, retryable=True)
    outcome = result.WhichOneof('outcome')
    if outcome == 'error':
        # A rejected refresh must not be retried on the same stream: the
        # bootstrap credential is re-used to establish a new connection.
        raise PluginError(result.error.code or common.UNAUTHENTICATED,
                          result.error.retryable)
    if outcome != 'authorization':
        raise PluginError(common.UNAUTHENTICATED, retryable=True)
    authorization = result.authorization
    validate_fence(authorization.fence, expected=state.fence)
    if (authorization.principal_id != state.fence.instance_id
            or len(authorization.proof) != _PROOF_BYTES
            or len(authorization.refresh_credential) != _PROOF_BYTES
            or authorization.refresh_generation != state.refresh_generation + 1
            or authorization.credential_version != state.credential_version
            or authorization.authorization_version < state.authorization_version
            or not authorization.HasField('issued_at') or not authorization.HasField('expires_at')):
        raise PluginError(common.UNAUTHENTICATED, retryable=True)
    if _safe_equal(bytes(authorization.proof), state.proof) or _safe_equal(
            bytes(authorization.refresh_credential), state.refresh_credential):
        raise PluginError(common.UNAUTHENTICATED, retryable=True)
    issued = timestamp_seconds(authorization.issued_at)
    expires = timestamp_seconds(authorization.expires_at)
    if issued <= 0 or expires <= issued or expires - issued > MAX_AUTHORIZATION_LIFETIME_SECONDS + 4.0:
        raise PluginError(common.UNAUTHENTICATED, retryable=True)
    return ConnectionState(fence=state.fence,
                           proof=bytes(authorization.proof),
                           refresh_credential=bytes(authorization.refresh_credential),
                           refresh_generation=authorization.refresh_generation,
                           authorization_version=authorization.authorization_version,
                           credential_version=authorization.credential_version,
                           issued_seconds=issued,
                           expires_seconds=expires,
                           received_monotonic=received_monotonic)


def validate_endpoint(endpoint: str) -> None:
    """Validate a host:port target endpoint without forcing a loopback literal."""
    if not endpoint or len(endpoint) > 255 or any(ch in endpoint for ch in ' \t\r\n/\\'):
        raise PluginError(common.INVALID_ARGUMENT)
    host, separator, port = endpoint.rpartition(':')
    if not separator or not host or len(host) > 253 or not port.isdigit():
        raise PluginError(common.INVALID_ARGUMENT)
    if not 1 <= int(port) <= 65535:
        raise PluginError(common.INVALID_ARGUMENT)


def random_request_id() -> str:
    import uuid
    return str(uuid.uuid4())


def monotonic_seconds() -> float:
    import time
    return time.monotonic()
