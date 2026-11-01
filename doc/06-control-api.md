# 控制面板 API 设计

## T08a 冻结决策（2026-10-30）

控制 API 使用同进程 C++ 应用端口 IControlApplication（core/include/kiarabot/core/application/control_application.hpp），由组合根注入到 control/api；不新增跨进程管理 RPC，不改变当前 Python 适配器/SDK 和 TypeScript 面板。端口覆盖当前全部 REST 操作；输入为 bearer 凭据、request_id/trace_id/deadline（最长三十秒）、幂等键及类型化命令，禁止自报 actor/role。Core 每次重新核验会话、执行授权、事务和审计；端口返回显式 PortResult，资源文档在输出前脱敏，HTTP 层按 OpenAPI 校验映射。T08b 已实现并完成本机 REST 服务验收。

已逐项核对历史 doc/13 交接：SessionReady 和 Action 查询已由前序任务定义，保留现行契约；其余 P0 决策写入 OpenAPI 的 x-control-policy、操作 x-permission 和 AsyncAPI 的 x-control-session-policy，作为唯一来源。新增 GET /auth/me（服务端角色/权限）、GET /auth/sessions 和 POST /auth/sessions/{sessionId}/revoke；管理员可管理全部会话，其他角色仅自己，不可见与不存在均 NOT_FOUND。TokenPair 保留既有字段，通过 /auth/me 查询当前 session_id。

认证时限为 access 十五分钟、refresh 三十天；refresh 轮换不改变 session_id。撤销后所有 REST 立即拒绝，WebSocket 最迟一秒关闭。幂等成功响应保留二十四小时；主体、operationId、资源 ID 和 key 为作用域；同键不同载荷 CONFLICT。登录以规范化用户名、刷新以原会话隔离，密码/token 仅作为加密摘要进入 fingerprint。已轮换旧 refresh 用不同键返回 UNAUTHENTICATED，同键可重放当前代次响应；会话撤销、过期或返回 token 代次已失效时，登录/刷新重放也返回 UNAUTHENTICATED。缓存 token 响应须加密，不写明文审计。失败不缓存；审计故障阻止新副作用。

列表为十分钟不可变快照，游标绑定用户、资源、当前权限、页大小和排序。v1 不支持未声明过滤参数；过期/篡改/跨范围 token 为 INVALID_ARGUMENT。默认二十、最大一百；末页省略 next_page_token。资源按 ID 升序，audit/logs 按 occurred_at、ID，sessions 按 created_at、session_id；这替代旧版统一 created_at 排序建议。先建立 WebSocket，再读 REST，并缓存此期间事件、刷新受影响资源，避免快照装载期间漏更新；无连接连续性时重新建立会话并全量刷新。

admin/operator/viewer 的默认权限和每个入口要求由 OpenAPI 给出，未知角色/权限默认拒绝；会话的所有者限制在角色检查之后执行。日志仅 operator/admin，用户与审计仅 admin；消息元数据频道仍须显式 `message.read`，不因管理员角色自动开放，唯一授予入口是私有控制配置中的每用户显式权限（见 [运维文档](08-operations.md)，W5）。所有写入口及拒绝都审计，幂等重放不再次产生业务副作用。202 仅表示命令已持久化接受，不能提前报告插件已启动；配置更新先比较 expected_version，冲突不修改数据。

`/audit` 条目对 Action 审计额外带可选 `account_id`/`conversation_id`（来自 Action 目标账号与会话）；其他审计省略这两个字段，OpenAPI 中二者均为可选属性。

HTTP 映射冻结为 INVALID_ARGUMENT=400、UNAUTHENTICATED=401、PERMISSION_DENIED=403、NOT_FOUND=404、CONFLICT=409、RATE_LIMITED=429（Retry-After）、TIMEOUT=504、UPSTREAM_UNAVAILABLE/PLUGIN_UNAVAILABLE/CAPABILITY_NOT_SUPPORTED=503、INTERNAL/未知码=500；不改变桥接 gRPC 既有映射。所有操作补齐实际可产生的依赖、超时、输入和认证响应，写操作补齐幂等冲突。

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

下图是目标职责划分；当前实现集中在 `control/api/src/` 的 `http_server.cpp`、`http_mapper.cpp`、`event_stream.cpp` 和 `configuration.cpp`。

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

分页规范：使用不透明 `page_token`；默认 20 条、最大 100 条；token 绑定当前用户、资源、权限、页大小和十分钟不可变快照；排序及无过滤参数规则以 T08a 和 OpenAPI x-control-policy 为准。

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

T09a 已实现同一监听提供以下频道；各频道状态如下：

```text
/ws/health      # health.updated 仍标 x-contract-status: not_implemented（频道可建但无可推送事件）
/ws/logs
/ws/adapters
/ws/plugins
/ws/tasks
/ws/messages    # 已实现（W5）：仅推送 message.updated 元数据；需每用户显式 message.read
```

WebSocket 只推送经过权限过滤的数据。普通面板用户不能订阅全部私聊消息或完整模型上下文。

WebSocket 服务端使用 WSS；首帧必须是携带 access token 的 `hello` 或同时携带 access token 与已有会话 ID 的 `resume`，不能要求浏览器在握手时设置自定义 Authorization 头。成功后服务端发送 `SESSION_READY`，包含 `session_id` 和确认的 `last_sequence`；会话 ID 是绑定认证主体和频道的不可预测秘密，必须保护、过期和撤销，不能记录或放入 URL。断线后客户端携带会话 ID 和最后处理的序号重连。服务端保留 10 分钟事件，无法补齐时返回 `RESYNC_REQUIRED`：客户端丢弃旧会话、重新以 `hello` 建立会话，并重新拉取 REST 快照。客户端忽略未知事件及字段，已知事件触发对应快照刷新；日志事件 `log.updated` 的 payload 使用 OpenAPI `LogEntry`。`message.updated` 是自包含元数据通知，没有可刷新的 REST 消息投影：客户端只触发既有刷新，不得据此重建历史，也不应期待消息正文；订阅需显式 `message.read`，未授予仍以 4403 关闭。每个消息组件都有唯一 `messageId`。

所有事件使用 `schema_version`、`event_type`、`sequence`、UTC `occurred_at` 和 `payload` envelope。`/ws/control` 汇聚事件；独立频道与它使用相同事件类型和 payload：

- `health.updated`：`state`、`components`，使用 `HealthState`；
- `log.updated`：使用 OpenAPI `LogEntry`，必须脱敏；
- `adapter.updated`：`id`、`state`、`implementation`，可选 `version`、`capabilities`；
- `plugin.updated`：`id`、`version`，至少一个 `state`（`PluginState`）或 `health_state`（`HealthState`），可选 `reason`；
- `task.updated`：`id`、`name`、字符串 `state`；
- `message.updated`：`message_id`、`conversation_id`、`direction`（`received`/`sent`，v1 只产生 `received`），可选 `action_id`（字段保留，v1 不产生）；payload 不含任何消息正文。该通知自包含，不提供 REST 消息投影。

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

## 历史：协议交接时的下游实施要求

以下为协议交接时的历史清单，不代表当前状态；后续接线见本页 T08b、T09a 和[开发台账](16-development-tasks.md)。

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

SQLite 从 v2 起保存审计和命令幂等记录，当前 schema v6 还包含 Host 命名空间、任务、Action 审计目标（账号/会话）与每日预算计数器；控制 API 和桥接业务已接入相应应用入口。

## T08b 实施与验收范围

Core ControlService 实现同进程端口，control/api 使用 Boost.Beast 提供回环 HTTP；生产由同机反向代理终止 HTTPS/WSS，服务拒绝非回环监听。会话及幂等响应使用 libsodium Argon2id/secretbox，控制密钥由私有配置提供，错误仅输出稳定码。状态存于 RuntimeStore 保留命名空间 `$control` 的加密文档，与成功审计同一事务提交；插件身份不允许 `$`，不能访问这些记录。控制存储未新增 SQLite schema；后续配置确认帧见本页配置下发闭环。

首版账号/角色及已安装插件由启动配置指定；没有用户创建或插件安装 API。插件 enable/disable 持久化投递开关，停用后拒绝注册和新投递，已发生的平台副作用不会撤回；enable 允许外部 SDK 重连，202 不代表进程启动完成。配置 API 管理非秘密 JSON 配置与不透明版本；敏感字段拒绝，更新成功仅表示控制配置已持久化；运行实例应用情况由 GET 的 delivery 确认，见配置下发闭环。未配置的资源返回真实空集合，不填充演示数据。

验收包含同一传输映射调用 fake/真实应用端口、HTTP 契约、登录/轮换/撤销、服务端角色、不可变分页、配置冲突、幂等重放、拒绝审计、超时/依赖故障回滚和 SQLite 重启恢复。T07b 真实 QQ/模型不参与本验收。

## T09a 事件流实现

同一监听提供 `/ws/control` 及 health/logs/adapters/plugins/tasks/messages 频道。会话绑定 REST 会话、用户、当前权限和频道；令牌只出现在首帧与服务内存。`messages` 频道已实现（W5，见本文末节）：它是仅元数据的通知流，不提供 REST 消息投影；消息正文不进入 Core 流投影，`message.updated` 的 payload 只有 `message_id`、`conversation_id`、`direction`，v1 只产生 `direction=received`。订阅要求每用户显式授予 `message.read`，任何默认角色（含 admin）都不持有，未授予仍以 4403 关闭。服务器每 100ms 比较经过 Core 授权的资源投影，推送变更后的当前状态；这是允许合并中间状态的刷新通知，不替代逐操作审计。每条已发布通知保留十分钟、每流最多 4096 条并按有界批次重放；进程重启丢弃内存流会话和内存投影，旧客户端收到 SERVER_RESTART 且不补历史。客户端仍遵守先 SESSION_READY、再 REST 快照的顺序。

重连按每流序号重放，SESSION_READY 确认客户端最后已处理位置，之后才重放后续事件。保留过期或容量上限导致无法补齐时明确 RESYNC_REQUIRED，禁止静默跳过；进程重启丢弃内存流会话，旧客户端收到 SERVER_RESTART 并重建快照。每次轮询重新核验认证，撤销/过期最迟一秒关闭；慢连接有界排队并关闭，不能无限缓存。实时流不声称保存每一次中间状态。


## T11a 进程健康契约

OpenAPI 单独定义根路径 `/health/live` 和 `/health/ready`（每路径 servers=/，不带 /api/v1）。两者无认证且只返回 `{state: HEALTHY}` 或稳定 Error，不披露配置、版本、用户或凭据；监听仍仅限回环。live 不调用存储，ready 在迁移、gRPC/控制监听初始化后执行无业务修改的写事务探针，读写失败 503。`/api/v1/health` 保留认证/RBAC 和 Health 组件详情，同时验证存储可写；探针失败不表示进程死亡。历史 `/health/details` 不实现，使用认证的 `/api/v1/health`。

## 配置下发闭环

配置通过 plugin.config.v1 / PluginConfig / PluginConfigAck 下发并按连接确认。REST GET 的 delivery 区分持久化与运行实例应用，PUT/幂等重放只确认持久化。状态、超时、重连、错误及事件门控的完整规则见 [PluginSDK 配置下发闭环](03-plugin-sdk.md#配置下发闭环t08b-后续)。协议唯一来源为 proto/kiarabot/v1/plugins.proto，REST 为 api/openapi.yaml；无新增错误码或 HTTP/gRPC 映射。

GET 配置的 delivery 是查询时观察值，未新增 WebSocket 事件类型；现有 plugin.updated 表示期望配置/插件投影变化，不承诺确认状态推送，客户端轮询 GET 观察应用结果。

## W3 生产可用性与事件流开销（2026-10-31）

本层新增的传输行为不改变 `api/openapi.yaml` 与 `api/asyncapi.yaml` 的操作、schema 或稳定错误码；以下语义属于契约尚未单独描述、需要 contracts 后续登记的部分（见交接项）：

- HTTP/1.1 keep-alive 与有界连接复用：空闲超时、每连接请求上限、关闭时排空在途响应；`HEAD` 返回与 GET 一致的头部且无正文；已知路径的方法不匹配返回 `405` 与 `Allow`；`OPTIONS` 返回允许方法（`../api/openapi.yaml` 尚未声明 405，客户端不得把 405 当作业务错误码）。
- CORS 默认拒绝，仅对 `KIARABOT_CONTROL_CONFIG` 的 `cors_origins` 精确匹配来源授予 `Access-Control-Allow-Origin`；无通配符、无凭据共享；预检返回允许方法与头部。仍要求生产使用同源代理，CORS 只是浏览器集成的显式白名单，不改变身份信任模型。
- 关联标识：接受格式合法的 W3C `traceparent` 与 `X-Request-ID`/`X-Trace-ID`，并在响应中回传 `X-Request-ID`/`X-Trace-ID`；这些值只用于关联，绝不作为身份或授权依据。每个请求向 stderr 输出一行 JSON 访问日志（`component=control-api`、`operation=http_request`，含方法/路径/状态/耗时/request_id/trace_id，不含凭据与 query 中的游标）。
- 传输层限流：认证请求与公开（探针/未认证）请求使用独立的令牌桶；超限返回 `429` 与 `Retry-After`，响应体为稳定 `Error`。该限流独立于 Core 的登录限流与会话/收据/游标容量上限，后者仍由 Core 返回 `RATE_LIMITED`。
- 每访问令牌的并发流配额（默认 16，含 `/ws/control` 与各频道）：超限以 WebSocket `1013` 关闭；`1013` 目前不在 AsyncAPI `x-control-session-policy.close_codes` 中，属于交接 contracts 的待登记关闭码。其它关闭码（4400/4401/4403/1011）不变。
- 优雅关闭：收到 SIGTERM/SIGINT 后停止 accept，等待在途 HTTP 响应与 WebSocket close 握手（有界 drain 超时），向每个已建立流发送 close `1001`，再停止 io 线程；未完成握手的连接与空闲 keep-alive 连接直接关闭。此前实现为立即中止。
- 事件流开销：只轮询已连接（未 detach）的流会话；同一访问令牌的多个流在同一 tick 复用一份 Core 投影；断线会话不继续轮询，恢复时以一次合并更新补齐（符合 AsyncAPI 允许合并中间状态、按序号重放）。`SESSION_READY`/序号/重放/resync 语义不变。

验收与证据见 `doc/16-development-tasks.md` 的 W3 记录；实际运行命令为 `cmake --preset full`、`cmake --build --preset full`、`control/api/tests/.venv/bin/python -m pytest -q control/api/tests`（27 passed）。

### W3 跨模块交接项（本分支不修改这些模块）

1. contracts：登记传输层 `429`（通用限流）、`405`、`OPTIONS`、`HEAD`、CORS 响应头、`X-Request-ID`/`X-Trace-ID` 回传及 WebSocket `1013` 关闭码；确认是否放入 OpenAPI/AsyncAPI 或仅模块文档。若契约要求不同形状，以契约为准回改本层映射。
2. core：`health.updated` 事件（当前流内 health 投影为常量，仍未实现）、Action 查询的 `control/<user>` 生产者与 7 天保留期、`/groups` 数据源、`/logs` 与审计解耦；`messages` 频道授权与投影已由 W5 闭环（见末节）。
3. contracts/core：`message.read` 权限的授予入口——已由 W5 闭环（2026-10-31）：私有控制配置每用户显式授予，有效权限包含授予项；未授予仍返回 4403；messages 事件与频道已从 `not_implemented` 移除。
4. ops/security：TLS 终止、生产反代与证书管理仍由 T11b 承担；本层只保证回环监听与有界资源。

## W5 messages 频道实现（2026-10-31）

`/ws/messages` 从 `x-contract-status: not_implemented` 改为已实现；频道与其它流共用 envelope（`schema_version`、`event_type`、`sequence`、`occurred_at`、`payload`）、首帧认证、序号重放和关闭码语义。契约唯一来源仍为 [`api/asyncapi.yaml`](../api/asyncapi.yaml)。

- 授权：`message.read` 不由任何默认角色（admin/operator/viewer）持有；唯一授予入口是私有控制配置中每用户显式 `"permissions": ["message.read"]`，字段与示例见 [运维文档](08-operations.md) 的 W5 节。未知或重复的权限值在 Core 构造时启动失败。有效权限 = 角色权限 + 显式授予：`GET /auth/me` 的 `permissions` 与流投影的权限都包含授予项。未授予订阅仍以 4403 关闭。
- 事件：只产生 `message.updated`，payload 仅 `{message_id, conversation_id, direction}`，不含任何消息正文；v1 只产生 `direction="received"`，`event.message.sent` 仍为 not_implemented，`action_id` 字段保留但 v1 不产生。
- 无 REST 消息投影：频道是自包含元数据的通知流（十分钟/4096 条有界重放、每批 32 条下发、容量或时间缺口返回 RESYNC_REQUIRED、重启 SERVER_RESTART 后不补历史），不是审计也不能当作审计；客户端不得据此重建历史。面板收到事件只触发既有快照刷新，不新增消息页面、不展示消息历史。
- 频道不再带 `x-contract-status: not_implemented`；`health.updated` 与 `plugin.updated.health_state` 仍未实现，保持原样。

## P01 目标公共结果（未实现）

目标管理响应携带所有者、资源版本、观测时间及 OperationRef。期望配置已保存、Host 进程启动、插件注册、配置应用分别观测；HTTP 202 只表示持久受理。Operation 的 Blocked 和 stalled 不等于失败；execution_outcome=UNKNOWN 不自动重发。Control 不另建权威终态。目标错误映射见 proto/tests/target_spec.json，后续 API schema 同步声明；既有端点未因类型声明自动接线。
