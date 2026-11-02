"""Target Runtime bridge value types, authorization lifecycle and secure deployment files.

Target contract (docs 01/04, frozen 2026-11-02): an external adapter connects to the Runtime
``bridge_listen`` endpoint with server-verified TLS (the Runtime certificate is validated
against a deployment CA; the adapter never presents or claims a client certificate). The first
stream frame registers the adapter; the successful registration response carries a ``Fence`` and
a short-lived ``ConnectionAuthorization``. Every later frame carries the current ``Fence`` plus
the opaque ``connection_proof``. Before the authorization expires the adapter rotates the proof
and refresh credential with a single-flight ``RefreshConnectionAuthorization`` request.

Secrets are only ever held in memory: the bootstrap credential is read from a deployment secret
file, and proofs, refresh credentials and bootstrap values are excluded from dataclass ``repr``,
logs and error text. Persisted state (the event outbox) only carries platform-neutral events.
"""
from __future__ import annotations

import os
import re
import stat
import time
import uuid
from collections.abc import Mapping
from dataclasses import dataclass, field

import grpc
from kiarabot.v1 import adapters_pb2 as adapters, common_pb2 as common, platform_pb2 as platform
from google.protobuf.timestamp_pb2 import Timestamp

PROTOCOL_VERSION = 'kiarabot.v1'
IMPLEMENTATION = 'kiarabot-adapter-nonebot'
IMPLEMENTATION_VERSION = '0.2.0'

# Contract budget: nominal proof lifetime is at most 56s plus 2s node skew and 2s tolerance.
MAX_AUTHORIZATION_LIFETIME_SECONDS = 60.0
# Refresh single-flight starts this many seconds before expiry (halved for very short lifetimes
# so a self-consistent minimum lifetime still gets one renewal window).
REFRESH_LEAD_SECONDS = 10.0
MIN_CREDENTIAL_BYTES = 32
MAX_DEPLOYMENT_FILE_BYTES = 1024 * 1024
MAX_BOOTSTRAP_BYTES = 4096
MAX_HEARTBEAT_SECONDS = 300

# Stable codes that cannot succeed by retrying the same deployment. Everything else (transport,
# dependency, timeout, rate limiting) is retried with the event outbox preserved.
FATAL_REGISTRATION_CODES = frozenset({common.UNAUTHENTICATED, common.PERMISSION_DENIED,
                                      common.INVALID_ARGUMENT, common.CAPABILITY_NOT_SUPPORTED,
                                      common.PLUGIN_UNAVAILABLE})

_UUID4 = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z')
_IDENTIFIER = re.compile(r'[A-Za-z0-9._:@-]{1,128}\Z')
_ENDPOINT = re.compile(r'(?P<host>[A-Za-z0-9.-]{1,253}|\[[0-9A-Fa-f:.]{2,45}\]):(?P<port>[0-9]{1,5})\Z')
_SERVER_NAME = re.compile(r'[A-Za-z0-9.-]{1,253}\Z')
_PRINTABLE = range(33, 127)


class BridgeFailure(Exception):
    """Stable-code failure; free text never carries deployment secrets or upstream bodies."""

    def __init__(self, code, *, fatal=False, retry_after=0.0):
        self.code = code if code in common.ErrorCode.values() and code else common.INTERNAL
        self.fatal = fatal
        self.retry_after = retry_after
        super().__init__(common.ErrorCode.Name(self.code))


def stable_code(value):
    return value if isinstance(value, int) and value in common.ErrorCode.values() and value else 0


def error_code(message) -> int:
    """Stable code from a wire Error; unknown/unspecified codes are a protocol violation."""
    if not isinstance(message, common.Error):
        raise BridgeFailure(common.INTERNAL)
    code = stable_code(message.code)
    if not code:
        raise BridgeFailure(common.INTERNAL)
    return code


def bounded_identifier(value: str) -> bool:
    return bool(value) and bool(_IDENTIFIER.fullmatch(value))


def bounded_epoch(value: str) -> bool:
    return bool(value) and len(value) <= 128 and all(ch.isascii() and ord(ch) in _PRINTABLE for ch in value)


def uuid4_text() -> str:
    return str(uuid.uuid4())


def valid_uuid4(value: str) -> bool:
    return bool(value) and bool(_UUID4.fullmatch(value))


def timestamp(seconds: float) -> Timestamp:
    """Wall-clock unix seconds as a protobuf Timestamp (never rewinds by truncation)."""
    result = Timestamp()
    whole = int(seconds)
    result.seconds = whole
    result.nanos = int(round((seconds - whole) * 1e9))
    if result.nanos >= 1_000_000_000:
        result.seconds += 1
        result.nanos = 0
    return result


def _unix_seconds(value: Timestamp) -> float:
    return float(value.seconds) + float(value.nanos) / 1e9


def request_context(timeout: float):
    """Registration RequestContext: UUIDv4 correlation ids and a bounded future deadline."""
    now = time.time()
    return platform.RequestContext(request_id=uuid4_text(), trace_id=uuid4_text(),
                                   deadline=timestamp(now + timeout))


def _secure_file(path: str, *, secret: bool, maximum: int) -> bytes:
    """Regular non-symlink deployment file; secrets also require owner-only permissions."""
    try:
        parent = os.path.dirname(os.path.abspath(path)) or '.'
        if os.path.basename(path) in ('', '.', '..') or not os.path.isdir(parent):
            raise OSError
        status = os.lstat(path)
        if stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode):
            raise OSError
        if status.st_size <= 0 or status.st_size > maximum:
            raise OSError
        if secret and status.st_mode & 0o077:
            raise OSError
        with open(path, 'rb') as handle:
            payload = handle.read(maximum + 1)
    except OSError:
        raise BridgeFailure(common.INVALID_ARGUMENT) from None
    if not payload or len(payload) > maximum:
        raise BridgeFailure(common.INVALID_ARGUMENT)
    return payload


@dataclass(frozen=True)
class Fence:
    instance_id: str
    execution_generation: int
    connection_generation: int
    runtime_epoch: str

    def __post_init__(self):
        if (not bounded_identifier(self.instance_id)
                or not isinstance(self.execution_generation, int) or self.execution_generation <= 0
                or not isinstance(self.connection_generation, int) or self.connection_generation <= 0
                or not bounded_epoch(self.runtime_epoch)):
            raise BridgeFailure(common.INVALID_ARGUMENT)

    @classmethod
    def from_proto(cls, value) -> "Fence":
        if (not isinstance(value, platform.Fence) or not value.instance_id
                or not value.HasField('execution_generation')
                or not value.HasField('connection_generation')):
            raise BridgeFailure(common.INVALID_ARGUMENT)
        return cls(value.instance_id, int(value.execution_generation),
                   int(value.connection_generation), value.runtime_epoch)

    def to_proto(self) -> platform.Fence:
        return platform.Fence(instance_id=self.instance_id,
                              execution_generation=self.execution_generation,
                              connection_generation=self.connection_generation,
                              runtime_epoch=self.runtime_epoch)


@dataclass(frozen=True)
class ConnectionAuthorization:
    """Short-lived connection fact; ``proof``/``refresh_credential`` are opaque secrets."""

    fence: Fence
    proof: bytes = field(repr=False)
    refresh_credential: bytes = field(repr=False)
    refresh_generation: int
    issued_at: float
    expires_at: float
    principal_id: str
    credential_version: int
    authorization_version: int

    def __post_init__(self):
        if (not isinstance(self.fence, Fence) or not isinstance(self.proof, bytes)
                or len(self.proof) < MIN_CREDENTIAL_BYTES
                or not isinstance(self.refresh_credential, bytes)
                or len(self.refresh_credential) < MIN_CREDENTIAL_BYTES
                or not isinstance(self.refresh_generation, int) or self.refresh_generation <= 0
                or not isinstance(self.issued_at, float) and not isinstance(self.issued_at, int)
                or not isinstance(self.expires_at, float) and not isinstance(self.expires_at, int)
                or self.issued_at <= 0 or self.expires_at <= self.issued_at
                or self.expires_at - self.issued_at > MAX_AUTHORIZATION_LIFETIME_SECONDS
                or not bounded_identifier(self.principal_id)
                or self.credential_version <= 0 or self.authorization_version <= 0):
            raise BridgeFailure(common.INVALID_ARGUMENT)

    @property
    def lifetime(self) -> float:
        return self.expires_at - self.issued_at

    @property
    def refresh_lead(self) -> float:
        return min(REFRESH_LEAD_SECONDS, max(0.1, self.lifetime / 2.0))

    def valid(self, now: float) -> bool:
        return self.issued_at <= now < self.expires_at

    def refresh_due(self, now: float) -> bool:
        return self.valid(now) and self.expires_at - now <= self.refresh_lead

    @classmethod
    def from_proto(cls, value, *, expected_fence: Fence) -> "ConnectionAuthorization":
        if not isinstance(value, platform.ConnectionAuthorization):
            raise BridgeFailure(common.INVALID_ARGUMENT)
        fence = Fence.from_proto(value.fence)
        if fence != expected_fence:
            raise BridgeFailure(common.UNAUTHENTICATED)
        if not value.HasField('issued_at') or not value.HasField('expires_at'):
            raise BridgeFailure(common.INVALID_ARGUMENT)
        return cls(fence, bytes(value.proof), bytes(value.refresh_credential),
                   int(value.refresh_generation), _unix_seconds(value.issued_at),
                   _unix_seconds(value.expires_at), value.principal_id,
                   int(value.credential_version), int(value.authorization_version))

    def refresh_request(self):
        return platform.RefreshConnectionAuthorization(
            request_id=uuid4_text(), fence=self.fence.to_proto(), proof=self.proof,
            refresh_credential=self.refresh_credential,
            refresh_generation=self.refresh_generation)


@dataclass(frozen=True)
class Registration:
    instance_id: str
    fence: Fence
    authorization: ConnectionAuthorization
    heartbeat_interval: float
    effective_capabilities: frozenset

    @classmethod
    def from_response(cls, result, *, expected_instance: str) -> "Registration":
        """Validate a successful target registration; every missing/ambiguous field fails closed."""
        if not isinstance(result, adapters.RegisterAdapterResponse) or not result.accepted:
            raise BridgeFailure(common.INTERNAL)
        if result.HasField('error'):
            raise BridgeFailure(error_code(result.error))
        if result.required_protocol_version != PROTOCOL_VERSION:
            raise BridgeFailure(common.CAPABILITY_NOT_SUPPORTED)
        instance = result.assigned_instance_id
        if not bounded_identifier(instance) or instance != expected_instance:
            raise BridgeFailure(common.PERMISSION_DENIED)
        fence = Fence.from_proto(result.fence)
        if fence.instance_id != instance:
            raise BridgeFailure(common.UNAUTHENTICATED)
        if not result.HasField('authorization'):
            raise BridgeFailure(common.UNAUTHENTICATED)
        authorization = ConnectionAuthorization.from_proto(result.authorization,
                                                           expected_fence=fence)
        if authorization.principal_id != instance:
            raise BridgeFailure(common.PERMISSION_DENIED)
        heartbeat = int(result.heartbeat_interval_seconds)
        if not 1 <= heartbeat <= MAX_HEARTBEAT_SECONDS or result.heartbeat_interval_seconds != heartbeat:
            raise BridgeFailure(common.INVALID_ARGUMENT)
        capabilities = frozenset(
            item.name for item in result.effective_capabilities
            if item.supported and item.version == '1' and bounded_identifier(item.name))
        return cls(instance, fence, authorization, float(heartbeat), capabilities)


@dataclass(frozen=True)
class BridgeConfig:
    """Deployment configuration for the target Runtime bridge (no protocol compatibility mode)."""

    endpoint: str
    ca_file: str
    bootstrap_file: str
    adapter_instance_id: str
    account_id: str
    adapter_id: str | None = None
    server_name: str | None = None
    timeout: float = 5.0
    retry_base: float = 1.0
    idle_interval: float = 1.0
    _ca: bytes = field(default=b'', repr=False, compare=False)
    _bootstrap: str = field(default='', repr=False, compare=False)

    def __post_init__(self):
        match = _ENDPOINT.fullmatch(self.endpoint) if isinstance(self.endpoint, str) else None
        if (not match or not 1 <= int(match['port']) <= 65535
                or not valid_uuid4(self.adapter_instance_id)
                or not bounded_identifier(self.account_id)
                or not isinstance(self.timeout, (int, float)) or not 0 < self.timeout <= 120
                or not isinstance(self.retry_base, (int, float)) or not 0 < self.retry_base <= 30
                or not isinstance(self.idle_interval, (int, float)) or not 0 < self.idle_interval <= 30):
            raise BridgeFailure(common.INVALID_ARGUMENT)
        adapter_id = self.adapter_id if self.adapter_id is not None else self.adapter_instance_id
        if not bounded_identifier(adapter_id):
            raise BridgeFailure(common.INVALID_ARGUMENT)
        if self.server_name is not None and not _SERVER_NAME.fullmatch(self.server_name):
            raise BridgeFailure(common.INVALID_ARGUMENT)
        object.__setattr__(self, 'adapter_id', adapter_id)
        ca = _secure_file(self.ca_file, secret=False, maximum=MAX_DEPLOYMENT_FILE_BYTES)
        if b'-----BEGIN CERTIFICATE-----' not in ca:
            raise BridgeFailure(common.INVALID_ARGUMENT)
        object.__setattr__(self, '_ca', ca)
        raw = _secure_file(self.bootstrap_file, secret=True, maximum=MAX_BOOTSTRAP_BYTES)
        try:
            credential = raw.decode('ascii')
        except UnicodeDecodeError:
            raise BridgeFailure(common.INVALID_ARGUMENT) from None
        if credential.endswith('\n'):
            credential = credential[:-1]
        if (not 32 <= len(credential) <= MAX_BOOTSTRAP_BYTES
                or any(ord(ch) not in _PRINTABLE or ch in ' \t\r\n' for ch in credential)):
            raise BridgeFailure(common.INVALID_ARGUMENT)
        object.__setattr__(self, '_bootstrap', credential)

    @classmethod
    def from_environment(cls, environ: Mapping[str, str]) -> "BridgeConfig":
        def required(name):
            value = environ.get(name)
            if not value:
                raise BridgeFailure(common.INVALID_ARGUMENT)
            return value

        try:
            return cls(
                endpoint=required('BRIDGE_ENDPOINT'),
                ca_file=required('BRIDGE_CA_FILE'),
                bootstrap_file=required('BRIDGE_BOOTSTRAP_CREDENTIAL_FILE'),
                adapter_instance_id=required('ADAPTER_INSTANCE_ID'),
                account_id=required('ACCOUNT_ID'),
                adapter_id=environ.get('ADAPTER_ID') or None,
                server_name=environ.get('BRIDGE_SERVER_NAME') or None,
                timeout=float(environ.get('BRIDGE_TIMEOUT', '5')),
                retry_base=float(environ.get('BRIDGE_RETRY_BASE', '1')),
                idle_interval=float(environ.get('BRIDGE_IDLE_INTERVAL', '1')),
            )
        except (TypeError, ValueError):
            raise BridgeFailure(common.INVALID_ARGUMENT) from None

    def tls_credentials(self):
        """Server-verified TLS: only the deployment CA is trusted; no client certificate."""
        return grpc.ssl_channel_credentials(root_certificates=self._ca)

    def channel_options(self):
        options = [('grpc.max_receive_message_length', 1024 * 1024),
                   ('grpc.max_send_message_length', 1024 * 1024)]
        if self.server_name is not None:
            options.append(('grpc.ssl_target_name_override', self.server_name))
        return tuple(options)

    def metadata(self):
        """Exactly one bootstrap credential; the value never leaves this tuple or the logs."""
        return (('authorization', 'Bearer ' + self._bootstrap),)

    def registration_request(self, capabilities) -> adapters.RegisterAdapterRequest:
        request = adapters.RegisterAdapterRequest(
            adapter_id=self.adapter_id, implementation=IMPLEMENTATION,
            implementation_version=IMPLEMENTATION_VERSION, protocol_version=PROTOCOL_VERSION,
            adapter_instance_id=self.adapter_instance_id,
            context=request_context(self.timeout))
        request.accounts.add(account_id=self.account_id, enabled=True)
        for name in capabilities:
            request.capabilities.add(name=name, version='1', supported=True)
        return request


def frame(fence: Fence, proof: bytes, **body) -> adapters.AdapterToCore:
    """Post-registration client frame: complete current fence plus opaque connection proof."""
    return adapters.AdapterToCore(fence=fence.to_proto(), connection_proof=proof, **body)


def check_response_fence(response, current: Fence) -> None:
    """Every post-registration Runtime frame carries the current fence; mismatch is unauthenticated."""
    if not response.HasField('fence'):
        raise BridgeFailure(common.UNAUTHENTICATED)
    if Fence.from_proto(response.fence) != current:
        raise BridgeFailure(common.UNAUTHENTICATED)


def authorization_result(response):
    """Return (request_id, authorization | None, error_code | None) from a refresh response."""
    result = response.authorization_result
    request_id = result.request_id
    if not bounded_identifier(request_id):
        raise BridgeFailure(common.INTERNAL)
    outcome = result.WhichOneof('outcome')
    if outcome == 'authorization':
        return request_id, result.authorization, None
    if outcome == 'error':
        return request_id, None, error_code(result.error)
    raise BridgeFailure(common.INTERNAL)


def shutdown_notice(response):
    notice = response.shutdown
    drain_ms = int(notice.drain_timeout_ms)
    if drain_ms > 600_000:
        raise BridgeFailure(common.INVALID_ARGUMENT)
    return drain_ms
