# Python NoneBot2 适配器设计

T07b 真实 QQ/NapCat 尚待用户配置；已核对适配器检出当前 OneBot 正向 WS 与 Core 环境变量，准备步骤见 [真实联调环境准备](../tests/acceptance/real-environment.md)。模板只运行消息观察插件，不主动发送；真实收发和回滚仍未验证。

## T04b Action 执行子集

适配器支持 action.send_message.text.v1，将 send_message 的纯文本群/私聊转换为 OneBot send_group_msg/send_private_msg；会话 ID 使用与事件一致的原生数字 ID（见「会话 ID 规范」）；只使用注册账号的 Bot。执行前核验目标身份、消息格式、能力和 deadline。其他操作/消息段拒绝 CAPABILITY_NOT_SUPPORTED，不降级丢字段。Core 负责业务授权，适配器不接受插件直接调用。

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

`ConversationRef.id` 使用 OneBot 平台原生数字 ID（正整数字符串，≤20 位），群聊与私聊不再使用 `group:`/`private:` 前缀，改由 `ConversationRef.type`（`GROUP`/`PRIVATE`）区分。事件 `MessageReceived.conversation`、`subject.conversation_id` 与 Action `SendMessage.conversation` 使用完全相同的值，因此插件可以把收到事件的会话直接用于回复，无需解析或改写 ID。非数字或带前缀的会话 ID 在入站映射和 Action 校验中均返回 `INVALID_ARGUMENT`。

## Action 转换

Core 下发统一 Action：

```text
SendMessage
MuteMember
KickMember
ApproveRequest
```

适配器负责把它们转换成 OneBot API 调用，并返回 `ActionResult`。权限由 Core 检查，适配器只检查平台能力和连接状态。

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

消息先进入本地有界 outbox 并原子分配会话 sequence；等待 Core 确认期间保留 pending，重连注册后重新绑定 instance 并重投。永久拒绝保留 failed 记录。会话 ID 使用平台原生数字 ID（见上方「会话 ID 规范」），事件与 Action 共用同一值。注册响应心跳间隔、健康探针与 AdapterStatusChanged 上报见上方「健康检查」与「故障处理」。当前仅接收群/私聊消息及 text/image/at/reply，未知段降级；Action 纯文本发送能力见上方 T04b。pending 上限为 10000 条、64 MiB、单消息 1 MB；已确认/失败记录属于去重与诊断索引，不占 pending 上限，7 天后清理（总量仍有上限，极端负载下去重窗口可能提前收缩），水位不回退；pending 达到上限明确失败。平台到 outbox 之前没有消费 ACK，真实可靠性仍需 T07b 验证。
