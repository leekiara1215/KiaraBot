# KiaraBot NoneBot2 适配器（目标 Runtime Bridge）

`NoneBot2 / OneBot V11` → 统一 `EventEnvelope` → 本地有界 SQLite outbox → 目标 Runtime 外部
`AdapterBridge.Connect` 双流。Python 3.11.18；依赖以 `uv.lock` 为准。代码不访问 Runtime 数据库，
不包含业务插件。

P06c（2026-11-02）起适配器只实现**目标 Bridge**，不再提供 reference 的 loopback insecure
gRPC/token 兼容模式：连接使用 server-verified TLS，注册使用部署交付的一次性 bootstrap 凭据，
注册成功后每帧携带完整 `Fence` 与当前 `connection_proof`，并在授权到期前单飞 refresh。契约
唯一来源为 `proto/kiarabot/v1/`（docs 01/04 目标章节已冻结）。

## 连接与认证（目标契约）

1. 适配器用部署 CA 校验 Runtime `bridge_listen` 的 TLS 服务端证书（证书 SAN 必须覆盖
   `BRIDGE_ENDPOINT` 主机名；本机验收可用 `127.0.0.1`/`localhost`）。适配器**不提供客户端
   证书**，身份由 bootstrap 凭据与随后的 `ConnectionAuthorization` 表达。
2. gRPC metadata 恰好一个 `authorization: Bearer <bootstrap_credential>`；首帧必须是
   `RegisterAdapterRequest`（含 `adapter_instance_id` 与 `RequestContext`）。
3. 成功响应返回 `Fence` + `ConnectionAuthorization` + `effective_capabilities` +
   `heartbeat_interval_seconds`；适配器校验四要素代次、60 秒授权预算、实例一致性与协商能力，
   任一项缺失/非法即失败，不回退到旧 proof 或旧行为。
4. 之后的事件、心跳、Action 结果与 refresh 帧都带完整 `Fence` 和当前 `connection_proof`。
   授权到期前（`min(10s, 生命周期/2)`）启动一次单飞
   `RefreshConnectionAuthorization`；轮换在途时不再用旧材料发送其他帧。成功则原子替换
   proof/refresh 代次；`CONFLICT`/超时/响应丢失即关闭旧流并用仍有效的 bootstrap 重连，
   产生新的 `connection_generation`；`PERMISSION_DENIED`/`UNAUTHENTICATED`（凭据撤销/失权）
   终止进程并返回非零。
5. `ShutdownNotice`：停止发送新帧，在 `drain_timeout_ms` 内等待在途事件确认，随后关闭并按
   退避重连；未确认事件保留在 outbox。未收到通知的流关闭/取消都按“未知结果”处理。

## 配置（全部为环境变量）

| 变量 | 必填 | 说明 |
| --- | --- | --- |
| `BRIDGE_ENDPOINT` | 是 | Runtime `bridge_listen`，`host:port`（TLS 服务端名默认即 host） |
| `BRIDGE_CA_FILE` | 是 | 部署 CA PEM（校验服务端证书；普通常规文件、非符号链接、≤1MiB） |
| `BRIDGE_BOOTSTRAP_CREDENTIAL_FILE` | 是 | 逐实例 bootstrap 凭据文件：常规非符号链接文件、权限必须 0600 或更严、≤4KiB；只读一次、不进日志/outbox |
| `ADAPTER_INSTANCE_ID` | 是 | Runtime 已绑定的 adapter 实例资源 ID（小写 UUIDv4；bootstrap 凭据解析出的资源必须与之一致） |
| `ACCOUNT_ID` | 是 | P06 target bridge 的 Runtime 账号绑定身份（首帧 `Account`）；真实 QQ 平台 self_id 映射与分离在 P07/M2 完成 |
| `ADAPTER_ID` | 否 | 注册帧 `adapter_id`；缺省等于 `ADAPTER_INSTANCE_ID` |
| `BRIDGE_SERVER_NAME` | 否 | TLS 服务端名覆盖（证书 SAN 与 endpoint host 不同时使用） |
| `BRIDGE_TIMEOUT` / `BRIDGE_RETRY_BASE` / `BRIDGE_IDLE_INTERVAL` | 否 | 秒；默认 5 / 1 / 1，非法即 `INVALID_ARGUMENT` |
| `OUTBOX_PATH` | 是 | 本地 outbox SQLite 路径；Action 执行日志为同路径 + `.actions` |
| `HEALTH_PORT` | 否 | 本机 `/health/live`、`/health/ready` 探针端口（默认 8081） |
| `ONEBOT_WS_URL` / `ONEBOT_ACCESS_TOKEN` | 是 | OneBot V11 正向 WS 与平台 token（平台侧，不属于 Bridge 契约） |

配置缺失、文件不可读/权限不符、endpoint/ID 非法一律以稳定码 `INVALID_ARGUMENT` 失败退 1，
不静默降级到不安全的 loopback 或匿名明文连接。

## 入站事件与 Action

- 消息/好友请求仍先映射为 `EventEnvelope` 并在本地 SQLite 持久（含会话 sequence）后才上报；
  只有 `PERSISTED`/`DUPLICATE` 才释放 outbox 项。`REJECTED` 中 `retryable=true`（含
  `RATE_LIMITED` 背压）保留未确认事件并按 `retry_after_ms` 退避重连重投；永久拒绝记录
  `failed` 与稳定码。相同 event_id 不同内容在 Runtime 侧为 `CONFLICT`，适配器不覆盖本地记录。
- Action（P07c 目标帧）：仅执行注册协商后 `effective_capabilities` 内的种类；未协商或
  参数/权限前置检查失败以 `FAILED`+`KNOWN_FAILED` 终态回传，不调用平台。Runtime 下发的
  `dispatch_attempt_id/dispatch_sequence` 缺失或非法时**不执行**并断开重连（无法关联的报告
  不伪造）；正常执行先把 attempt+内容 fingerprint 写入本地 journal 再调平台。
- 上报：平台明确成功/拒绝用 `action_result`（`SUCCEEDED`/`FAILED` + `execution_outcome`
  `KNOWN_SUCCEEDED`/`KNOWN_FAILED`，回带 attempt）；deadline 已过、未执行用
  `action_evidence`（`NOT_STARTED`+`TIMEOUT`）；平台调用超时/断线/未知结果用
  `action_evidence`（`UNKNOWN`）且**永不自动重放平台调用**。同一 action_id+内容重派发只回放
  已知结果/UNKNOWN（新 attempt 原样关联），不同内容回 `CONFLICT`。
- 确认：Runtime `action_receipt` 的 `RECORDED`/`DUPLICATE`/`EVIDENCE_ONLY` 才把 journal 标记
  已上报；`CONFLICT` 停止重发该 attempt 并把回执留作审计；无回执的结果/证据在重连（含进程
  重启恢复 `UNKNOWN`）后按原 attempt 重发。迟到结果由 Runtime 按 EXPIRED 处理为
  `EVIDENCE_ONLY`，适配器本地已观测的终态不被覆盖。
- OneBot 消息段/映射规则与 reference 相同：text/image/at/reply 映射，其它段 `UNKNOWN`；
  群/私聊由 `ConversationRef.type` 区分，会话 ID 为原生数字。适配器只使用注册账号的 Bot。

## 健康检查

`/health/live` 仅报告进程存活；`/health/ready` 在 **Runtime 注册成功且当前授权未过期（fence
当前）** 且 **平台 Bot 已连接** 时返回 200/READY，否则 503/NOT_READY。组件为
`core`（目标桥授权）、`authorization`（proof/refresh 有效）与 `platform`（OneBot 连接）。
桥断线/授权轮换失败时 ready 立即转 503，恢复注册后自动转回 200。

## 开发、测试与验收

```sh
uv sync --locked --python 3.11.18 --project adapters/nonebot
adapters/nonebot/.venv/bin/python adapters/nonebot/tools/generate_proto.py proto
cd adapters/nonebot && ./.venv/bin/python -m pytest -q
```

测试使用本机 `FakeTargetRuntime`（真实 gRPC + server-verified TLS + 可注入 ack/refresh/关闭/
协议故障）与 disposable 测试证书；`python -c ...` 需要 `openssl` 生成临时自签证书。覆盖：
注册协商与帧 fencing、bootstrap/CA/秘密文件错误、事件 ACK 三态与背压重投、refresh 轮换/
冲突/丢响应/失权、`ShutdownNotice` drain、协议违规/响应 fence 失配重连、能力协商与 Action
执行、P07 action 结果/证据/receipt/重连补报/进程崩溃恢复/重复派发/迟到回执、真实 NoneBot
子进程 + loopback OneBot 平台全链路、秘密不落日志。**真实 QQ/NapCat 验收留 P07/M2**；
Runtime 侧 P07a handler 与 P06a-2 合入后需复跑真实目标端到端；`scripts/local/*` 单机
reference MVP 冻结在 reference 分支，不随目标适配器工作。

## 初始化与生产注意

- Runtime 侧需先经 Control/bind 流程取得 `bridge_listen` 地址、CA 与一次性
  `bootstrap_credential`，并按实例资源 ID 配置本适配器；bootstrap 只在部署秘密中出现一次，
  撤销随实例停用/解绑生效。
- 证书 SAN 必须覆盖客户端实际连接的主机名；`bridge_listen` 只能部署在受控网络/防火墙后，
  不得作为匿名公网入口。
- 本机 `.venv/`、`generated/` 与临时数据库/凭据不提交；新电脑恢复锁定依赖并重新生成协议。
