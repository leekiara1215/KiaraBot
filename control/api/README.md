# C++ control service

## Target independent Control process (P04)

`kiarabot-control` is an independent process. It owns no authoritative business
DB, users, password verifiers or session/authorization cache. Its HTTPS
Login/Refresh/Me/ListSessions/Revoke endpoints call the current target IdentityService
over verified mTLS; Identity alone decides authentication, authorization and replay.
The public route prefix is `/api/v1`. The business HTTPS listener is separate from
loopback health probes and does not require a browser client certificate.

Start with `kiarabot-control --config <private-control.json>` using the strict service
configuration and optional `control_api` object described in
[operations](../../doc/08-operations.md). The current authentication mapping, error
semantics, limits and implementation scope are documented in
[Control API](../../doc/06-control-api.md). No local DB or fallback Identity is selected.
Readiness remains false while the wider business implementation is incomplete.
The generated-server transport fixture does not replace real Identity/PG/bootstrap
acceptance; see the [P04 evidence](../../reports/modules/architecture-p04-control-evidence.md).

## Historical reference implementation

Everything below describes the preserved same-process/SQLite reference implementation
and its historical behavior assertions. Its configuration, build/run commands and
production statements are not instructions for the independent target Control process.

The same Core process hosts the authenticated REST API and WebSocket service through
`IControlApplication`. Python adapter/SDK and TypeScript panel stay separate components.
OpenAPI and AsyncAPI in `api/` are the public contracts.

Build and verify from the repository root:

```sh
uv sync --project control/api/tests --locked --python 3.11.18
cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset proto --parallel 4
ctest --preset proto
control/api/tests/.venv/bin/python -m pytest -q control/api/tests
```

Requires the pinned JSON header fetched by CMake, Boost.Beast/Asio and libsodium;
see [toolchain](../../doc/11-toolchain.md). No third-party sources or generated types
are checked in. Tests use throwaway loopback ports, accounts and databases. They
include a standalone mock application and the production SQLite application using
the same response schema suite. The TypeScript live test uses `tests/live_fixture.py`.

`KIARABOT_CONTROL_CONFIG` points to an owner-only 0600 JSON file. Its shape is:

```json
{
  "listen": "127.0.0.1:8080",
  "encryption_key": "REPLACE_WITH_64_HEX_CHARACTERS",
  "users": [
    {"id": "administrator", "username": "admin", "password_hash": "REPLACE_WITH_ARGON2ID_HASH", "role": "admin"}
  ],
  "plugins": [],
  "cors_origins": ["https://panel.example.invalid"],
  "limits": {
    "authenticated_requests_per_second": 1000,
    "authenticated_burst": 2000,
    "public_requests_per_second": 30,
    "public_burst": 60,
    "max_streams_per_token": 16,
    "max_requests_per_connection": 1000,
    "idle_timeout_seconds": 30,
    "drain_timeout_seconds": 3
  }
}
```

`cors_origins` and `limits` are optional; a missing field keeps the defaults above and
unknown limit names or malformed origins fail startup.

Each `users[]` entry may carry an optional `permissions` string array of explicit grants.
A non-array or non-string value fails startup; Core's startup whitelist currently accepts
only `message.read`, the grant required to subscribe to the `/ws/messages` event stream
(no default role, including `admin`, holds it). The event payload is metadata only; see
[`api/asyncapi.yaml`](../../api/asyncapi.yaml) and [`doc/06-control-api.md`](../../doc/06-control-api.md).

Use `build/proto/control/api/kiarabot-control-credentials --key` to generate the key;
without arguments it reads a password line from stdin and prints its Argon2id hash.
Keep these outputs out of source control and logs. Supply the existing Core runtime
DB and bridge configuration, then start `kiarabot-core --grpc-listen 127.0.0.1:50051`.
The control listener cannot start without the persistent store. Back up the control
key with the database; a wrong key fails startup instead of resetting sessions.

Local endpoints are `/api/v1/*`, `/ws/control` and the individual stream channels,
plus `/health/live` and `/health/ready`. Use a same-origin HTTPS/WSS reverse proxy in
production; this service refuses external interface binding. TLS and deployment
recovery still require T11b acceptance.

Current limits: 64 KiB request bodies, 16 KiB headers, 128 HTTP connections and 128
WebSockets, 1024 retained stream sessions, 4096 events per stream and ten-minute
retention. Backlogs are sent in batches of 32; an unfillable gap requires resync.
Streams coalesce resource states every 100ms and are not an audit history. Sessions
and successful receipts persist encrypted; stream sessions, page snapshots and logs
are bounded process memory and restart requires new snapshots.

Transport production behavior (W3):

- HTTP/1.1 keep-alive is enabled with an idle timeout, a bounded per-connection request
  count and per-connection drain on shutdown. `HEAD` mirrors the matching GET headers
  with an empty body; a known path with the wrong method returns `405` and `Allow`;
  `OPTIONS` returns allowed methods for a known path.
- CORS is deny-by-default and only granted to exact origins listed in
  `cors_origins`. Preflight responses include `Allow-Methods`/`Allow-Headers`; no
  wildcard and no credential sharing is offered.
- `traceparent` (W3C) and `X-Request-ID`/`X-Trace-ID` are accepted when well formed and
  echoed on every response; they are never trusted as identity. An access log line per
  request is written to stderr as JSON (`component=control-api`, `operation=http_request`,
  method/path/status/duration/request_id/trace_id, no credentials or query tokens).
- Per-client token buckets protect the listener (separate authenticated/public rates);
  exceeding them returns `429`/`Retry-After`. Authenticated streams are limited per
  access token (default 16) and return WebSocket close code `1013`; `1013` remains a
  transport-only code pending contracts registration.
- `SIGTERM`/`SIGINT` stop accepting connections, let in-flight HTTP requests finish,
  send WebSocket close `1001` to every stream, then join the io thread; the HTTP writer
  and idle keep-alive connections are closed after a bounded drain timeout.

Plugin enable/disable controls registration and new delivery. Config updates persist
non-secret control values with optimistic versions; supported SDK instances apply
them through the configuration delivery and acknowledgement protocol. An accepted command never claims a child process has started or that
an external side effect was cancelled. This is a functional local baseline, not a
load, hostile-traffic or production deployment certification.

插件配置：PUT 成功仅表示加密持久化；GET 的可选 delivery 展示当前连接应用证据，包含 UNAVAILABLE/PENDING/APPLIED/REJECTED、applied_version 和脱敏 error。只有协商 plugin.config.v1 的 SDK 完成回调并确认才进入 APPLIED；重连/重启重新应用。PUT 重放不返回瞬时状态，界面需要轮询 GET；现有 WebSocket 只通知期望配置变化，不推送 ACK 状态。

配置链路验收还需已安装 sdk/python 锁定环境并运行 `sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py`；统一 `tests/acceptance/run.sh` 会先生成协议，再执行控制面和 SDK 测试。
