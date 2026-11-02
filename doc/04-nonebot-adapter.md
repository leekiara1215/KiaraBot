# Python NoneBot2 适配器设计

2026-11-01已按用户授权完成本机NapCat安装、扫码登录与指定测试群的纯文本自动回复，
用户确认收到，Adapter执行日志与入站ACK已核对。见[实际验收](../reports/execution/2026-11-01-real-qq-onboarding.md)。
这仅覆盖reference单机群聊闭环；真实私聊、其他Action、断线恢复和回滚仍未验收。
早期[真实联调环境准备](../tests/acceptance/real-environment.md)只运行消息观察插件，
当前固定回复入口和独立数据目录见[真实QQ接入](26-real-qq-onboarding.md)，不要混用两套配置。

## T04b Action 执行子集

适配器支持 action.send_message.text.v1、action.react_message.v1、action.delete_message.v1、action.mute_member.v1、action.kick_member.v1 与 action.approve_request.v1：SendMessage 的纯文本群/私聊转换为 OneBot send_group_msg/send_private_msg；ReactMessage 的群/私聊转换为 NapCat 扩展 API set_msg_emoji_like（reaction token → emoji ID 的映射见适配器 `REACTIONS`；该 API 不属于 OneBot V11 标准动作集，真实平台行为待 T07b 验证）；DeleteMessage 转换为 delete_msg，只允许撤回本适配器记录过的已发送消息；MuteMember 转换为 set_group_ban（duration 1–2592000 秒）；KickMember 转换为 set_group_kick（`reason` 只作为 Core 审计元数据，不下发平台）；ApproveRequest 用 `request_id` 解析本机保存的好友请求 `flag` 后调用 set_friend_add_request（`reason` 不同样下发）。会话 ID 使用与事件一致的原生数字 ID（见「会话 ID 规范」）；只使用注册账号的 Bot。执行前核验目标身份、消息格式、能力和 deadline。其他操作/消息段拒绝 CAPABILITY_NOT_SUPPORTED，不降级丢字段。Core 负责业务授权，适配器不接受插件直接调用。

Action 执行日志使用独立本机 SQLite 文件（默认 OUTBOX_PATH 加 .actions），P07c 起为带 dispatch attempt 的 journal：执行前先持久登记（action_id+内容 fingerprint+attempt），只有平台明确成功/拒绝才写终态；平台调用超时、取消、断线或进程在调用中退出都保持 **UNKNOWN 证据**并永不自动重放平台调用。journal 按 action_id+fingerprint 去重，同内容重投返回已知结果/UNKNOWN、不同内容回 `CONFLICT`。结果/证据必须等 Runtime `action_receipt` 才标记已上报，否则重连（含崩溃重启）按原 attempt 重发。此策略避免重复副作用，但不能提供平台不支持的端到端 exactly-once；日志容量有上限，终态保留七天。

## P07 Adapter Action 日志与回执（P07c 适配器侧已实现，2026-11-02）

目标 Action 帧实现：`CoreToAdapter.action` 必带 Runtime 生成的 `dispatch_attempt_id`（UUIDv4）与 `dispatch_sequence`（≥1）；缺失/非法时不执行并断开重连（无法关联的报告不伪造）。执行前先在本地 journal 持久 attempt+内容 hash，再调平台。上报分两类：终态 `AdapterToCore.action_result` 只允许 `SUCCEEDED`/`FAILED`，分别带 `EXECUTION_KNOWN_SUCCEEDED`/`EXECUTION_KNOWN_FAILED` 并回带 attempt；非终态 `AdapterToCore.action_evidence` 用于 `NOT_STARTED`（deadline 已过/未执行，`last_error=TIMEOUT`）与 `UNKNOWN`（平台调用后结果未知）。`CoreToAdapter.action_receipt` 的 `RECORDED`/`DUPLICATE`/`EVIDENCE_ONLY` 把报告标记已上报；`CONFLICT` 停止重发该 attempt 并保留回执审计（避免无限重发；与契约“否则重发”的字面差异待 P07a 对齐），迟到结果由 Runtime 按 EXPIRED 记为 `EVIDENCE_ONLY`，不覆盖适配器已观测终态。崩溃重启会把 `claimed` 未完成 attempt 恢复为 UNKNOWN 证据并按原 attempt 补报；已完成结果丢失后重连仍重发；重复派发（同 attempt 或新 attempt）只回放已知结果/UNKNOWN，绝不重复调平台；旧 adapter 实例未知执行不跨实例接管。P07a Runtime 侧与 P06a-2 合入前，以上行为由 `FakeTargetRuntime` 验证；真实目标端到端属 P07 验收。

## 责任

Python 适配器是第一种渠道适配器，负责连接 NapCatQQ，并把 OneBot V11 转换成 KiaraBot 统一协议。

它只做：

- 启动 NoneBot2；
- 注册 OneBot V11 适配器；
- 连接 NapCatQQ；
- 转换事件和消息段；
- 执行 Core 下发的 Action；
- 报告连接和平台能力。

## 账号与合规风险

本适配器连接的 NapCatQQ 以第三方工具登录个人 QQ 号，存在账号被限制或封禁的风险；工程实现只能在行为层降低概率，不能在保持该渠道的前提下消除。分层评估、当前缺口和按层建议见 [账号封禁风险评估与分层控制建议](17-account-ban-risk.md)。

## 目录

该适配器由本仓库维护，位置为 `adapters/nonebot/`。下图为当前结构（2026-11-02 核对）：`src/kiarabot_adapter_nonebot/` 是 10 个平铺模块（P06c 新增 `bridge.py`），不是按子目录分层的目标结构；`generated/` 为本机生成物、不入库，运行命令见 [适配器 README](../adapters/nonebot/README.md)。

```text
adapters/nonebot/
  README.md
  pyproject.toml
  uv.lock
  src/kiarabot_adapter_nonebot/
    __init__.py
    __main__.py
    actions.py
    client.py
    health.py
    mapper.py
    outbox.py
    platform_errors.py
    status.py
  tests/
  tools/
```

适配器实现只放在 `adapters/`，不放入 `core/`、`plugins/` 或 `panel/`；后续可再考虑拆成单独插件。

它不做：

- AI 业务逻辑；
- 群组权限决策；
- Core 数据库访问；
- 插件调度；
- 控制面板页面。

## 进程边界

```text
NapCatQQ
  │ OneBot V11 / reverse WebSocket
  ▼
kiarabot-adapter-nonebot
  │ gRPC / Protobuf
  ▼
kiarabot-core
```

Python 适配器不与 C++ Core 共享数据库。它只保存本地连接缓存和必要的临时状态。

## 事件转换

OneBot 原始事件必须在适配器内完成转换：

```text
OneBot message
  → MessageReceived
  → Core

OneBot request (request_type=friend)
  → FriendRequest（request_id 为适配器作用域安全 token）
  → Core；原始 flag 只保存在适配器本机索引
```

适配器需要处理：

- 文本、图片、回复、@、文件等消息段；
- 群聊和私聊会话 ID；
- 用户、群组和账号 ID 映射；
- 平台时间戳；
- 平台原始消息 ID；
- 不支持消息段的降级表示。

原始 OneBot JSON 只能出现在适配器日志或诊断接口中，不能进入插件 SDK。

### 会话 ID 规范（T04b/W3）

`ConversationRef.id` 使用 OneBot 平台原生数字 ID（正整数字符串，≤20 位），群聊与私聊不再使用 `group:`/`private:` 前缀，改由 `ConversationRef.type`（`GROUP`/`PRIVATE`）区分。事件 `MessageReceived.conversation`、`subject.conversation_id` 与 Action `SendMessage.conversation` 使用完全相同的值，因此插件可以把收到事件的会话直接用于回复，无需解析或改写 ID。非数字或带前缀的会话 ID 在入站映射和 Action 校验中均返回 `INVALID_ARGUMENT`。`ConversationRef.type` 也参与适配器本地 outbox 的会话 sequence 键（键为 adapter/account/conversation/type 四元组），群号与私聊号相同的两个会话各自独立计数。

## Action 转换

Core 下发统一 Action：

```text
SendMessage
ReactMessage
DeleteMessage
MuteMember
KickMember
ApproveRequest
```

适配器负责把它们转换成 OneBot API 调用，并返回 `ActionResult`。权限由 Core 检查，适配器只检查平台能力和连接状态。成功结果允许 `platform_message_id` 为空（例如表情回应、撤回、禁言、踢人或审批不返回消息 ID），此时仍回传 `SUCCEEDED`；平台明确拒绝才回传 `FAILED`。`DeleteMessage` 只允许撤回适配器自己记录过的已发送消息（本机 SQLite `own_messages` 索引，保留七天），未索引或会话不匹配返回 `PERMISSION_DENIED`。`MuteMember`/`KickMember` 只做平台调用与参数校验；白名单、每日预算和权限门禁全部在 Core 执行，适配器不复制这些业务规则。`ApproveRequest` 用适配器本机 `requests` 索引把 `request_id` 解析为平台 `flag`；未知/过期 token 返回 `NOT_FOUND`，原始 flag 不离开适配器。

## 配置

P06c 起目标适配器配置为：`BRIDGE_ENDPOINT`、`BRIDGE_CA_FILE`、
`BRIDGE_BOOTSTRAP_CREDENTIAL_FILE`（0600 部署秘密，只读一次）、`ADAPTER_INSTANCE_ID`、
`ACCOUNT_ID`（P06 target bridge 使用 Runtime 账号绑定身份，必须是 Runtime 账号资源 id/UUID）、`ONEBOT_SELF_ID`（可选，OneBot/NapCat 平台数字 self_id；真实平台账号与 Runtime 资源 id 不同时必填，缺省回退 `ACCOUNT_ID`）、`OUTBOX_PATH`、`HEALTH_PORT`，可选 `ADAPTER_ID`/`BRIDGE_SERVER_NAME` 与
`BRIDGE_TIMEOUT`/`BRIDGE_RETRY_BASE`/`BRIDGE_IDLE_INTERVAL`，平台侧仍是
`ONEBOT_WS_URL`/`ONEBOT_ACCESS_TOKEN`；完整语义见适配器 README。缺失/非法/权限不符一律稳定
`INVALID_ARGUMENT` 失败，不提供 insecure 或匿名明文降级。

reference 时代的环境变量 `CORE_GRPC_ENDPOINT`/`CORE_AUTH_TOKEN` 仅存在于冻结的 reference
分支/tag，不再被目标适配器读取；NapCat WebUI 密码、OneBot Token 与 bootstrap 凭据必须分开
管理，三者都不进入日志或 outbox。

## P06 全链路验收

`ctest -R p06_client_e2e` 在真实 PG16.4 + 临时证书 + 真实 `kiarabot-runtime` 进程上运行
本适配器真实的 `BridgeClient`/`Outbox`（经 `map_message` 产生 MessageReceived）和真实
`kiarabot_sdk.Runner` 插件：注册/授权/心跳/事件 lease+Renew+ACK/证明 refresh/断线重连新
connection_generation/ShutdownNotice drain。NoneBot 平台进程、OneBot WebSocket 与真实 QQ
仍在 P07/M2 范围。

## 故障处理

- Core 不可用时，适配器持续重连但使用退避；
- 适配器通过 `AdapterStatusChanged` 上报状态迁移（Core 连接、平台连接/断开、平台动作失败），状态不变时不重复上报；
- 平台调用超时/取消/断线保留 `ACCEPTED` 未知结果，不伪造 `TIMEOUT`；执行前 deadline 已过返回 `EXPIRED` + `TIMEOUT`；
- OneBot `ActionFailed` 按 `retcode` 分类为 `RATE_LIMITED`/`PERMISSION_DENIED`/`NOT_FOUND`/`UPSTREAM_UNAVAILABLE` 后再回传 `ActionResult`，不把明确拒绝当作未知；
- 不支持的 Action 返回 `CAPABILITY_NOT_SUPPORTED`；
- 适配器重连后重新注册能力和账号；
- 不在适配器内无限缓存消息。

## 健康检查

适配器在 `127.0.0.1:HEALTH_PORT`（默认 `8081`，`HEALTH_PORT` 可配置）提供本地 HTTP 探针：`/health/live` 只表示进程存活（恒 200）；目标 Bridge 的 `/health/ready` 要求**Runtime 注册成功且当前 `ConnectionAuthorization` 未过期（fence 当前）**且**平台 Bot 已连接**，否则 503/NOT_READY。响应体 `components` 返回 `core`（目标桥授权）、`authorization`（proof/refresh 有效）与 `platform`（OneBot 连接）；桥断线/refresh 失败时 ready 立即转 503，重新注册并取得有效授权后自动转回 200。`heartbeat_interval_seconds` 来自注册响应（P06 target 为 30 秒；客户端按注册响应自适应）；适配器空闲时持续读取 Runtime 推送的 Action/回执，不等待下一次心跳。探针绑定回环地址，供同机 compose/systemd 使用。以上为目标行为；reference bridge 的旧就绪语义（仅 Core 注册）随 P06c 移除。

## 替换标准

未来的 C++ QQ 适配器只要能实现相同的：

- 注册协议；
- EventEnvelope；
- Action；
- Capability；
- 心跳和错误码；

就可以替换本适配器，Core 和插件不应感知替换。

## 验收标准

当前目标阶段以 `FakeTargetRuntime`（真实 gRPC + 测试 TLS）验证；真实 QQ/NapCat 验收留 P07/M2。

- 能接收 NapCat 的文本、图片和 @ 消息；
- 能把统一 Action 发送回群聊和私聊；
- 断线可以自动重连；
- 不导入业务插件；
- 没有对 Core 数据库的直接访问；
- 可以用模拟 Core 独立测试事件转换。

## 代码启动句

> 请只实现 Python NoneBot2 / OneBot V11 适配器进程（本仓库 `adapters/`），把 NapCat 事件转换为 `kiarabot.v1` 事件并执行 Core Action，使用模拟 gRPC Core 完成断线、重连、能力和消息段测试，不实现任何业务插件。

## T02a 持久化确认客户端要求

> 目标 P06c 起认证、注册帧与帧 fencing 以本文「P06 适配器侧冻结」为准：外部监听
> （`bridge_listen`）、bootstrap metadata、`Fence`+`connection_proof`、refresh 与
> `ShutdownNotice`；本节其余投递/ACK 语义（先持久 event+sequence、只有 PERSISTED/DUPLICATE
> 释放、RATE_LIMITED 背压保留）在目标下继续有效，但不再使用 reference 的 Bearer/token
> metadata 或 insecure loopback 连接。

连接后先发送协议版本 kiarabot.v1 的注册请求，等待 accepted=true 并保存 assigned_instance_id；每次重连重新注册。发送事件前重绑 source.adapter_instance_id，event_id/trace_id/sequence/正文保持不变。等待匹配 event_id 的 event_ack：PERSISTED 或 DUPLICATE 才删除本地待发记录。REJECTED 的 retryable=true 或连接中断时保留事件，按 retry_after_ms 和退避重连；不可重试拒绝进入本地失败记录，不得当作投递成功。队列满会回复 RATE_LIMITED 并关闭流，所有尚未确认事件必须重投。适配器对 RATE_LIMITED 一律按可重试处理（即使 Core 返回 retryable=false 也不标 failed、不丢弃），保留 pending 并退避重投；其余不可重试拒绝才进入失败记录。心跳只确认连接，不确认事件。

账号必须由 Core 管理员预先授权；注册的 capabilities 只声明可执行的 Action 能力（P06 target 的六个 action.*），事件上报不是协商能力：认证连接上的 MessageReceived/FriendRequest/AdapterStatusChanged 由 Runtime inbox 接收，插件订阅按插件 manifest 单独协商。capabilities 不能授予账号权限。适配器不得发 TimerTriggered/PluginStatusChanged。当前任务只实现 Core 端及契约；Python 适配器客户端在 T02b 实现。

## T02b 适配器实现

当前实现维护于 `adapters/nonebot/`；配置、锁文件和测试命令见该目录 README。后续可再考虑拆成单独插件。当前使用 NoneBot websockets driver 和 onebot_ws_urls 连接 NapCat 正向 WebSocket，反向连接尚未配置。

消息先进入本地有界 outbox 并原子分配会话 sequence；本地 outbox DB 由 user_version 1 迁移到 2，旧共享计数器保守复制到群/私聊两类（sequence 相同，空会话归入 type 0），pending/failed 行不动，未知版本 fail-closed 为 INTERNAL；等待 Core 确认期间保留 pending，重连注册后重新绑定 instance 并重投。永久拒绝保留 failed 记录。会话 ID 使用平台原生数字 ID（见上方「会话 ID 规范」），事件与 Action 共用同一值。注册响应心跳间隔、健康探针与 AdapterStatusChanged 上报见上方「健康检查」与「故障处理」。当前仅接收群/私聊消息及 text/image/at/reply，未知段降级；Action 纯文本发送能力见上方 T04b。pending 上限为 10000 条、64 MiB、单消息 1 MB；已确认/失败记录属于去重与诊断索引，不占 pending 上限，7 天后清理（总量仍有上限，极端负载下去重窗口可能提前收缩），水位不回退；pending 达到上限明确失败。平台到 outbox 之前没有消费 ACK，真实可靠性仍需 T07b 验证。

### 目标认证适配器目录读取（未实现）

Runtime GetAdapter/ListAdapters读取稳定adapter实例的最后有效实现/协议/声明与有效能力、获准绑定及当前online/Fence，不从ID猜实现类型。断线保留metadata但不带current_fence；声明能力不授予权限。API新adapter-instances是目标实例目录，历史/adapters reference视图不是目标生产桥接。adapter.read与exact实例/目录及child grant当前核验，返回关联引用不授予关联账号。适配器生产/消费实现未因schema声明改变。

## 外部 Bridge 连接授权（目标未实现）

采用已批准的最小三载荷 ConnectionAuthorization / RefreshConnectionAuthorization / ConnectionAuthorizationResult，唯一来源 platform.proto 与 adapters.proto/plugins.proto。Adapter/Plugin 成功登记必须带 authorization，且与响应 Fence 一致；注册后入站帧必须带完整 Fence 和当前 connection_proof（续期正文 proof/Fence 也须与帧一致）。反向 refresh 结果按 request_id 返回恰好一个新 authorization 或稳定 Error，不能空成功。缺少 proof/授权不允许旧流回退。

proof/refresh_credential 是独立 CSPRNG 至少256位 opaque秘密，绑定实际已认证 TLS 流、principal、完整 Fence、Runtime epoch、credential_version；不能把客户端自报证书/Fence 当绑定。Runtime只保存摘要和当前受信连接状态，不把秘密写入公开投影、日志或outbox。bootstrap仅登记换取受限证明，不授权Action/Model/存储等业务。内部受限委托仍是Ed25519 JWT，平台owner监听仍mTLS；TLS-only外部Bridge传输接线属于P05/P06，不修改P02监听行为。

issued_at/expires_at 为受信UTC，名义寿命最多56秒；含既定2秒节点偏差和最多2秒处理容差总预算不超过60秒。服务端同时以单调期限和受信UTC约束，每秒检查到期、撤销、凭据版本和资源gate；客户端到期前10秒单飞续期。高风险操作仍逐次在线授权。Identity不可用不能刷新或无限延长旧流，停止新副作用并返回UPSTREAM_UNAVAILABLE；不伪造PERMISSION_DENIED。

refresh只接受当前实际流、未过期未撤销的proof/refresh及正refresh_generation，在线验证当前Identity主体/凭据状态与gate后原子轮换二者和generation；旧材料不可再次换发。材料/流/Fence失配或过期UNAUTHENTICATED并关闭流，已认证请求generation冲突CONFLICT，明确失权PERMISSION_DENIED，依赖故障UPSTREAM_UNAVAILABLE，期限TIMEOUT。失败不轮换、不复活失效授权。refresh无持久秘密收据；响应丢失视授权未知，关闭旧流，经仍有效的登记凭据重新bootstrap产生新connection_generation，禁止重放旧refresh猜测结果。

授权过期停止新投递、入站受理、续租和副作用，不把已有Action/Model事实变FAILED。auth刷新不续Delivery租约；ACK/Renew仍核验有效attempt、完整Fence及现有事实规则。适配器本地已执行结果在断线后通过新认证连接按原account/action ID交回，不能据失效proof新执行。所有新增字段/帧target均not_implemented，oracle和wire往返不证明生产授权或并发正确性。

### P06 适配器侧冻结（未实现）

外部适配器在 P06 只连 Runtime 的外部 Bridge 监听（`bridge_listen`，server-verified TLS，不要求客户端证书）：部署一次交付的 `bootstrap_credential`（来自 `RuntimeBindAdapterResponse.bootstrap_credential`）经 gRPC metadata `authorization: Bearer <credential>` 提交，首帧 `RegisterAdapterRequest.adapter_instance_id` 必须与凭据解析出的 adapter 资源一致；成功响应带 `Fence`+`ConnectionAuthorization`，之后事件/心跳/refresh 帧都带完整 `Fence` 与当前 `connection_proof`。事件仍须先在本地 SQLite 持久 event+sequence 再上报；只有 `PERSISTED`/`DUPLICATE` 才释放 outbox 项，`REJECTED` 中 `retryable=true` 保留重投，容量背压 `RATE_LIMITED` 可重试并关闭本次流（适配器保留未确认事件）。bootstrap/proof/refresh 明文不得写入 outbox、日志或错误信息；不实现自报客户端证书或旧 proof 回退。适配器的 TLS/bootstrap/帧改造属 P06c：**第一阶段已实现**（server-verified TLS、bootstrap metadata 注册、
`Fence`+`connection_proof` 每帧、单飞 refresh、事件 ACK/背压、`ShutdownNotice`、能力协商与
`/health/ready` 授权门禁；测试见 `adapters/nonebot/tests/{test_bridge,test_client,test_nonebot_process}.py`
与 `FakeTargetRuntime`；P07c Action dispatch/evidence/receipt/journal 已在该夹具验证）。未完成项：
真实 QQ/NapCat 回归（P07/M2）、P07a Runtime 侧与 P06a-2 Bridge handler 就绪后的真实目标端到端
验收（含 receipt CONFLICT 字面语义与真实 Runtime 核对）。unary RuntimeService 注册/事件方法不供
适配器使用。

## P07d 真实 Adapter + fake OneBot + 真实 Runtime 全链路验收（2026-11-02）

`tests/architecture/p07_adapter_e2e.py` 启动真实 `kiarabot-runtime` 进程（真 PG16.4 + mTLS AdapterBridge/PluginBridge）、真实 NoneBot adapter 子进程和 loopback fake OneBot v11 WS 平台：fake 平台发送群消息事件并实现 `send_group_msg`/`send_private_msg`/`delete_msg`/`set_group_ban`/`set_group_kick`/`set_friend_add_request` 的成功/失败响应；真实 adapter 执行 Action 后把 `action_result`/`action_evidence` 与幂等 attempt 上报 Runtime，Runtime 记账并回 `action_receipt`。脚本覆盖成功、平台 retcode 失败（RATE_LIMITED 等稳定码→KNOWN_FAILED，绝不伪成功）、重复平台帧（Runtime event 去重、不新增 Action）、dispatch 后取消（CONFLICT 且不改写终态）、平台 WS 断线与 adapter 重连后继续执行且不重复平台调用；所有真实进程断言均在 CTest `p07_adapter_e2e`（labels `p07;integration;fault`）中运行。

真实 QQ/NapCat 未在本测试中使用：平台账号 self_id 与 Runtime 账号资源 UUID 的分离通过 `ONEBOT_SELF_ID` 配置解决；真实 QQ 的审批/撤回/禁言/踢出凭证、NapCat 扩展 API（如 `set_msg_emoji_like`）和真实网络/封禁风险仍属 M2/T07b，不在本测试的通过范围内。

P07g（2026-11-02）：真实 NoneBot adapter 进程 + fake OneBot WS + 真实 Runtime Bridge 的 `p07_adapter_e2e` 已纳入 P07 退出证据（成功、平台 retcode 失败、重复帧、dispatch 后取消 CONFLICT、平台断线重连无重复调用）。真实 QQ/NapCat 平台 login/撤回/禁言/踢出/审批凭证与 reaction 扩展 API 仍属 M2/T07b 用户环境验收；adapter 侧领域/单测覆盖不等同真实平台副作用。

## P06g 真实总线全链路验收（2026-11-02）

`p07_adapter_e2e` 已扩展为单一真实进程链验收：真 PG16.4、真 mTLS Runtime、真
`kiarabot-adapter`（NoneBot）+ loopback fake OneBot WS、以及**独立进程**的真实 SDK plugin
（`python -m kiarabot_sdk` 的受管子进程，通过真实 PluginBridge 注册/订阅/提交 Action）。
覆盖：平台消息→adapter 事件→Runtime accept→订阅投递（delivery target/lease/ACK）→plugin
收到事件→plugin 提交 send_message + policy mute Action（action_policies 真 PG 播种）→
Runtime 派发 adapter→fake OneBot `send_group_msg`/`set_group_ban`→result/evidence/receipt→
plugin 查询终态；重复平台帧不重复建事件/Action；adapter 崩溃重启后 Action 不重复执行；
非订阅/离线插件无 delivery target；post-dispatch cancel CONFLICT。仍未覆盖：真实
QQ/NapCat（M2 环境项）与单链 UNKNOWN/EXPIRED late-evidence（手工 wire fixture
`p06_bridge_acceptance` 已有，待后续合并到单链）。
