# NoneBot2 适配器模块设计

本文描述 `adapters/nonebot/`（Python NoneBot2 / OneBot V11 渠道适配器）如何把 NapCatQQ 平台事件转换为 KiaraBot 统一协议，并执行 Runtime 下发的 Action。跨进程契约唯一来源是 `proto/kiarabot/v1/adapters.proto`、`proto/kiarabot/v1/platform.proto` 与 `proto/kiarabot/v1/services.proto`；实现位于 `adapters/nonebot/src/kiarabot_adapter_nonebot/`，目标验收脚本为 `tests/architecture/p06_client_e2e.py` 与 `tests/architecture/p07_adapter_e2e.py`。模块在整体架构中的位置见[架构总览](../architecture.md)。

## 1 模块定位与职责

适配器是第一种渠道适配器，负责连接 NapCatQQ，并把 OneBot V11 转换成 KiaraBot 统一协议。它只做：启动 NoneBot2、注册 OneBot V11 适配器、连接 NapCatQQ、转换事件与消息段、执行 Core 下发的 Action、上报连接与平台能力状态。

它不做：AI 业务逻辑、群组权限决策、Core 数据库访问、插件调度、控制面板页面。业务授权、限流、白名单和审计全部在 Core；适配器只校验平台身份、参数和能力，不复制业务规则。

NapCatQQ 通过第三方方式登录个人 QQ 号，存在平台限制或封禁风险；工程实现只能在行为层降低概率，不能消除。真实 QQ 平台验收仍属 M2/T07b。

## 2 边界与依赖

- 只通过 `proto/` 定义的类型与 Runtime `AdapterBridge.Connect` 通信；不导入任何业务插件，不访问 Runtime 数据库。
- 平台对象（OneBot JSON、raw `flag`、平台消息 ID）只存在于适配器进程；上报给 Core 的是统一 `EventEnvelope`/`ActionResult`/`ActionExecutionEvidence`。
- 适配器拥有平台连接状态、原始 ID 映射与本地可靠发送/执行日志；不拥有 Core 的任何权威业务状态。
- 依赖固定为 Python 3.11、`nonebot2[websockets]`、`nonebot-adapter-onebot`、`grpcio`、`protobuf`，见 `adapters/nonebot/pyproject.toml` 与 `adapters/nonebot/uv.lock`。
- 生成代码位于 `adapters/nonebot/generated/`，不提交、不手改；生成入口为 `adapters/nonebot/tools/generate_proto.py`。
- 模型、任务、存储等能力归 PluginSDK；适配器从不向插件暴露平台 API。

## 3 进程/组件结构

```text
NapCatQQ ── OneBot V11 正向 WebSocket ──► NoneBot2 进程
                                           │ EventEnvelope（本地 outbox）
                                           ▼
                                Runtime AdapterBridge.Connect（server-verified TLS）
```

`adapters/nonebot/src/kiarabot_adapter_nonebot/` 为平铺模块结构：

| 模块 | 职责 |
| --- | --- |
| `__main__.py` | NoneBot 初始化、onebot v11 适配器、事件/请求处理器、健康探针与进程入口 |
| `bridge.py` | 目标桥配置、`Fence`/`ConnectionAuthorization`、TLS 与秘密文件校验、注册帧 |
| `client.py` | `AdapterBridge.Connect` 会话：注册、帧 fencing、单飞 refresh、心跳、事件发送、Action 接收 |
| `mapper.py` | OneBot 消息/好友请求 → `EventEnvelope`，平台 ID 与会话类型校验 |
| `outbox.py` | 本地有界 SQLite 事件 spool（会话 sequence、pending/failed/acked） |
| `actions.py` | 目标 Action 执行器：dispatch journal、幂等、结果/证据上报与回执 |
| `health.py` | 回环 HTTP `/health/live` 与 `/health/ready` |
| `status.py` | 通过持久 outbox 上报 `AdapterStatusChanged` 状态迁移 |
| `platform_errors.py` | OneBot `retcode` → 稳定 `ErrorCode` 分类 |

适配器为单进程单事件循环：outbox 与 action journal 都只有单一 owner；Runtime 帧由 reader 持续读取，空闲时仍能接收 Action/回执。

## 4 数据模型与持久化

适配器不保存 Core 业务数据，只保留本地可靠发送/执行责任：

- **事件 outbox**（`OUTBOX_PATH`，SQLite schema v2）：`sessions(key, sequence)` 与 `outbox(position, id UNIQUE, fingerprint, payload, status pending|acked|failed, error, updated)`。会话键为 `adapter/account/conversation/type` 四元组；同一 event_id 不同内容在本地为 `CONFLICT`。
- outbox 上限：pending 10000 条、64 MiB，单消息 ≤1 MiB；超限返回 `RATE_LIMITED`；acked/failed 行保留 7 天后清理，只作去重与诊断，不占 pending 上限；总行数另有 4 倍硬上限。
- v1 旧会话键迁移到 v2 时，同一原生 ID 的群/私聊各自继承原水位（只可能高估，不会乱序）；未知版本 fail-closed 为 `INTERNAL`。
- **Action journal**（`OUTBOX_PATH + ".actions"`，SQLite schema v4）：`actions(id, fingerprint, state claimed|terminal|unknown|not_started, result, evidence, attempt_id, sequence)`、`action_reports(action_id, attempt_id, kind, payload, state, receipt)`、`own_messages`（本 bot 已发消息，用于撤回校验）、`requests`（好友请求 `request_id → flag`，7 天）。旧 v3 日志就地迁移；未知版本拒绝启动。
- 不持久化 bootstrap、proof、refresh、OneBot token 或平台 access token；outbox 只包含平台中立的 `EventEnvelope`。

## 5 关键流程/状态机

注册与授权：以部署 CA 建立 server-verified TLS（适配器不出示客户端证书，无 insecure 回退），gRPC metadata 只带一个 `authorization: Bearer <bootstrap>`；首帧 `RegisterAdapterRequest` 含 `adapter_instance_id`、`RequestContext`、`Account` 与声明的 Action 能力。成功响应必须同时通过：`required_protocol_version == kiarabot.v1`、`assigned_instance_id == ADAPTER_INSTANCE_ID`、`Fence` 与 `ConnectionAuthorization` 完整且 fence 一致、`principal_id` 等于实例、心跳间隔 1–300 秒；`effective_capabilities` 是后续可执行 Action 的唯一种类来源。之后每帧携带完整 fence + 当前 `connection_proof`。

授权轮换：在到期前 `min(10s, lifetime/2)` 发起单飞 `RefreshConnectionAuthorization`；轮换在途时不再用旧材料发送其他帧，成功后原子替换 proof/refresh 与 generation；`CONFLICT`/超时/响应丢失关闭旧流并用仍有效 bootstrap 重连，产生新 `connection_generation`；`PERMISSION_DENIED`/`UNAUTHENTICATED`（撤销/失权）终止进程并非零退出。

事件路径：NoneBot 收到消息/好友请求后先映射：`self_id` 必须等于本 bot 平台 ID（不一致为 `PERMISSION_DENIED`），消息段支持 text/image/at/reply，其他段降级为 `UNKNOWN`；会话 ID 使用 OneBot 原生数字 ID，由 `ConversationRef.type` 区分群/私聊；超出 1024 段或 1 MiB 为 `INVALID_ARGUMENT`。映射结果先在本地 outbox 原子分配会话 sequence 并持久化，才上报 Runtime；只有 `PERSISTED`/`DUPLICATE` 才释放 outbox 项。`REJECTED` 中 `retryable=true` 或 `RATE_LIMITED` 背压保留 pending，按 `retry_after_ms` 退避重连重投；永久拒绝记录 failed 与稳定码。

Action 路径：Runtime 下发的 `CoreToAdapter.action` 必须带 Runtime 生成的 `dispatch_attempt_id`（UUIDv4）与 `dispatch_sequence`（≥1），缺失/非法时不执行并断开重连。执行前先校验目标 adapter/account/instance、deadline、协商能力与 payload 形状，再把 attempt+内容 fingerprint 写入本地 journal 才调用平台。平台明确成功/拒绝用 `action_result`（`SUCCEEDED`/`FAILED` + `KNOWN_*`，回带 attempt）上报；deadline 已过用 `action_evidence`（`NOT_STARTED` + `TIMEOUT`）；平台超时/断线/结果不明用 `action_evidence`（`UNKNOWN`），永不自动重放平台调用。Runtime `action_receipt` 的 `RECORDED`/`DUPLICATE`/`EVIDENCE_ONLY` 才标记已上报；`CONFLICT` 停止重发该 attempt 并保留回执审计。进程重启时 `claimed` 未完成 attempt 恢复为 UNKNOWN 并按原 attempt 补报；同 action_id+内容重派发只回放已知结果/UNKNOWN，不同内容回 `CONFLICT`。

状态机与生命周期：适配器进程启动后 NoneBot 连接平台、BridgeClient 连接 Runtime；drain 时 `ShutdownNotice` 触发停止新帧、等待在途事件确认后关闭并按退避重连（未确认事件保留在 outbox）。平台连接/断开、桥连接/断开以 `AdapterStatusChanged` 迁移事件上报，状态不变不重复上报。

## 6 接口与契约

gRPC：`services.AdapterBridge/Connect(stream AdapterToCore) returns (stream CoreToAdapter)`（`proto/kiarabot/v1/services.proto`）。

- `AdapterToCore`：`register`、`event`、`heartbeat`、`action_result`、`action_evidence`、`refresh_authorization`；每帧 `fence` + `connection_proof`（注册除外）。
- `CoreToAdapter`：`register_result`、`action`、`heartbeat_ack`、`event_ack`、`shutdown`、`authorization_result`、`action_receipt`；同样带当前 fence。
- Action 能力名：`action.send_message.text.v1`、`action.react_message.v1`、`action.delete_message.v1`、`action.mute_member.v1`、`action.kick_member.v1`、`action.approve_request.v1`。
- `AdapterEventAck` 的 disposition 为 `PERSISTED`/`DUPLICATE`/`REJECTED`（含 `retry_after_ms`），只表示 inbox 接收，不等于插件 ACK 或 Action 完成。
- Action 参数映射：SendMessage→`send_group_msg`/`send_private_msg`（仅纯文本 text 段）；ReactMessage→NapCat 扩展 `set_msg_emoji_like`（reaction token→emoji ID 见 `REACTIONS`）；DeleteMessage→`delete_msg`（仅 `own_messages` 索引内、会话匹配）；MuteMember→`set_group_ban`（duration 1–2592000 秒）；KickMember→`set_group_kick`；ApproveRequest→`set_friend_add_request`（`flag` 由适配器 `requests` 索引解析）。
- 配置（环境变量）：必填 `BRIDGE_ENDPOINT`、`BRIDGE_CA_FILE`、`BRIDGE_BOOTSTRAP_CREDENTIAL_FILE`、`ADAPTER_INSTANCE_ID`、`ACCOUNT_ID`、`OUTBOX_PATH`、`ONEBOT_WS_URL`、`ONEBOT_ACCESS_TOKEN`；可选 `ADAPTER_ID`、`BRIDGE_SERVER_NAME`、`BRIDGE_TIMEOUT`（默认 5，0–120）、`BRIDGE_RETRY_BASE`（默认 1，0–30）、`BRIDGE_IDLE_INTERVAL`（默认 1，0–30）、`HEALTH_PORT`（默认 8081）、`ONEBOT_SELF_ID`（平台数字 self_id，缺省回退 `ACCOUNT_ID`）。
- 部署文件：CA 文件为普通非符号链接、≤1 MiB、含 PEM 证书；bootstrap 为常规非符号链接、权限 0600 或更严、≤4 KiB、只读一次。

## 7 权限与安全

- 传输层只有 server-verified TLS：用部署 CA 校验 Runtime 服务端证书，SAN 必须覆盖连接主机名；缺失/非法 CA 或 bootstrap 以 `INVALID_ARGUMENT` 失败，无 loopback/匿名明文降级。
- bootstrap/proof/refresh 只在内存，字段 `repr=False`，不写 outbox、日志或错误文本；`ADAPTER_INSTANCE_ID` 必须为小写 UUIDv4，且与 bootstrap 凭据解析出的资源一致。
- 适配器能力声明只是申请；Runtime 只接受声明并由注册响应返回的 `effective_capabilities`。适配器不检查业务权限，只检查平台能力与参数。
- `DeleteMessage` 只允许撤回本适配器记录过的已发送消息；`ApproveRequest` 的原始 `flag` 不离开适配器；`KickMember.reason` 只作 Core 审计元数据，不下发平台。
- NoneBot/Loguru 日志经过滤器清空 message 与异常正文，避免平台 payload、URL 或 token 泄漏。
- NapCat WebUI 密码、OneBot Token、bootstrap 凭据必须分开管理。

## 8 错误/失败/恢复语义

- 注册/连接错误：`UNAUTHENTICATED`、`PERMISSION_DENIED`、`INVALID_ARGUMENT`、`CAPABILITY_NOT_SUPPORTED`、`PLUGIN_UNAVAILABLE` 视为 fatal，终止进程并保留 outbox；其他依赖故障按 `min(30s, retry_base × 2^(n-1))` 加 0.8–1.0 抖动退避重连，失败计数上限 6。
- 平台调用已发出但结果不明（超时、取消、断线、进程退出）保持 `UNKNOWN` 证据，不伪造失败、不重放平台调用；平台明确拒绝才回终态。
- OneBot `retcode` 分类：1200/1201→`RATE_LIMITED`；200/1401/1403→`PERMISSION_DENIED`；1404/2000→`NOT_FOUND`；其他（含非整数）→`UPSTREAM_UNAVAILABLE`。
- outbox 满返回 `RATE_LIMITED` 且不丢已持久事件；无法重试的拒绝进入 failed 记录；outbox 读写故障为 `UPSTREAM_UNAVAILABLE`。
- 健康门禁：桥授权过期/refresh 失败或平台断开时 ready 立即 503；重新注册并取得有效授权后自动恢复 200；健康端口被占用不会杀死适配器，只记录错误。
- 退出码：配置/平台 URL/凭据非法退出 1；fatal Bridge 失败经正常 shutdown 后非零退出。

## 9 可观测性与运维

- 本机回环 HTTP 探针（`127.0.0.1:HEALTH_PORT`）：`/health/live` 恒 200 表示进程存活；`/health/ready` 要求“Runtime 注册成功且当前授权未过期（fence 当前）”且“平台 Bot 已连接”，响应体给出 `core`、`authorization`、`platform` 组件状态。
- 日志字段为 `operation=... error_code=<稳定码>`，不输出平台正文、token 或凭据；平台登录/连接状态通过 `AdapterStatusChanged` 上报 Core。
- 部署：Runtime 侧需先经 Control/bind 流程取得 `bridge_listen` 地址、部署 CA 与一次性 bootstrap 凭据；`bridge_listen` 只能位于受控网络/防火墙后，不能作为匿名公网入口。
- 运行与本地验收：`uv sync --locked --python 3.11.18 --project adapters/nonebot`，生成协议后以部署环境变量启动 `python -m kiarabot_adapter_nonebot`；测试见下节。
- 适配器崩溃/重启由运维拉起；已持久 outbox 与 action journal 保证待确认事件与未上报结果恢复，不依赖 Core 重放平台调用。

## 10 测试与验收证据

- 适配器单元/协议测试：`adapters/nonebot/tests/test_bridge.py`、`test_client.py`、`test_actions.py`、`test_mapping_outbox.py`、`test_health.py`、`test_platform_errors.py`、`test_nonebot_process.py`；TLS 夹具为 `adapters/nonebot/tests/tls_fixture.py`，目标 Runtime 替身为 `adapters/nonebot/tests/fake_target_runtime.py`（真实 gRPC + server-verified TLS + 可注入 ack/refresh/关闭/协议故障）。
- 覆盖点：注册协商与帧 fencing、bootstrap/CA/秘密文件错误、事件 ACK 三态与背压重投、refresh 轮换/冲突/丢响应/失权、`ShutdownNotice` drain、协议违规与响应 fence 失配重连、能力协商与 Action 执行、Action 结果/证据/receipt、重连补报、进程崩溃恢复、重复派发与迟到回执、真实 NoneBot 子进程 + loopback OneBot 平台全链路、秘密不落日志。
- 全链路 CTest（注册于 `infrastructure/postgres/CMakeLists.txt`）：`p06_client_e2e` 在真实 PG16.4 + TLS + 真实 `kiarabot-runtime` 上运行本适配器真实 `BridgeClient`/`Outbox` 与真实 SDK Runner；`p06_bridge_acceptance` 用手工 wire 客户端覆盖 Bridge 边界；`p07_adapter_e2e` 运行真实适配器子进程 + fake OneBot WS + 真实 Runtime，覆盖平台 retcode 失败、重复帧、dispatch 后取消 `CONFLICT`、平台断线重连后不重复调用。
- 本地测试命令：`cd adapters/nonebot && ./.venv/bin/python -m pytest -q`。

## 11 已知限制与后续

- 真实 QQ/NapCat 的目标栈验收未完成（P07/M2/T07b）：仓库仅完成过 reference 单机纯文本群聊首轮实测，真实私聊、审批/撤回/禁言/踢出凭证、表情回应扩展 API 与真实封禁风险仍属环境项。
- 当前使用 NoneBot websockets driver 的正向 WebSocket（`ONEBOT_WS_URL`）；反向连接未配置。
- 平台帧进入本地 outbox 之前没有消费 ACK，平台→适配器这一段可靠性仍需真实平台验证。
- Action journal 有容量上限；终态保留 7 天，极端负载下去重窗口可能提前收缩。
- 回执 `CONFLICT` 的停止重发行为与契约字面“否则重发”的差异待 P07a/真实 Runtime 对齐；迟到结果按 EXPIRED 处理需真实链路复核。
- 适配器是 Python 实现；未来 C++ 适配器只要实现相同的注册协议、`EventEnvelope`、Action、能力、心跳与错误码即可替换，Core 与插件不应感知替换。
