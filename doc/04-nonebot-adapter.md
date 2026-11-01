# Python NoneBot2 适配器设计

T07b 真实 QQ/NapCat 尚待用户配置；已核对适配器检出当前 OneBot 正向 WS 与 Core 环境变量，准备步骤见 [真实联调环境准备](../tests/acceptance/real-environment.md)。模板只运行消息观察插件，不主动发送；真实收发和回滚仍未验证。

## T04b Action 执行子集

适配器支持 action.send_message.text.v1、action.react_message.v1、action.delete_message.v1、action.mute_member.v1、action.kick_member.v1 与 action.approve_request.v1：SendMessage 的纯文本群/私聊转换为 OneBot send_group_msg/send_private_msg；ReactMessage 的群/私聊转换为 NapCat 扩展 API set_msg_emoji_like（reaction token → emoji ID 的映射见适配器 `REACTIONS`；该 API 不属于 OneBot V11 标准动作集，真实平台行为待 T07b 验证）；DeleteMessage 转换为 delete_msg，只允许撤回本适配器记录过的已发送消息；MuteMember 转换为 set_group_ban（duration 1–2592000 秒）；KickMember 转换为 set_group_kick（`reason` 只作为 Core 审计元数据，不下发平台）；ApproveRequest 用 `request_id` 解析本机保存的好友请求 `flag` 后调用 set_friend_add_request（`reason` 不同样下发）。会话 ID 使用与事件一致的原生数字 ID（见「会话 ID 规范」）；只使用注册账号的 Bot。执行前核验目标身份、消息格式、能力和 deadline。其他操作/消息段拒绝 CAPABILITY_NOT_SUPPORTED，不降级丢字段。Core 负责业务授权，适配器不接受插件直接调用。

Action 执行日志使用独立本机 SQLite 文件（默认 OUTBOX_PATH 加 .actions），按 action_id 和内容 fingerprint 去重；执行前持久化占用，终态保存后才回传。正常重投返回原结果。平台调用超时、取消或进程在调用后退出时保留 ACCEPTED/未知，不重复调用平台；Core 可查询并最终按 deadline 过期。此策略避免重复副作用，但不能提供平台不支持的端到端 exactly-once。无已知平台结果时不伪造 FAILED。日志容量有上限，终态保留七天，未终态不自动删除。

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

该适配器由本仓库维护，位置为 `adapters/nonebot/`。下图为目标结构；当前具体文件和运行命令见 [适配器 README](../adapters/nonebot/README.md)：

```text
adapters/nonebot/
  pyproject.toml
  src/kiarabot_adapter_nonebot/
    onebot/
    event_mapper/
    action_executor/
    grpc_client/
    health/
    main.py
  tests/
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

适配器所需配置：

```text
CORE_GRPC_ENDPOINT
CORE_AUTH_TOKEN
ONEBOT_WS_URL
ONEBOT_ACCESS_TOKEN
ACCOUNT_ID
```

NapCat 的 WebUI 密码、OneBot Token 和 Core Token 必须分开管理。

## 故障处理

- Core 不可用时，适配器持续重连但使用退避；
- 适配器通过 `AdapterStatusChanged` 上报状态迁移（Core 连接、平台连接/断开、平台动作失败），状态不变时不重复上报；
- 平台调用超时/取消/断线保留 `ACCEPTED` 未知结果，不伪造 `TIMEOUT`；执行前 deadline 已过返回 `EXPIRED` + `TIMEOUT`；
- OneBot `ActionFailed` 按 `retcode` 分类为 `RATE_LIMITED`/`PERMISSION_DENIED`/`NOT_FOUND`/`UPSTREAM_UNAVAILABLE` 后再回传 `ActionResult`，不把明确拒绝当作未知；
- 不支持的 Action 返回 `CAPABILITY_NOT_SUPPORTED`；
- 适配器重连后重新注册能力和账号；
- 不在适配器内无限缓存消息。

## 健康检查

适配器在 `127.0.0.1:HEALTH_PORT`（默认 `8081`，`HEALTH_PORT` 可配置）提供本地 HTTP 探针：`/health/live` 只表示进程存活（恒 200）；`/health/ready` 以 Core 注册连接为就绪条件（未注册返回 503），响应体 `components` 分别返回 `core` 与 `platform` 状态，平台未连接时为 `DEGRADED` 但不阻止就绪。注册响应的 `heartbeat_interval_seconds` 决定空闲心跳间隔；适配器空闲时持续读取 Core 推送的 Action，不等待下一次心跳。探针绑定回环地址，供同机 compose/systemd 使用。

## 替换标准

未来的 C++ QQ 适配器只要能实现相同的：

- 注册协议；
- EventEnvelope；
- Action；
- Capability；
- 心跳和错误码；

就可以替换本适配器，Core 和插件不应感知替换。

## 验收标准

- 能接收 NapCat 的文本、图片和 @ 消息；
- 能把统一 Action 发送回群聊和私聊；
- 断线可以自动重连；
- 不导入业务插件；
- 没有对 Core 数据库的直接访问；
- 可以用模拟 Core 独立测试事件转换。

## 代码启动句

> 请只实现 Python NoneBot2 / OneBot V11 适配器进程（本仓库 `adapters/`），把 NapCat 事件转换为 `kiarabot.v1` 事件并执行 Core Action，使用模拟 gRPC Core 完成断线、重连、能力和消息段测试，不实现任何业务插件。

## T02a 持久化确认客户端要求

连接后先发送协议版本 kiarabot.v1 的注册请求，等待 accepted=true 并保存 assigned_instance_id；每次重连重新注册。发送事件前重绑 source.adapter_instance_id，event_id/trace_id/sequence/正文保持不变。等待匹配 event_id 的 event_ack：PERSISTED 或 DUPLICATE 才删除本地待发记录。REJECTED 的 retryable=true 或连接中断时保留事件，按 retry_after_ms 和退避重连；不可重试拒绝进入本地失败记录，不得当作投递成功。队列满会回复 RATE_LIMITED 并关闭流，所有尚未确认事件必须重投。适配器对 RATE_LIMITED 一律按可重试处理（即使 Core 返回 retryable=false 也不标 failed、不丢弃），保留 pending 并退避重投；其余不可重试拒绝才进入失败记录。心跳只确认连接，不确认事件。

账号必须由 Core 管理员预先授权；注册的 capabilities 仅描述平台支持能力，不能授予账号权限。适配器不得发 TimerTriggered/PluginStatusChanged。当前任务只实现 Core 端及契约；Python 适配器客户端在 T02b 实现。

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
