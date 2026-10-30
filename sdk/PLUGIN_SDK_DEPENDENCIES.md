# PluginSDK 跨模块依赖与协议缺口

状态：待协议模块负责人评审；本文件是任务交接建议，不是已经冻结的接口。

## 目标与当前边界

PluginSDK 需要通过生成的 Protobuf/gRPC 类型连接 Core，提供进程外生命周期、事件订阅、消息、模型、会话、存储、权限、调度和配置能力。插件不能直接取得数据库连接、模型 API Key、OneBot API 或 Core 内部类。

当前 `proto/kiarabot/v1/plugins.proto` 已定义清单的 `isolation` 和 `ordered`、注册、事件及 ACK、Action 请求、心跳和配置下发；`actions.proto` 已定义 `ActionResult`。但 `CoreToPlugin` 没有 Action 结果，其他 Host API 也没有完整的请求与响应。SDK 不应自行复制协议类型或引入未协商的替代线协议。

以下结构是逻辑建议，不是可直接生成代码的最终 `.proto`；字段名、编号、服务拆分和版本须由协议负责人确定。

## 协议模块：建议任务

### 公共请求上下文

建议每个并发请求携带 `request_id`、`trace_id` 和 UTC `deadline`；需要幂等的写操作携带 `idempotency_key`。响应携带对应 `request_id`，并用 `oneof` 表示成功结果或现有 `Error`。明确断线、取消、超时、重试和结果查询语义。遵守 `doc/01-contracts.md` 已规定的 ID 和 deadline 限制。

认证后的连接或服务端上下文决定插件身份；请求中的插件 ID 不能单独作为授权依据。重用 `Message`、`ConversationRef`、`Subject`、`PageRequest`、`PageInfo`、`Health` 和 `Error` 等现有类型。

### Action 结果与查询

```text
PluginActionResult
  result: ActionResult

GetActionResultRequest
  context: RequestContext
  idempotency_key

GetActionResultResponse
  context: ResponseContext
  oneof outcome: ActionResult | Error
```

定义 `action_id` 的关联规则、`ACCEPTED` 与最终状态的投递、按插件身份和幂等键查询结果、结果保留期及重复提交行为。网络超时不等于平台执行失败。覆盖 `PERMISSION_DENIED`、`CAPABILITY_NOT_SUPPORTED`、`TIMEOUT` 和依赖故障。

### ModelService

```text
ChatRequest: context, conversation/context, repeated Message, optional model selector, generation options
ChatResponse: context, oneof outcome { response | Error }
ChatDelta: request_id, sequence, typed delta, terminal result or Error
```

提供非流式和流式调用；明确分片顺序、取消、截止时间、正常终止和流中途故障。由 Core 管理 Provider、模型、配额和凭据，协议不得向插件返回模型 API Key。

### ConversationService

```text
GetConversationRequest: context, ConversationRef, PageRequest
GetConversationResponse: context, oneof outcome { messages + PageInfo | Error }
AppendConversationRequest: context, ConversationRef, Message, optional expected version
ClearConversationRequest: context, ConversationRef, optional expected version
```

为写入与清除定义结果、幂等和并发冲突语义；明确读取顺序、分页和权限。是否使用乐观并发版本由协议负责人确定。

### StorageService

```text
GetStorageRequest: context, namespace, key
SetStorageRequest: context, namespace, key, value, ttl
DeleteStorageRequest: context, namespace, key
```

各操作定义成功或 `Error` 响应；明确值编码、大小上限、TTL 单位和缺省语义、键不存在和过期行为、写入与删除的幂等性。命名空间由已认证插件身份约束，不允许插件通过自报 namespace 访问其他插件的数据。

### PermissionService

```text
CheckPermissionRequest: context, Subject, permission, resource_scope
CheckPermissionResponse: context, oneof outcome { decision | Error }
Decision: allowed, optional stable reason, optional policy version/validity
```

定义 `global`、`account`、`group`、`user`、`conversation` 作用域表示。清单中的权限是申请，不是授予；明确决策是否可缓存，以及后续 Host API 调用的授权要求。

### SchedulerService

在现有 `ScheduleTask` 基础上补齐或定义任务结构：`task_id`、由 Core 绑定的插件所有者、计划、payload、目标会话、取消策略，以及必要的稳定状态。定义创建、取消、分页列表及各自结果；明确重复请求、跨插件访问、插件停用时暂停或取消、恢复后的行为。

### 生命周期、配置和订阅

- **生命周期**：先确定由进程启动器、本地 SDK 和 RPC 分别承担什么。若需要远程控制，定义包含 `command_id`、`deadline` 的 `start`、`stop(reason)`、`health` 命令，以及关联的状态或 `Error` 结果。定义重复命令、异常和超时；强制终止由宿主进程管理负责。
- **配置**：现有 `PluginConfig` 已包含版本和值。若 Core 需要知道配置是否生效，增加包含版本、应用结果和 `Error` 的确认；定义重复或旧版本、失败重试和重连重发。明确 `config_schema` 的来源、取得方式和校验位置，不能假定本机路径跨进程可用。
- **订阅**：确定清单 `capabilities` 是否为注册时静态订阅，以及它与 `EventEnvelope.payload` 的映射。只有支持动态变更时才需要订阅请求、结果和生效版本。明确 `PluginEventAck.accepted` 与 `error` 的合法组合、重复事件和处理异常；ACK 表示接受事件，不表示 Action 成功。
- **协商**：注册时明确支持的功能版本、实际授予权限和能力；重连重新注册，确认 API 版本和权限后恢复。明确依赖版本字符串，以及 runtime 与 `process`、`native`、`wasm` 的合法组合。`ordered` 继续表示同一会话内串行投递。

### 兼容性与交付

- 不复用、删除或改变已发布字段编号、类型及含义；客户端安全处理未知字段和枚举值。
- 新增 `oneof` 成员不保证旧端支持。通过明确的功能协商或新的版本化服务避免静默丢失；未协商成功时客户端明确失败。
- 更新 `proto/README.md`、`doc/01-contracts.md`、`doc/03-plugin-sdk.md`；涉及 ACK、重试、重连和生命周期时更新 `doc/01-runtime-semantics.md`。协议是跨语言类型的唯一来源。
- 验证 C++/Python 生成、当前冻结 descriptor 兼容性、未知字段行为和跨语言 round-trip；生成文件只放构建输出，不提交。

## 其他模块需要配合的工作

以下工作由各模块负责人在契约冻结后分别实施，不授权 PluginSDK 任务修改其内部实现。

### Core 与插件宿主

- 实现协商后的 PluginBridge、Host API、Action 结果和查询；从认证上下文绑定插件身份，执行权限、限流、审计和适配器能力检查。
- 按冻结语义管理注册、配置、生命周期、健康、事件 ACK、重试、重连和 `ordered` 投递；隔离插件异常并处理启动/停止超时。
- 确保 Action 幂等和结果可查询；实现配置版本、任务归属及停用策略。Core 通过端口访问基础设施，不把内部类暴露给插件。

### 基础设施

- 按 Core 端口提供会话、插件命名空间存储、任务、模型访问及必要的持久化能力；实现 TTL、幂等和重连恢复所需状态。
- 模型凭据留在受控的提供商/配置边界内；不得直接暴露给插件。业务授权和调度策略由 Core 决定。

### 适配器

- 按现有协议报告可用能力并执行 Core 转交的 Action；返回可关联的结果和稳定错误。插件不直接调用适配器或 OneBot API。

### 控制 API、面板与运维

- 仅当最终契约改变这些模块的可观察接口、配置或状态时，由对应负责人评估并同步 OpenAPI/AsyncAPI、模块文档和测试；不要为 SDK 单独复制业务规则。
- 如启动、健康、强制终止、日志或部署配置发生变化，由运维负责人同步 `doc/08-operations.md` 和相关验证。

### PluginSDK 后续工作

协议及宿主接口冻结后，使用生成类型实现 Python RPC SDK、清单校验、生命周期、Host API、fake host 和插件测试；支持声明 `process`、`native`、`wasm` 与 `ordered`，但不因此声称所有隔离运行时均已实现。覆盖权限拒绝、模型超时、重复事件、配置变更、插件停用和适配器能力不足。

## 协议验收测试

1. C++ 与 Python 代码生成、冻结 descriptor 兼容性、跨语言 round-trip、未知字段和未知枚举处理。
2. Action 提交、接受与终态、重复幂等键、超时后查询、断线恢复、权限拒绝和能力不足。
3. 各 Host API 的成功、参数错误、拒绝、超时和依赖故障；存储隔离、TTL、分页及任务归属。
4. 模型流分片顺序、正常终止、取消、deadline 和中途故障；并发请求的乱序响应与关联。
5. 重复事件、ACK、`ordered` 会话投递、插件异常、生命周期超时、重连版本/权限不匹配。
6. 配置重复/旧版本、应用拒绝与重连重发，以及停用插件后的任务策略。
7. 跨插件资源访问拒绝、未协商功能明确失败，以及断线后操作结果和重试语义。

最低仓库检查为 `cmake --preset proto`、`cmake --build --preset proto`、`ctest --preset proto`；另运行 descriptor 兼容性、Python 生成与跨语言 round-trip。交付时记录实际命令、结果及未运行检查的原因。
