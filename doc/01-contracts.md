# 统一协议层设计

T03a 新增 CoreToPlugin.error（oneof 字段 6），承载注册之后的终止错误，不复用旧编号。注册拒绝仍使用 RegisterPluginResponse.error；注册前身份错误仍为 gRPC UNAUTHENTICATED/PERMISSION_DENIED。注册后的非法 ACK、权限/状态拒绝、依赖故障使用终止 Error 帧，随后结束流；断线/取消可能使该帧不可达，此时客户端保持未确认结果未知并重新注册。完整注册、订阅和 ACK 约束见 doc/03-plugin-sdk.md 的 T03a 子集。

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

T04a 定义可选功能 `action.result.v1`：RegisterPluginRequest.supported_features（字段 3）申请，RegisterPluginResponse.enabled_features（字段 5）返回实际支持的交集。缺省为空，旧客户端仍只使用事件能力；新客户端连接旧 Core 时不得发送 Action 请求，应本地返回 CAPABILITY_NOT_SUPPORTED。Core 不得声明尚未接线的功能，未协商的 Action 帧明确拒绝。未知功能名不启用。

PluginToCore.action 保留字段 3，PluginActionRequest.action 保留字段 1，新增 request_id=2；新增 action_query=6、action_cancel=7；CoreToPlugin.action_response=7。响应带同一 request_id，outcome 恰好一个 result 或 error。提交/查询/取消均为一问一答；不承诺异步终态推送，客户端通过查询获得终态。请求 ID 在当前连接的在途操作中唯一，长度 1–128；trace_id 同样非空且最多 128。ID/幂等键沿用 ASCII 字母数字、点、下划线、连字符约束。

提交必须有 action_id、idempotency_key、trace_id、deadline 和已知 payload；身份来自认证插件连接，requested_by 为空时由 Core 填入，非空且不匹配则 PERMISSION_DENIED。幂等索引为调用方身份+Action 幂等键，与连接实例和 request_id 无关；相同键/相同规范化内容返回原 action_id 和状态，不同内容返回 CONFLICT。查询 PluginActionQuery 的 selector 必须恰好为 action_id 或 idempotency_key；查询和取消均要求未来 UTC deadline。不可见/不存在/保留期后记录统一 NOT_FOUND。取消仅对未派发、未到期 ACCEPTED 生效；已派发且结果未知返回 CONFLICT，已有终态返回原结果。

拒绝、查询超时、存储故障使用 response.error：INVALID_ARGUMENT、PERMISSION_DENIED、CAPABILITY_NOT_SUPPORTED、CONFLICT、NOT_FOUND、RATE_LIMITED、TIMEOUT、UPSTREAM_UNAVAILABLE、INTERNAL。它们不自动生成 Action FAILED。ActionResult 沿用已有状态；ACCEPTED 不带 completed_at/error，终态带 completed_at，FAILED 必须带稳定 Error；EXPIRED 使用 TIMEOUT，CANCELLED 不要求 error。断线、缺失响应、未知枚举或 NOT_FOUND 均不得解释为平台执行失败；重新协商后按同一调用方及幂等键查询。协议已定义不代表服务已实现，T04b 负责实际接线。

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

### 插件 Host API v1

T06a 在现有功能协商字段中增加 host.storage.v1 名称，不新增线协议字段；仅协商成功后使用 storage_get/set/delete。Core 原样关联 request_id，HostResponse.error 与 result 必须互斥。具体 TTL、限额和稳定错误见 doc/03-plugin-sdk.md；读取/写入必须获授 storage.namespace。失败状态不以空值代替；空 bytes 是合法存储值。其余 Host operation 在后续功能接线前明确 CAPABILITY_NOT_SUPPORTED。

`PluginBridge` 注册成功后，插件以 `HostRequest` 调用模型、私有存储和一次性任务服务；Core 使用相同 `request_id` 返回 `HostResponse`。请求必须携带非空 `request_id`、`trace_id` 和未来的 UTC `deadline`。`storage_set`、`storage_delete`、`task_create`、`task_cancel` 必须携带幂等键；同一插件和幂等键重放返回原结果，载荷不同返回 `CONFLICT`。响应必须恰好包含对应的结果或 `Error`，不得同时包含两者。插件身份来自已注册连接，不能由请求指定；所有调用均先经过 Core 权限检查。断线后未确认结果须由调用者以相同幂等键查询或重试，不能假定超时代表未执行。

- 模型请求包含模型名和角色/文本消息，角色仅允许 `system`、`user`、`assistant`；响应包含文本及输入、输出 token 数。API Key、上游响应正文和私密消息不得进入日志或错误详情。
- 存储的命名空间由 Core 根据插件身份绑定；键仅在该命名空间内有效。缺失或过期键返回 `NOT_FOUND`；删除不存在的键成功。可选过期时间为 UTC，大小和 schema 校验由 Core 策略决定。
- 任务属于调用插件，读取和取消其他插件的任务返回 `NOT_FOUND`。`run_at` 是 UTC；`local_time` 使用 IANA 时区的当地一次性时间。无效时区、模糊或不存在的当地时间返回 `INVALID_ARGUMENT`；不自动猜测夏令时偏移。任务状态由 `ScheduledTaskState` 定义。`ScheduleTask.schedule` 是原有 Action 字段，不在此处重新解释。
- 模型超时映射 `TIMEOUT`，限流映射 `RATE_LIMITED`，可重试的上游连接或服务故障映射 `UPSTREAM_UNAVAILABLE`，拒绝由 Core 映射 `PERMISSION_DENIED`；其他内部故障映射 `INTERNAL`。`retryable` 必须反映该次失败是否允许重试，不得包含密钥或上游原始内容。

## 错误模型

T08a 控制面 REST 新增会话查询/撤销及当前主体接口，具体响应、RBAC、幂等与分页在 OpenAPI x-control-policy 冻结；WebSocket 的关闭码/权限/撤销/重同步在 AsyncAPI x-control-session-policy 冻结。REST 503 还覆盖 PLUGIN_UNAVAILABLE 和 CAPABILITY_NOT_SUPPORTED；其余映射见 doc/06-control-api.md。这是同进程应用端口，不新增 Protobuf 管理 service，也不改变既有 gRPC 错误映射。

T06c 使用 host.tasks.v1，task.schedule 权限和 event.timer.triggered 订阅；task_get/cancel 无订阅要求但仍需权限。字段及状态数值不变；文档中的 PENDING/RUNNING/RETRYING/COMPLETED/FAILED/CANCELLED 均是 ScheduledTaskState 的 SCHEDULED_TASK_ 同名枚举缩写，不能与 ActionStatus 混用。非法日期/时区/DST/过期任务时间 INVALID_ARGUMENT，非所有者 NOT_FOUND，幂等/终态取消/旧 claim 冲突 CONFLICT，限额 RATE_LIMITED，存储错误按既有映射返回。创建/取消失败不产生事件；详细验收与取消边界见 doc/01-runtime-semantics.md。

T06b 增加现有功能协商名 host.model.v1，不新增字段。model_chat 使用对应 HostResponse.model_chat；模型未配置或未协商 CAPABILITY_NOT_SUPPORTED，未获授 model.chat 为 PERMISSION_DENIED。TIMEOUT 和不确定 UPSTREAM_UNAVAILABLE 不可自动重试（retryable=false），RATE_LIMITED 可稍后重试。模型不属于上述写操作幂等回放范围；详细限制和 HTTP 映射以 doc/05-infrastructure.md 的 T06b 实现范围为准。

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

## 桥接连接认证（T01b）

两个 Connect RPC 的 metadata 必须恰好包含一个 `authorization: Bearer <token>`。凭据由宿主配置绑定角色（adapter/plugin）与资源 ID，不从请求取得授权身份。缺失、重复、错误格式或未知凭据返回 gRPC UNAUTHENTICATED；角色不匹配或注册 ID 冒用返回 PERMISSION_DENIED。错误详情 `Status.error_details()` 为序列化的 `kiarabot.v1.Error`，客户端使用 code 判定，不解析自由文本。

未配置相应 T02a/T03a 业务入口时，已认证连接的首条请求返回 `CAPABILITY_NOT_SUPPORTED` / gRPC INTERNAL；不会发送 accepted、持久化确认或心跳成功。空消息返回 INVALID_ARGUMENT，客户端正常半关闭无消息返回 OK；取消及 deadline 使用 gRPC 传输状态 CANCELLED/DEADLINE_EXCEEDED，不代表业务成功。关闭服务取消等待中的流并等待处理器退出。已接线的注册和权限约束见 T02a/T03a 对应章节。

## T02a 注册与 inbox 确认

启用持久化接收的 AdapterBridge 首帧必须是 RegisterAdapterRequest。协议版本固定 kiarabot.v1；账号至少一个，必须 enabled 且在宿主配置的允许列表中；自报账号及 Capability 不授予权限。注册成功返回 accepted、随机 assigned_instance_id、required_protocol_version 和 30 秒心跳间隔；注册失败返回 accepted=false 和稳定 Error，然后正常结束流。新注册替换同身份旧实例，旧连接不能继续入库。事件和心跳必须绑定当前实例。身份冒用仍以 gRPC PERMISSION_DENIED 终止。

CoreToAdapter.event_ack 使用新编号 4。AdapterEventAck 包含 event_id、trace_id、status、error、retry_after_ms：PERSISTED 表示 inbox/会话水位/审计和幂等记录已原子提交，DUPLICATE 表示同内容已提交，REJECTED 表示本次未接收。只有前两者是确认；不是插件 ACK 或 Action 完成。

内容或顺序冲突使用 CONFLICT，错误 schema/缺失字段使用 INVALID_ARGUMENT，无账号授权或伪造 Core 内部事件使用 PERMISSION_DENIED，队列满 RATE_LIMITED，存储不可用 UPSTREAM_UNAVAILABLE，损坏/内部失败 INTERNAL。RATE_LIMITED、存储暂时故障及版本提交冲突可重试，retry_after_ms=1000；内容冲突不可重试。传输超时/取消依旧使用 gRPC 状态，确认丢失不能推断未入库。未装配 ActionRouter 时 ActionResult 明确拒绝 CAPABILITY_NOT_SUPPORTED / gRPC INTERNAL；T04b 装配后按已认证适配器归属验证并持久化结果。未配置持久化入口时保留 T01b 拒绝语义。
