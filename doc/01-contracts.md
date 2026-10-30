# 统一协议层设计

## 责任

统一协议层负责定义所有语言和进程都能理解的事件、动作、错误、能力和版本信息。

它是 Python 适配器、C++ Core、C++ 插件和 TypeScript 面板之间唯一的共享语言。

## 目录

```text
proto/kiarabot/v1/
  common.proto
  events.proto
  actions.proto
  adapters.proto
  plugins.proto
  services.proto

api/
  openapi.yaml
  asyncapi.yaml
```

## 事件协议

事件类型只由 `EventEnvelope.payload` 的 `oneof` 判定，不再维护单独的 type 字符串。Action 同理；消费者使用 oneof discriminator 选择处理器，无法匹配 payload 时拒绝消息并返回 `INVALID_ARGUMENT`。REST 错误类型以 `api/openapi.yaml` 的 Error schema 为准，实时频道以 `api/asyncapi.yaml` 为准。

所有事件都使用统一外层：

```text
EventEnvelope
  event_id          全局唯一
  schema_version    协议版本
  occurred_at       发生时间
  trace_id          链路追踪 ID
  source            适配器和账号
  subject           用户和会话
  payload           事件内容
```

第一批事件：

```text
MessageReceived
MessageSent
GroupMemberChanged
FriendRequest
TimerTriggered
AdapterStatusChanged
PluginStatusChanged
```

消息内容必须使用平台无关的消息段：

```text
text
image
audio
video
file
mention
reply
location
unknown
```

平台不支持的类型由适配器报告能力错误，不能在 Core 中写平台判断。

## Action 协议

插件和 Core 不直接调用平台 API，而是产生 Action：

```text
SendMessage
DeleteMessage
ReactMessage
MuteMember
KickMember
ApproveRequest
ScheduleTask
```

每个 Action 都有：

```text
action_id
idempotency_key
deadline
requested_by
target
payload（由 oneof discriminator 唯一决定）
```

适配器执行后返回：

```text
ActionResult
  action_id
  status
  platform_message_id
  error（Error message，包含 code、message、details、request_id、trace_id 和 retryable）
```

## 适配器注册协议

适配器启动后必须先注册：

```text
RegisterAdapterRequest
  adapter_id
  implementation
  implementation_version
  protocol_version
  accounts[]
  capabilities[]
```

Core 返回：

```text
RegisterAdapterResponse
  accepted
  assigned_instance_id
  required_protocol_version
  heartbeat_interval
  error（失败时返回）
```

## 插件协议

插件注册时发送 PluginManifest。Core 校验：

- 插件 ID 是否重复；
- API 版本是否兼容；
- 依赖是否满足；
- 权限是否允许；
- 配置 Schema 是否有效；
- 运行时是否被允许启动。

## 错误模型

错误必须包含稳定错误码，不让前端或插件解析自由文本：

```text
INVALID_ARGUMENT
UNAUTHENTICATED
PERMISSION_DENIED
NOT_FOUND
CONFLICT
CAPABILITY_NOT_SUPPORTED
RATE_LIMITED
TIMEOUT
UPSTREAM_UNAVAILABLE
PLUGIN_UNAVAILABLE
INTERNAL
```

错误码在 Protobuf、REST 和 WebSocket 中保持同名语义；gRPC 状态映射为：`INVALID_ARGUMENT → INVALID_ARGUMENT`、`UNAUTHENTICATED → UNAUTHENTICATED`、`PERMISSION_DENIED → PERMISSION_DENIED`、`NOT_FOUND → NOT_FOUND`、`CONFLICT → ALREADY_EXISTS/ABORTED`、`RATE_LIMITED → RESOURCE_EXHAUSTED`、`TIMEOUT → DEADLINE_EXCEEDED`，其他错误映射为 `INTERNAL`。

跨协议的稳定枚举词汇必须逐项一致：

- `ErrorCode`：`ERROR_CODE_UNSPECIFIED`、`INVALID_ARGUMENT`、`UNAUTHENTICATED`、`PERMISSION_DENIED`、`NOT_FOUND`、`CONFLICT`、`CAPABILITY_NOT_SUPPORTED`、`RATE_LIMITED`、`TIMEOUT`、`UPSTREAM_UNAVAILABLE`、`PLUGIN_UNAVAILABLE`、`INTERNAL`；
- `HealthState`：`HEALTH_STATE_UNSPECIFIED`、`HEALTHY`、`DEGRADED`、`UNHEALTHY`；
- `PluginState`：`PLUGIN_STATE_UNSPECIFIED`、`PLUGIN_STATE_DISABLED`、`PLUGIN_STATE_STARTING`、`PLUGIN_STATE_READY`、`PLUGIN_STATE_RUNNING`、`PLUGIN_STATE_STOPPING`、`PLUGIN_STATE_STOPPED`、`PLUGIN_STATE_UNAVAILABLE`、`PLUGIN_STATE_FAILED`；
- `ActionStatus`：`ACTION_STATUS_UNSPECIFIED`、`ACCEPTED`、`SUCCEEDED`、`FAILED`、`EXPIRED`、`CANCELLED`。

这些枚举在 Protobuf、`api/openapi.yaml` 的 REST schema 和 `api/asyncapi.yaml` 的组件 schema 中使用相同名称；客户端必须保留未知值以兼容后续新增枚举。WebSocket 频道及 payload 定义以 AsyncAPI 为准。

已发布的 `PluginStatusChanged.state`（字段 3）保持为 `HealthState`；新增 `optional int32 lifecycle_state = 5`，其数值以 `plugins.proto` 中的 `PluginState` 为唯一来源，不重新定义枚举。缺失表示事件没有提供生命周期状态，显式 0 表示 `PLUGIN_STATE_UNSPECIFIED`；未知数值必须保留。健康状态为 `HEALTH_STATE_UNSPECIFIED` 表示未知，两个状态不能互相推断。旧消费者忽略新增字段，旧生产者不提供生命周期状态。

`events.proto` 不能导入依赖它的 `plugins.proto`；整数载体避免循环依赖，消费方需要显式校验或保留 `PluginState` 的数值，不能依赖生成代码的枚举类型检查。

## 代码生成和字段校验

协议实现必须使用仓库根目录的命令：

```bash
cmake -S . -B build -DKIARABOT_GENERATE_PROTO=ON
cmake --build build --target kiarabot_proto
```

生成输出只能放在 `build/generated/`。CI 还必须生成 descriptor 并比较基线，检测字段编号复用和不兼容删除：

```bash
protoc --descriptor_set_out=build/kiarabot-v1.pb \
  --include_imports -I proto proto/kiarabot/v1/*.proto
```

字段规则：所有 ID 非空且最长 256 字节；时间戳使用 UTC；`page_size` 默认 20、最大 100；`deadline` 最长 5 分钟；消息段总大小默认 1 MiB。

## 兼容性

- Protobuf 使用 `kiarabot.v1`、`kiarabot.v2` 版本空间；
- 新增字段不影响旧客户端；
- 删除字段前先标记 deprecated；
- 已删除的字段编号永不复用；
- 枚举新增值时客户端必须允许未知值；
- 协议变更必须有兼容性测试。

## 不属于本层的内容

- 不实现 WebSocket 连接；
- 不解析 OneBot 原始 JSON；
- 不实现数据库；
- 不实现插件逻辑；
- 不实现控制面板页面；
- 不保存运行时状态。

## 验收标准

- Python 和 C++ 都能生成客户端；
- 一条事件可以从适配器发送到 Core，再转换为 Action；
- 重复 `event_id` 不会造成重复处理；
- 未知字段不会导致旧客户端崩溃；
- 错误码和协议版本有自动化测试。

## 代码启动句

> 请只创建 `proto/kiarabot/v1` 的 Protobuf 协议和生成测试，先实现事件、Action、适配器注册、插件注册及稳定错误码，不实现任何平台或业务逻辑。
