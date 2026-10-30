# C++ control service

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
  "plugins": []
}
```

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

Plugin enable/disable controls registration and new delivery. Config updates persist
non-secret control values with optimistic versions; supported SDK instances apply
them through the configuration delivery and acknowledgement protocol. An accepted command never claims a child process has started or that
an external side effect was cancelled. This is a functional local baseline, not a
load, hostile-traffic or production deployment certification.

插件配置：PUT 成功仅表示加密持久化；GET 的可选 delivery 展示当前连接应用证据，包含 UNAVAILABLE/PENDING/APPLIED/REJECTED、applied_version 和脱敏 error。只有协商 plugin.config.v1 的 SDK 完成回调并确认才进入 APPLIED；重连/重启重新应用。PUT 重放不返回瞬时状态，界面需要轮询 GET；现有 WebSocket 只通知期望配置变化，不推送 ACK 状态。

配置链路验收还需已安装 sdk/python 锁定环境并运行 `sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py`；统一 `tests/acceptance/run.sh` 会先生成协议，再执行控制面和 SDK 测试。
