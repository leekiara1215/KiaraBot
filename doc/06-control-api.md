# 控制面板 API 设计

T04a 插件桥接新增 Action 结果和查询承载，与 REST 查询共享调用方隔离和“查询失败不代表执行失败”的语义。插件通过 action.result.v1 协商，不复用浏览器认证主体。REST 路径、schema 和 HTTP 映射此次不变；服务实现仍由 T08 承担。

## 责任

控制面板 API 是浏览器和未来 C++ 控制客户端的统一入口，负责认证、授权、查询和管理操作。

它不直接连接 NapCat、数据库或插件进程，而是调用 Core 的应用服务。

## 协议

```text
HTTP REST + JSON：配置和管理
WebSocket：实时状态、日志和事件
OpenAPI：api/openapi.yaml
AsyncAPI：api/asyncapi.yaml
```

## 目录

```text
control/api/
  src/
    auth/
    rbac/
    rest/
    websocket/
    health/
    mappers/
  tests/
```

`api/openapi.yaml` 和 `api/asyncapi.yaml` 是契约；`control/api/` 只实现服务和协议映射，不复制契约定义。

## API 分区

```text
/api/v1/auth
/api/v1/bots
/api/v1/adapters
/api/v1/plugins
/api/v1/models
/api/v1/groups
/api/v1/users
/api/v1/tasks
/api/v1/actions
/api/v1/logs
/api/v1/audit
/api/v1/health
```

## 认证和权限

控制面板 API 必须实现：

- 登录和会话；
- Token 轮换；
- 管理员、运维和只读角色；
- API 操作审计；
- 插件权限和平台权限分离；
- 默认拒绝的 RBAC。

前端隐藏按钮不能代替后端授权检查。每一个写操作都必须由 API 服务再次检查权限。

认证规范：

- 密码使用 Argon2id 哈希；
- access token 默认 15 分钟，refresh token 默认 30 天；
- refresh token 采用旋转策略，旧 token 使用一次后失效；
- 管理员可以撤销会话并轮换 refresh token；
- 生产环境只允许 HTTPS/WSS；
- API 不返回或记录模型 API Key、OneBot Token 和 Core 内部凭据；
- 写请求使用 `Idempotency-Key`，重复请求返回原响应。

分页规范：使用不透明 `page_token`；默认 20 条、最大 100 条；token 绑定用户、过滤条件和资源版本；列表使用稳定的 `created_at, id` 排序。

`GET /groups` 返回群组标识、角色和有效权限标识；`GET /users` 返回用户所属群组、有效角色和有效权限标识。权限由服务端计算并执行，客户端只展示结果。`GET /logs` 返回脱敏后的日志时间、级别、组件、消息及可选 trace ID；日志不得包含凭据。

## 插件管理接口

至少提供：

```text
GET  /plugins
POST /plugins/validate
POST /plugins/{id}/enable
POST /plugins/{id}/disable
GET  /plugins/{id}/config
PUT  /plugins/{id}/config
GET  /plugins/{id}/health
```

插件安装来源、版本、许可证和校验值必须记录在 Core 的插件状态中。

## WebSocket 频道

建议按频道隔离：

```text
/ws/health
/ws/logs
/ws/adapters
/ws/plugins
/ws/tasks
/ws/messages
```

WebSocket 只推送经过权限过滤的数据。普通面板用户不能订阅全部私聊消息或完整模型上下文。

WebSocket 服务端使用 WSS；首帧必须是携带 access token 的 `hello` 或同时携带 access token 与已有会话 ID 的 `resume`，不能要求浏览器在握手时设置自定义 Authorization 头。成功后服务端发送 `SESSION_READY`，包含 `session_id` 和确认的 `last_sequence`；会话 ID 是绑定认证主体和频道的不可预测秘密，必须保护、过期和撤销，不能记录或放入 URL。断线后客户端携带会话 ID 和最后处理的序号重连。服务端保留 10 分钟事件，无法补齐时返回 `RESYNC_REQUIRED`：客户端丢弃旧会话、重新以 `hello` 建立会话，并重新拉取 REST 快照。客户端忽略未知事件及字段，已知事件触发对应快照刷新；日志事件 `log.updated` 的 payload 使用 OpenAPI `LogEntry`。每个消息组件都有唯一 `messageId`。

所有事件使用 `schema_version`、`event_type`、`sequence`、UTC `occurred_at` 和 `payload` envelope。`/ws/control` 汇聚事件；独立频道与它使用相同事件类型和 payload：

- `health.updated`：`state`、`components`，使用 `HealthState`；
- `log.updated`：使用 OpenAPI `LogEntry`，必须脱敏；
- `adapter.updated`：`id`、`state`、`implementation`，可选 `version`、`capabilities`；
- `plugin.updated`：`id`、`version`，至少一个 `state`（`PluginState`）或 `health_state`（`HealthState`），可选 `reason`；
- `task.updated`：`id`、`name`、字符串 `state`；
- `message.updated`：`message_id`、`conversation_id`、`direction`（`received`/`sent`），可选 `action_id`；默认不推送正文。

每个频道只推送已授权的数据。具体 schema 和字段约束以 AsyncAPI 为准。

频道、首帧、消息 envelope 和重连语义以 [`api/asyncapi.yaml`](../api/asyncapi.yaml) 为唯一契约。OpenAPI 只描述 REST，不再把 WebSocket 仅作为文字建议。插件健康查询使用 `GET /plugins/{pluginId}/health`，返回统一 `Health` schema。

当前 AsyncAPI 固定为 2.6.0，以保持现有频道/操作模型和生成工具兼容；校验器对 3.x 的提示属于升级建议，不改变当前契约的有效性。

## 错误格式

所有 REST 错误使用统一结构：

```json
{
  "code": "PERMISSION_DENIED",
  "message": "permission denied",
  "request_id": "...",
  "trace_id": "...",
  "retryable": false,
  "details": {}
}
```

REST 的 `Error.code`、健康状态、插件状态和 Action 状态分别引用 `api/openapi.yaml` 的 `ErrorCode`、`HealthState`、`PluginState` 和 `ActionStatus` schema。它们与 Protobuf 和 AsyncAPI 中的同名枚举逐项一致；`ERROR_CODE_UNSPECIFIED` 等未指定值只用于兼容解码，服务端返回业务错误时使用稳定的具体错误码。

`GET /actions/{actionId}` 按 ID 查询；`GET /actions/by-idempotency-key` 使用 `Idempotency-Key` 请求头，在已认证调用方的授权范围内查询，密钥不得写入日志。`by-idempotency-key` 为保留路径段。两者返回 `ActionResult`：`action_id` 和 `status` 必填；`ACCEPTED` 未终结，`SUCCEEDED`、`FAILED`、`EXPIRED`、`CANCELLED` 已终结，终结结果必须有 `completed_at`，`FAILED` 必须有 `error`。终结结果保留 7 天。

查询错误使用统一 Error：400 `INVALID_ARGUMENT`、401 `UNAUTHENTICATED`、403 `PERMISSION_DENIED`、404 `NOT_FOUND`（包括不存在、已过期和不可见）、429 `RATE_LIMITED`、500 `INTERNAL`、503 `UPSTREAM_UNAVAILABLE`、504 `TIMEOUT`。查询超时不代表 Action 执行失败。

## 下游模块待实现

- Core：在插件状态事件中独立填充健康状态和生命周期字段；按现有 `PluginState` 数值编码，保留未知值。提供按 Action ID 和按已认证调用方及幂等键查询结果的应用服务；执行中的不确定结果不能被误报为失败。终结结果保留 7 天。
- 基础设施：实现 Core 端口要求的 Action 结果和调用方范围内幂等键索引、7 天保留及清理；支持控制 API 会话重放所需的 10 分钟事件保留，不自行决定业务状态。
- 控制 API：实现 OpenAPI 所列两个 Action 查询，进行认证、授权、幂等键作用域隔离及稳定错误映射；落实 AsyncAPI 的会话认证、10 分钟重放、权限过滤和各频道 payload。日志必须脱敏，消息事件默认不含正文。
- 适配器、插件 SDK 和面板客户端：兼容缺失的生命周期字段及未知枚举数值；面板使用 `session_id` 和 `sequence` 恢复，收到 `RESYNC_REQUIRED` 后刷新 REST 快照。客户端不得从健康状态推断生命周期。
- 运维与 CI：配置 HTTPS/WSS 和密钥脱敏，提供已发布 descriptor 基线并在 CI 中运行兼容性、生成、round-trip 和 API 校验；修正仓库格式配置中的无效布尔值后恢复标准 `clang-format` 检查。

这些是下游实施要求，当前协议变更不修改上述模块内部实现。浏览器通过首帧提供 access token；服务端须在恢复连接时重新认证并验证会话绑定。

## 不属于本层的内容

- 不在 API 中编写 AI Prompt；
- 不在 API 中解析 OneBot；
- 不在 API 中直接访问数据库表；
- 不在 API 中实现插件业务；
- 不把管理员 Token 下发给插件。

## 验收标准

- OpenAPI 文档可生成 TypeScript 和 C++ 客户端；
- 未授权请求不能读取或修改资源；
- 所有写操作有审计记录；
- WebSocket 重连后能恢复订阅；
- API 在 Core 不可用时返回稳定错误。
- OpenAPI schema 通过 YAML 解析和 lint；
- 401/403/404/409/429/500/503 错误映射固定且有测试；
- TLS、token 轮换、分页和幂等请求有集成测试。

## 代码启动句

> 请只实现 `api/openapi.yaml` 和控制面板 API v1 的认证、RBAC、机器人、适配器、插件、配置、健康和审计接口，使用 mock Core 服务完成 REST、WebSocket、错误格式和权限测试。

## T10a 受保护的应用命令

AuthorizedCommands 是 Core 内部入口：调用方必须从已认证连接构造 CommandContext，并由服务端计算规范化请求的 SHA-256 fingerprint（64 位小写十六进制），不能相信插件自报身份、权限或 fingerprint。静态 PermissionGrant 按 subject/permission/resource 精确匹配，只有配置中的资源 `*` 表示全部资源；没有匹配规则时默认拒绝。此入口不是跨进程 SDK。

execute 的回调只能修改传入的 RuntimeSnapshot，不得执行网络/平台副作用或另启 Runtime 事务。业务修改、成功幂等记录和审计在同一提交中完成；提交失败全部不生效。幂等范围为主体+权限+资源+键，同键同 fingerprint 重放成功且不重复审计；不同 fingerprint 返回 CONFLICT。重放前重新鉴权。拒绝及回调失败记录审计，失败回调的状态变更被回滚；错误详情只保留稳定码和请求标识，不转发回调的自由文本。审计满（默认十万条）时返回 UPSTREAM_UNAVAILABLE 且不执行回调。审计/幂等保留和清理策略须后续管理任务落实，不能当作无限容量服务。

SQLite schema v2 保存审计和命令幂等记录，并事务化升级 v1 快照；该版本仍不包含 Host 模型/任务/命名空间存储。控制 API 和桥接业务还需在相应任务中使用这一入口；当前网络服务不会因该类存在就获得业务能力。
