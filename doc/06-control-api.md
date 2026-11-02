# 控制面板 API 设计

## 接入向导进程内模型

Control 接入驱动模型、配置校验和接口见 [接入方案](26-real-qq-onboarding.md#第一项进程内通用模型与驱动端口)。
此阶段不新增 HTTP/WS 路由，驱动执行、操作持久化、权限和向导尚未接线；账号与绑定仍由 Runtime 拥有。

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
/api/v1/principals
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

`GET /groups` 返回群组标识、角色和有效权限标识；`GET /users` 返回用户所属群组、有效角色和有效权限标识。P12 目标目录改用只读 `GET /api/v1/principals`：Identity `ListPrincipals` 按调用者 `identity.principal.read` 与实际获授资源范围返回人类主体的分页快照，Control 只透出最小展示字段（principal_id、binding、state、version、roles），不混入服务/Host/插件身份，也不返回凭据；普通用户查看自己仍用 `/auth/me`。权限由服务端计算并执行，客户端只展示结果。`GET /logs` 返回脱敏后的日志时间、级别、组件、消息及可选 trace ID；日志不得包含凭据。

P12 目标权限补充：`PERMISSION_LOG_READ = 48`（`log.read`）与 `PERMISSION_AUDIT_READ = 49`（`audit.read`）是分开的敏感读权限，固定角色和创建策略都不派生；`/logs` 只要求 `log.read`，`/audit` 只要求 `audit.read`，二者不能互相替代。Control 只做只读聚合，数据归属各业务服务；缺少权威查询来源时返回 `UNAVAILABLE`（HTTP 503），不得返回成功空页。日志/审计分页按 `occurred_at` + 稳定 ID，响应不含凭据、消息正文或模型密钥。

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

## 健康与组件聚合（P12）

- `/health/live`：仅进程存活，不访问存储、不暴露组件；200 只表示 HTTP 进程仍在运行。
- `/health/ready`：匿名接流量判断。只有 Control 完成启动、自身存储可用且 Identity 会话校验/撤销链当前可达并可撤销时返回 200；Identity/凭据链失效或无法兑现撤销返回 503。匿名响应不暴露组件名、版本、原因或依赖细节。
- `/api/v1/health`：需 `health.read`，返回聚合 `HealthState` 与四个直接业务依赖 `identity`、`runtime`、`scheduler`、`model` 的组件详情（`component`/`state`/可选 `reason` 与观测时间）。聚合规则：
  - Identity 不可用、凭据链不是当前版本、或会话撤销无法证明 → 整体未就绪（503）；
  - Runtime、Scheduler、Model 任一单点故障 → 整体 `DEGRADED`（HTTP 200），对应 owner 功能返回 `UPSTREAM_UNAVAILABLE`，不得伪造空页/旧快照，Control 不整体摘流；
  - Control 自身进程/存储故障或启动未完成 → 未就绪；
  - 单项故障绝不产生 `HEALTHY`。
- `health.updated` WS 事件当前仍标 not_implemented；组件详情以受权 REST 为准，客户端不得从 WS 缺席推断组件状态。

## P12 契约口径（Q1–Q8）

**Q1 Action 查询**：Control 只能调用 Runtime `QueryActionResult`，由 Runtime 在线核验 `action.inspect` 与恰好一个 account scope；按幂等键查询必须同时传 `owner_instance_id`（同一账号不同插件实例可有相同键）。插件继续经 PluginBridge 查询自身业务实例。管理员角色不自动获得 inspect 权限。`NOT_FOUND` 只表示不可见/已过期/不存在，不得据此推断平台动作失败；查询超时也不改变 Action 状态。

**Q2 审计与日志**：Control 提供只读 API，数据归属仍属各业务服务（业务变更事务内提交审计），Control 只经所有者授权接口聚合。`log.read`（48）与 `audit.read`（49）是分开的敏感读权限，固定角色不派生；对应 REST `/logs`、`/audit`，WS `logs`/`log.updated` 只映射 `log.read`。`app.logs` 只是运维日志分类，不是插件可访问存储、公共权限名或 WS 频道。缺少权威来源必须 `UNAVAILABLE`，不得返回成功空页。日志/审计按 `occurred_at`+稳定 ID 分页，服务端脱敏，绝不返回凭据、消息正文或模型密钥。

**Q3 groups 与账号策略**：弃用 `/groups`（无条件空页，不得作为群目录或白名单数据源）。Identity 管“谁有权修改哪个账号”；Runtime 管“该账号允许执行哪些动作”。禁言/踢出开关、允许群 ID、两类每日预算与版本保留在 Runtime `ActionPolicy`，Panel 直接管理这份带版本策略（`/accounts/{id}/action-policy`）。不把危险动作判定拆到 Identity；把白名单扩大为“群内所有消息是否接入”属新能力，v1 不定义。

**Q4 主体目录**：普通用户查看自己继续用 `/auth/me`；管理目录使用 `GET /api/v1/principals`（Identity `ListPrincipals`），只列人类主体、分页快照、最小展示字段，不混入服务/Host/插件身份，不引入自定义角色管理。见“健康与组件聚合”前的 principal_directory 契约。

**Q5 健康**：见“健康与组件聚合（P12）”一节；匿名 ready 只回答接流量条件，组件详情只走受权 `/api/v1/health`；Identity/凭据链/撤销失败整体未就绪，Runtime/Scheduler/Model 单点故障只降级对应功能。

**Q6 安装/实例/配置**：Panel 分开展示三类事实——Runtime 已保存的期望状态/配置版本、Host 观察到的受管进程启动/退出、插件确认配置已应用。Control 转发带幂等键+预期版本的命令，返回 202 仅表示已受理；进度按 operation 查询。禁止把旧 `/plugins/{id}/enable` 机械映射到单个实例：一个安装可多实例，操作对象必须明确（`/installations*`、`/instances/*/desired-state|config`）。Host 负责执行/恢复/上报进程事实，配置是否应用由插件确认。

**Q7 模型权限**：保留 `model.read`/`model.manage`；`get_configuration` 要求精确模型的 `model.manage`。普通只读页用 `model.read` 展示脱敏元数据；管理页凭 `model.manage` 读取完整配置/修改/轮换。密钥写入后只显示是否已配置及版本，任何管理员都不能读回原值。

**Q8 P12 v1 验收**：分批交付；首批“基础写操作”验收项：会话撤销、账号与适配器绑定、插件启停与配置、模型配置与密钥更新、群白名单更新、任务查询与取消、Action 元数据查询。异步操作必须区分已受理/已完成/受阻/结果未知；WS 断流后从权威快照恢复，不得用本地/旧流推断成功。日志/审计查询 UI 可后置，但必须记录为阶段性延期（冻结基线与 P12 计划将日志/审计列入面板能力）。P12b 只是 mock 预览；完整 P12 需真实 HTTP/WS + Panel live 覆盖批准入口、拒绝、版本冲突、断流恢复。

## P11d Model 管理面（Control）

Control 提供 Model 管理面的 REST 转发，全部经 Identity 一次性委托（audience=`model`、单一精确 `RESOURCE_MODEL` scope、每次请求重新在线授权，不使用本地角色缓存）：

```text
PUT  /api/v1/models/{modelId}                          model.manage  UpdateModel
POST /api/v1/models/{modelId}/disable                  model.manage  DisableModel（expected_version 必填）
POST /api/v1/models/{modelId}/enable                   model.manage  EnableModel（expected_version 必填）
PUT  /api/v1/models/{modelId}/secret                   model.manage  RotateProviderSecret
GET  /api/v1/models/{modelId}                          model.read    GetModelMetadata
GET  /api/v1/models                                    model.read    ListModelMetadata（scope 必填，一个或多个 RESOURCE_MODEL）
GET  /api/v1/models/{modelId}/configuration            model.manage  GetModelConfiguration
GET  /api/v1/models/{modelId}/calls/{logicalCallId}    model.call.inspect GetCallStatus
POST /api/v1/models/{modelId}/calls/{logicalCallId}/resolve model.manage ResolveModelCall
GET  /api/v1/operations/model/{operationId}            resource.read GetOperation（仅 model owner）
```

约束：
- 密钥只写不读：轮换请求中的 `new_secret` 只转发给 owner，不写日志、不回显；读取只返回 `secret_configured`/`secret_version` 与脱敏配置。
- `expected_version` 缺失/过期映射 `CONFLICT`；未知/不可见模型 `NOT_FOUND`；委托失败在调用 owner 前终止（401/403/503），不得本地放行。
- `GetCallStatus` 可返回 `EXECUTION_UNKNOWN` 与 `may_have_billed`；查询超时/传输失败不等于 Provider 调用失败，客户端不得推断 FAILED。`ResolveModelCall` 只接受有限 disposition，同键重放返回 owner 快照，异处置同调用为 `CONFLICT`。
- `control_api.model_endpoint` 可省略；省略时 Model REST 路由明确返回 `UNAVAILABLE`（503），不伪造成功或空结果。
- `GET /api/v1/models` 必须携带 `scope`（一个或多个 `RESOURCE_MODEL`），按该精确资源集委托；metadata 只返回脱敏字段，不含密钥。
- `/api/v1/operations/{owner}/{id}` 统一 owner 分支：`model` 走 ModelService.GetOperation；`runtime`/`scheduler`/`host` 的 Control operation 适配器未接线时稳定返回 `CAPABILITY_NOT_SUPPORTED`（503），不按 404/405 猜测。
- 当前 target REST operations 仍登记 not_implemented（owner REST 契约状态另行翻转）；ModelService 生产 RPC 已由 P11d 证据翻转。

## WebSocket 频道

T09a 已实现同一监听提供以下频道；各频道状态如下：

```text
/ws/health      # health.updated 仍标 x-contract-status: not_implemented（频道可建但无可推送事件）
/ws/logs        # 需显式 log.read（P12 权限 48），任何默认角色都不派生
/ws/adapters
/ws/plugins
/ws/tasks
/ws/messages    # 已实现（W5）：仅推送 message.updated 元数据；需每用户显式 message.read
```

WebSocket 只推送经过权限过滤的数据。普通面板用户不能订阅全部私聊消息或完整模型上下文。

WebSocket 服务端使用 WSS；首帧必须是携带 access token 的 `hello` 或同时携带 access token 与已有会话 ID 的 `resume`，不能要求浏览器在握手时设置自定义 Authorization 头。成功后服务端发送 `SESSION_READY`，包含 `session_id` 和确认的 `last_sequence`；会话 ID 是绑定认证主体和频道的不可预测秘密，必须保护、过期和撤销，不能记录或放入 URL。断线后客户端携带会话 ID 和最后处理的序号重连。服务端保留 10 分钟事件，无法补齐时返回 `RESYNC_REQUIRED`：客户端丢弃旧会话、重新以 `hello` 建立会话，并重新拉取 REST 快照。客户端忽略未知事件及字段，已知事件触发对应快照刷新；日志事件 `log.updated` 的 payload 使用 OpenAPI `LogEntry`；订阅该频道要求显式 `log.read`。审计没有 WebSocket 频道，`/audit` 仅经 `audit.read` 的 REST 读取，二者不得复用同一权限。`message.updated` 是自包含元数据通知，没有可刷新的 REST 消息投影：客户端只触发既有刷新，不得据此重建历史，也不应期待消息正文；订阅需显式 `message.read`，未授予仍以 4403 关闭。每个消息组件都有唯一 `messageId`。

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

P07a-C 验收：真实桥帧测试覆盖 Action 的受理/派发/结果/证据/取消/过期/配额语义；QueryActionResult handler 已实现但尚无 Identity 在线授权链的端到端 RPC 测试，因此 Control REST 转发与 target API 的最终启用仍留 P12。P07a-B 已在 Runtime RPC 层实现该查询 handler（Control peer 认证 + `action.inspect` 在线授权 + 显式账号 scope；按 idempotency key 时强制 `owner_instance_id`），REST/Control 转发与 target 契约翻转仍留 P12/P07-C。

目标契约为（P07 冻结，Control 接线仍为 not_implemented，P12 接线）：两条查询映射 `RuntimeService.QueryActionResult`，caller 是持 `action.inspect` 的 scoped Control；必须带显式 `scope`（恰含一个 `RESOURCE_ACCOUNT` 的 `ResourceScope`，服务端在线校验），`by-idempotency-key` 还必须带 `owner_instance`（业务幂等按插件实例隔离），否则 `INVALID_ARGUMENT`；不存在/不可见/超保留统一 `NOT_FOUND`，`ACCEPTED` 且已过 deadline 读取为 `EXPIRED` + `error.code=TIMEOUT`，查询超时不改变状态。响应增加 `execution_outcome`（`EXECUTION_NOT_STARTED`/`EXECUTION_KNOWN_SUCCEEDED`/`EXECUTION_KNOWN_FAILED`/`EXECUTION_UNKNOWN`）区分平台执行是否已知；插件仍经 `PluginBridge` owner-scoped 查询，不走 REST。目标角色表中不保留 `action.read`，Action 查询只认显式 `action.inspect` 资源授予（admin 不隐式越权）。

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

### P01 目标管理 REST/WS（未接线）

target Model REST 端点（元数据/配置/密钥/Action 策略等）在本页保持 `not_implemented`；P11 新增的 `DisableModel`/`EnableModel`/`ResolveModelCall` 只冻结 owner RPC 与权限（`model.manage`、scoped Control、幂等、版本 CAS），REST/Panel 映射留 P12，不新增或改动 OpenAPI 路径。

OpenAPI 增加35个类型化管理操作：账号/适配器绑定、安装/实例期望和配置、资源删除恢复及 operation、Action元数据、账号危险动作策略、共享成员、共享 namespace 创建/读取（P09：`POST/GET /installations/{installationId}/shared-namespace`，POST 需 `Idempotency-Key` 与 `namespace.manage`，GET 需 `namespace.read` 与 exact installation scope；保持 not_implemented 至 P12）、死信重放、任务查询取消和occurrence、模型配置/密钥、固定角色授予及主体/凭据。路径 ID 与重复正文 ID 必须相等；未知 kind/owner、空 scope、缺少修改版本一律 INVALID_ARGUMENT。版本使用正十进制 uint64 字符串，防止浏览器精度丢失；创建不要求 expected_version。Control 从当前会话和路径构造内部上下文，不允许浏览器提交 delegation 或自报 actor。权限为每方法 x-permission 与明确资源授予的交集，未知权限拒绝；未实现方法须显式503/CAPABILITY_NOT_SUPPORTED，不返回假资源或假成功。

管理写要求 Idempotency-Key；成功 Control 收据24小时，下游业务命令收据至少七天，同键恢复前重新授权。202 响应只包含持久受理引用/版本；200 为本地已提交结果或当前查询，不证明跨服务全局完成。`/operations/{ownerService}/{operationId}` 按 runtime/scheduler/model 所有者路由并核验 ref.resource；资源类型仅接受 ResourceKind 已登记项。查询必须显式 scope，Action敏感读不靠admin默认越权。错误增加可选 failure_context，包含受理是否已知、operation、恢复动作、retry_after/current_version；Error.details 保持既有空对象语义，不返回供应商正文或秘密。

AsyncAPI operations 频道增加 operation.updated，首帧 hello/resume 认证并绑定资源 scope，SESSION_READY 后才回放/直播。owner_cursor 包含源、epoch、scope、sequence；owner断档/重启或超出窗口明确 RESYNC_REQUIRED，客户端读所有者快照。Blocked/UNKNOWN 不映射成功或失败；UI流不是可靠业务outbox或审计。Identity会话每250ms发起/共享验证，单次500ms，最后确认超过750ms即关闭，调度余量总计不超过1秒；不能兑现则实例不ready。原 messages 仍仅元数据及received子集，默认角色没有message.read。

以上均为目标契约声明。原已实现端点不因新增 schema 自动获得目标服务路由；实际 Control/Panel/所有者实现与故障验收属于 P04–P13。doc/02–05/07–08 的运行说明由对应实现阶段更新，本任务不声称已改变生产行为。

目标 schema 的成功响应主结果、资源/操作标识与版本、完整 Fence 均明确 required，不能用 `{}` 当成功。Fence generation 与已登记资源 version 为正 uint64；配置 applied_config_version、序号与计量可为0。TargetPositiveUint64/TargetUint64 使用精确范围正则及 maxLength=20，拒绝超过18446744073709551615、负数、小数和前导零。registered=true 的状态必须有完整 fence，尚未连接状态可缺；Blocked 必须有稳定 reason 和 next_attempt_at，可选诊断/观察不会被一律设必填。

创建实例与实例投影均按部署方式校验Host绑定：受管实例必须提供非空host_id，externally_managed=true的外部自管实例可省略host_id，不要求空字符串占位；显式提供空host_id仍拒绝。创建请求省略externally_managed按false处理，必须提供Host；实例投影必须明确输出externally_managed。Host存在性及授权由Runtime实现核验，schema只校验字段形状。

## P02 独立 Control 组合根（构建接线中）

`control/api/app/main.cpp` 是新 `kiarabot-control` 入口，装配
`application/target/application.hpp` 的进程本地 application 与公共 platform runner。
新 application 只依赖 Core 纯标准库 readiness 端口；不创建权威数据库，不注入旧
`IControlApplication`/RuntimeSnapshot，也不包含 Protobuf、HTTP、JSON 或数据库对象。
Control 具有独立服务身份，业务未实现、Identity 和业务凭据核验均阻止就绪。
端口返回不可改变应用状态的值快照；配置或调用方修改快照不能启用假 ready。

CLI、配置/TLS 校验、存活/就绪及有界关闭由外层公共 runner 提供，约定见
[P02 实施记录](../reports/execution/2026-11-01-architecture-p02.md)。存活不表示身份服务
或业务可用；本阶段 ready=false，REST/WS 业务入口明确拒绝。既有端点的历史 implemented
登记不表示这个新入口已实现其六服务路由。身份委托、所有者 RPC、会话/权限、收据与
浏览器流仍由 P04/P12 实现，不能用旧同进程调用填补。

本节不改变 OpenAPI/AsyncAPI 的字段、权限或错误映射，不把 target 能力标为已实现。
进程启动失败输出脱敏的稳定错误与退出码，不输出异常正文或秘密。实际构建/进程
验收结果随本模块报告和主集成记录更新；入口文件存在不代表 P02 完成。

目标GET /models/{modelId}/configuration映射ModelService.GetModelConfiguration，需model.manage及exact Model scope的当前在线核验，返回配置/secret存在性/observed_at，无秘密。scope参数为JSON编码ResourceScope（非空exact资源引用）。/operations/{ownerService}/{operationId}按runtime/scheduler/model实际owner路由，分别核验resource.read/task.read/resource.read及真实operation绑定；响应共用中立的operation结果形状，不强行视为Runtime结果。Model查询尚未实现，不因HTTP schema存在而启用生产功能。

目标GET accounts/installations/instances/adapter-instances/hosts及各/{id}分别映射Runtime typed Get/List；GET instances/{id}/config读取完整无明文秘密配置及独立应用版本。列表catalog参数是JSON编码TargetCatalogRef，只允许RESOURCE_CATALOG与离线登记UUID，返回当前本人独立获权child资源及SnapshotInfo；不可用依赖不假空页。单查路径构造exact固定kind。既有/adapters声明保留reference语境，不作为目标兼容桥。新GET默认2秒/5秒上限，逐方法account.read/plugin.read/adapter.read/host.read/plugin.config.read配合exact grant，不从角色或关系自动扩权；状态全部not_implemented。

## Identity 当前投影与凭据状态（目标未实现）

Identity 的 ValidateSession/GetPrincipal 返回当前主体的逐条 AssignmentRecord、authorization_version 和权威 UTC checked_at。每条绑定独立计算权限与 exact scope，不能把显示 roles/permissions 与全部 scope 做笛卡尔积授权；PrincipalRecord.roles/display_permissions 仅是当前绑定的显示并集。固定角色只用于 human；显式权限使用 role=UNSPECIFIED 加非空、去重的封闭 Permission 列表，两种授予依据不能混用。message.read/action.inspect 等敏感权限不由默认角色或创建资源自动授予。空 scope 不代表全局。

SetAssignment 输入 AssignmentSpec：创建不带 assignment_id/expected_version，Identity 生成 UUID/version=1；更新二者同时提供且版本为正，主体不可更换，并核验旧、新 scope 并集上的当前授予权限。目标尚未发布，原开发快照的 AssignmentRecord 请求已统一替换为 Spec，不提供旧请求桥接。

SessionRecord.created_at 是 Identity 持久保存的首次创建时间，刷新/撤销不改写。会话状态以同响应 checked_at 判定：revoked 优先 REVOKED，其次 checked_at >= expires_at 为 EXPIRED，否则 ACTIVE。Login/Refresh 的剩余 access TTL 为 max(0,floor(access_expires_at-checked_at))；Control 不用本地时间、token iat 或占位时间补齐。缺失权威字段、非当前绑定或授权依赖不可用必须拒绝，不能伪造空权限/成功。

RegisterPrincipal 在同一 Identity 事务创建 Disabled Principal 与无秘密 PENDING WorkloadBootstrap 凭据引用（credential_id/principal_id/binding/version/created_at/checked_at），allowed_services 作为 Pending audiences 持久化；尚未发放的 PENDING 不得伪造 expires_at。首轮成功的 RotateCredential（WorkloadBootstrap 或 human Password）在同一事务把 Disabled principal 激活为 Active 并前进 principal version；RevokeCredential 保留历史版本，撤销后该凭据的 Login/Validate 当前凭据门禁必须失败。GetCredentialStatus 以 identity.principal.read 在线读取同一记录，默认2秒/最多5秒；ACTIVE/EXPIRED 必须有 expires_at，REVOKED 可保留原到期。状态查询不返回秘密、摘要或可重放材料。初次实际发放仍用 RotateCredential 单次 CredentialDelivery，未知结果不得从状态查询恢复秘密。WorkloadBootstrap 轮换交付一次 opaque token（可信策略 24 小时），human Password 轮换交付一次服务端生成的临时密码并由 PasswordCryptoPort 重新派生 verifier；CertificateBinding 轮换属于证书登记流程，本 RPC 明确返回 CAPABILITY_NOT_SUPPORTED。交付加密收据 scope 为稳定调用者+credential+幂等键，expected_version 进入 keyed request commitment，同键异版本 CONFLICT；重放先校验当前授权与当前凭据/轮次。未授权与不存在统一 NOT_FOUND；v1 约束一个精确 binding 只对应一个 Principal（Identity 私有 PG 唯一索引，幂等键不改变该唯一性）。应用/仓储/RPC 已接线，状态为实现中，待完整门禁与真实 PG/TLS 验收后翻转契约状态。

目标 /auth/me、/auth/sessions、/auth/login、/auth/refresh 使用 x-target-owner-rpc 和 x-target-response-schema 指定严格目标响应，P04 必须验证 TargetCurrentPrincipal/SessionPage/TokenPair；不能降级为缺少绑定、checked_at 或 created_at 的 reference 响应。GET /credentials/{credentialId} 映射 Identity.GetCredentialStatus，响应不含秘密。既有 reference 标准响应保留历史断言，不构成目标兼容入口；目标状态均 not_implemented。

### 外部连接授权目标补全（未实现）

platform.proto三类ConnectionAuthorization/RefreshConnectionAuthorization/Result与两Bridge的登记、proof、refresh帧定义短期外部连接授权。名义最多56秒、总预算60秒、每秒检查失效，单飞原子轮换；丢响应须关闭旧流、新bootstrap与连接代次。完整规则见doc/01-contracts及运行语义；内部JWT委托和P02 mTLS owner监听不变，P05/P06真实接线前仍not_implemented。授权材料禁止日志/公开投影/持久秘密收据，高风险业务仍在线核验，刷新不续投递租约、不改历史业务事实。浏览器REST/WS不使用这些proof，OpenAPI/AsyncAPI因此无schema变更。

### P04 minimal HTTPS boundary

The independent Control composition owns its HTTPS endpoint and destroys it before
the application. The business listener is separate from the loopback probe.
Login/Refresh/Me/ListSessions/Revoke map to IdentityService; Identity owns all
session, authorization and receipt decisions. Control owns no business database
and never derives permissions by multiplying roles and scopes. Configuration and
endpoint shutdown are specified in doc/08; this does not change the REST contract.

P04 authentication boundary now maps `/api/v1/auth/login`, `/auth/refresh`,
`/auth/me`, `/auth/sessions` and `/auth/sessions/{sessionId}/revoke` to the target
IdentityService over verified mTLS. Login/Refresh/Revoke preserve the browser's
Idempotency-Key unchanged; Control has no success cache and makes no automatic
retry. Every protected request calls ValidateSession. ListSessions requests an
Identity-issued delegation for exactly RESOURCE_PRINCIPAL=current subject UUID,
IDENTITY_SESSION_READ and explicitly passes that principal_id. Revoke requests
exactly RESOURCE_SESSION=target UUID and IDENTITY_SESSION_REVOKE. The original
access proof and SHA-256 of the actual outbound Control leaf certificate bind the
delegation; audience is identity. Only Identity decides self-service/other-resource
visibility and grants. Browser actor/context/fingerprint fields are rejected.

JSON accepts only the declared flat string fields, rejects duplicate/unknown keys,
wrong types, nested objects/arrays and invalid bounds. Pagination accepts only
page_size (1–100, default 20) and opaque page_token, rejects duplicates and invalid
percent encoding. Authorization is never logged. Required token fields and remaining
TTL come from Identity checked_at and expiry; session state uses persisted created_at
and Identity's checked_at, with REVOKED before EXPIRED. uint64 versions serialize as
exact decimal strings. Display permissions and assignments are projections only.

The whole HTTP operation, including worker queue time and all upstream calls, shares
a two-second budget. Dependency or TLS failures return UPSTREAM_UNAVAILABLE/503,
deadline expiry TIMEOUT/504. Typed upstream stable errors retain the documented HTTP
mapping; a rejection without valid stable details fails INTERNAL/500. Free-text
upstream messages/details never reach logs or browser errors. Responses are no-store;
no CORS origin is enabled. Unknown routes return 404; wrong methods return 405/Allow.

The generated Identity fixture verifies transport mapping only. Real Identity,
registered Control peer and PostgreSQL bootstrap integration remain a separate gate;
Control readiness intentionally remains false while the wider business implementation
is incomplete. The historical same-process implementation above is reference-only.

P04 response validation additionally rejects malformed successful Identity responses:
current principal must be ACTIVE with a positive version and valid binding, current
session must not be revoked/expired at Identity checked_at, and timestamps must fit
the target int64-nanosecond time range. Resource/principal/session/assignment IDs are
canonical lowercase UUIDv4; versions stay positive and exact. The refresh deadline must
follow checked_at and cover the access deadline. A current original-pair replay may have
an expired access token: retain both original secrets/deadlines and return expires_in=0,
so the client can refresh. Control must neither renew its expiry nor turn this success
into INTERNAL. Session/revocation/current-generation decisions remain Identity's. Session
pages must provide a snapshot UUID and a still-valid expiry within ten minutes,
match the requested page size, and contain unique ordered records for the requested
principal. Missing metadata never becomes an empty successful page.

RPC request_id and trace_id use the same fresh UUID correlation. Received RpcFailure
details take precedence over broad transport categories: the stable code must match
the gRPC status and this request's correlation; acceptance, recovery, operation,
retry delay and current version are retained in HTTP failure_context. Invalid details
fail INTERNAL, including unknown enums and invalid operation IDs. Only absent details
allow the generic transport 503/504 mapping, without inventing acceptance/rollback.
Bearer scheme/media type matching is case-insensitive; JSON permits UTF-8 charset.

Control uses the shared boundary `runtime/platform/target_codec` for timestamp,
permission/resource/error conversion, HTTP status mapping and complete RpcFailure
validation. Unary details have no Error-only fallback. The HTTP envelope is checked
with from_http_failure before projecting its failure_context. Per-request correlation
and successful authentication response shape remain Control boundary checks.

### P04默认角色目录与Core一致性

目标角色能力以OpenAPI的`x-control-policy.target_role_capabilities`为唯一来源，不能
使用开发阶段`roles`表代替。model.read属于admin/operator/viewer；会话read/revoke及
account.policy.manage属于admin/operator。action.inspect与message.read必须显式授予，
不由admin等默认角色派生；角色能力还须与同一assignment的精确资源范围相交。
G08的account.read/plugin.read/adapter.read/host.read保留三角色，plugin.config.read保留
admin/operator。本人会话规则不变。`proto/tests/identity_role_catalog_test.rb`调用`--role-catalog`输出并
与API各角色精确集合比较，阻止文档表与实际授权函数漂移。

## P12c 首批 REST/WS 实现状态（2026-11-02）

P12c 在独立 Control 进程的 HTTPS 边界新增 owner-backed 转发层（`control/api/src/target/owner_gateway.cpp`），
只做传输/映射，不含本地权威数据：

- `GET /api/v1/actions/{actionId}` 与 `GET /api/v1/actions/by-idempotency-key`：要求显式 `scope`（恰含一个
  `RESOURCE_ACCOUNT` 的 `ResourceScope` JSON），按幂等键查询还要求 `owner_instance` + `Idempotency-Key`；
  Control 用调用者 Bearer 会话向 Identity `IssueDelegation`（audience=runtime、action.inspect、精确 scope），
  再原样转发 `RuntimeService.QueryActionResult`。不存在/不可见/过保留返回 404 `NOT_FOUND`，不推断动作执行失败；
  身份/权限/超时/不可用分别映射 401/403/504/503；Control 不缓存授权、不重试副作用。
- `GET /api/v1/logs`、`GET /api/v1/audit`：P12e 已声明 Identity/Runtime 只读 owner RPC
  (`IdentityService.ListLogs`/`ListAudit`、`RuntimeService.ListLogs`/`ListAudit`)；P12c-2 的 REST
  聚合接线尚未完成，边界仍返回 503 `UPSTREAM_UNAVAILABLE`，不返回成功空页。
- `/health/live` 仅进程存活；`/health/ready` 由进程 readiness 决定；受权 `/api/v1/health` 组件详情待 P12a
  健康契约与四依赖端点配置对齐后接入。
- 配置：`control_api` 新增可选 `runtime_endpoint`（Runtime owner mTLS 地址）；缺省时 Action 查询在委托/owner
  调用阶段稳定失败，不伪造结果。

**与 P12a 的接口缺口（首批未实现）**：账号/适配器绑定、安装/实例启停与配置（202+operation）、插件启停/配置、
群白名单（Runtime `ActionPolicy` 版本化）、任务查询/取消、模型配置/更新/密钥、主体目录 ListPrincipals 与
`log.read`/`audit.read`（权限 48/49）、健康三端点聚合完整规则、日志/审计数据源、WS 断流恢复与异步 operation
快照。上述端点占用现有 OpenAPI 路径但服务端 owner RPC/权限/配置端点仍待 P12a 契约与 P11d 管理 RPC 完整接线；
在其落地前仍按未实现处理（不得返回假资源/成功空页）。`

### P12c-2 已实现（2026-11-02）

- `GET /api/v1/principals`：转发 Identity `ListPrincipals`；要求登录态，经 `IssueDelegation`
  （audience=identity、`identity.principal.read`）在线授权；仅人类主体、`page_size`/`page_token`
  分页快照；owner 不可用返回 503 `UPSTREAM_UNAVAILABLE`（不返回空页），拒绝映射 401/403。
- `/health/live`：仅进程存活；匿名 `/health/ready`：仅当 Identity 会话校验/撤销链当前可达时 200，
  否则 503，且不暴露组件细节；受权 `GET /api/v1/health`：返回 `state` + `identity/runtime/
  scheduler/model` 四组件与 `checked_at`。Identity 不可用 → 503 `UNHEALTHY`；Runtime/Scheduler/Model
  单点不可用 → 200 `DEGRADED`（不整体摘流，对应 owner 功能 503），单项故障不产生 `HEALTHY`。
- `/logs`、`/audit`：先按 `log.read`(48)/`audit.read`(49) 分别取得一次性委托（缺失/拒绝 → 401/403）。
  P12e 已实现 Identity/Runtime 只读 owner RPC（真实 PG16.4 keyset 分页/过滤/脱敏/权限校验）；
  P12c 的 REST 聚合接线尚未合入，授权通过后仍返回 503 `UPSTREAM_UNAVAILABLE`，绝不返回成功空页。
- `control_api` owner 端点统一：`identity_endpoint`（必填）+ 可选 `runtime_endpoint`、
  `scheduler_endpoint`、`model_endpoint`、`host_endpoint`（host:port，逐项校验）；缺省端点使对应
  owner 读取稳定失败而不伪造数据。健康聚合用这些 mTLS 通道的当前状态观测。
- 尚未实现（P12d/P11d 依赖或后续批次）：账号/适配器绑定、安装/实例启停配置、插件配置、
  ActionPolicy REST、任务查询/取消、模型管理 REST（P11d 负责专有端点，通用 owner 委托层本任务提供）、
  WS 断流恢复/异步 operation 快照。以上不得返回假资源。

### P12c-3 已实现（2026-11-02）

在通用 owner gateway 上继续接入 P12a 已冻结的 owner RPC（均每请求一次 Identity 委托、mTLS 转发、
不缓存授权、不推断完成；成功响应只暴露持久 operation 引用，202/完成语义归 owner）：

- `PUT/DELETE /api/v1/adapter-bindings/{accountId}` → `RuntimeService.BindAdapter/UnbindAdapter`
  （`account.manage`、精确 account scope、`Idempotency-Key`、`expected_version`；响应剔除一次性
  `bootstrap_credential`，绝不经 REST 回显）。
- `POST /api/v1/installations` → CreateInstallation；`POST /api/v1/installations/{id}/instances`
  → CreateInstance（path/body installation_id 必须一致；响应剔除 bootstrap credential）。
- `PUT /api/v1/instances/{id}/desired-state` → SetDesiredState（`plugin.manage`、instance scope、
  expected_version）；`PUT/GET /api/v1/instances/{id}/config` → UpdateConfig/GetInstanceConfig
  （`plugin.manage`/`plugin.config.read`；三类事实由 owner 快照区分，不伪造 APPLIED）。
- `GET/PUT /api/v1/accounts/{id}/action-policy` → Get/UpdateActionPolicy（`account.policy.read/
  manage`、account scope、`expected_version`；群白名单/禁言踢出开关/每日预算随 policy 透传）。
- `GET /api/v1/tasks` → Scheduler ListTasks；`GET /api/v1/tasks/{id}` → GetTask；
  `POST /api/v1/tasks/{id}/cancel` → CancelTask（`task.read`/`task.schedule` + 可选 scope JSON、
  page_size/page_token；owner UNAVAILABLE/TIMEOUT 稳定映射）。
- 版本冲突 owner `CONFLICT` → 409；与 path 不一致/缺 expected_version → 400；无 owner 端点 → 503；
  错误体保持统一 Error。
- 尚未实现（后续批次）：实例级插件 enable/disable/config REST（不映射旧 `/plugins/{id}/enable`，
  操作对象必须明确为实例；需要 Host/插件确认状态来源）、WS/异步 operation 快照与断流恢复、
  operation 查询 REST、模型管理 REST（P11d/ref-infra）、日志/审计权威来源。

### P12c-4 已实现（2026-11-02）

- **operation 快照 REST**：`GET /api/v1/operations/{ownerService}/{operationId}`（ownerService ∈
  runtime/scheduler/model）按路径只路由到该 owner 的 `GetOperation`，经一次性 Identity 委托
  （runtime/model=`resource.read`、scheduler=`task.read`）与 mTLS 调用；`scope` 必填（恰一个资源），
  owner 以“存储的 operation 资源”做授权、客户端 scope 不能重标定。响应 `{"operation": {...}}` 含
  ref/version(string)/state/execution_outcome/participants[]/observed_at/reason/next_attempt_at/stalled；
  缺失/不可见→404 NOT_FOUND；200 只表示权威快照，绝不等于完成。Panel 对 202/200+operation 的写操作
  应轮询该端点。
- **schema 对齐**：tasks 列表改为 OpenAPI `TaskPage`（`items[{id,name,state,created_at}]` +
  `page{page_size,next_page_token}`），不再直接透传 proto `tasks/page`；instance config GET 已是
  `{"config": {instance,desired_version,values,applied_version,apply_state,...}}`，与
  `TargetRuntimeGetInstanceConfigResponse` 一致（uint64 版本仍为 JSON number，浏览器端需按字符串处理，
  已记录为 P12a/P12d 对齐项）。
- **WS/断流恢复**：独立 Control 目标进程目前只提供 HTTPS，尚无目标 WS 服务端（reference WS 不代表
  target 能力）；本批先把**权威 REST operation 快照**作为 resync 数据源落地。WS operation 进度事件/
  RESYNC_REQUIRED 仍为 not_implemented，需 P12a 定义 target WS 承载与发布者、P12d 对接后实现；客户端
  断流后应重新拉取 operation/资源 REST 快照，不得据旧流推断完成。
- **实例级插件 enable/config**：P12a/OpenAPI 仅保留旧 `/plugins/{id}/*`（不得机械映射到实例），尚无
  冻结的实例级路径；本批**未实现**，需 P12a 补 `/installations/{id}/instances` 下的启用/禁用与配置
  应用状态投影（Runtime 期望 / Host 观察 / 插件 ACK 三类事实分离）。旧 `/plugins` 路径保持非目标语义。
- 测试：gateway 单测覆盖三 owner 分支/权限映射/404/409/scope 必填/participants+stalled 字段/TaskPage
  形状；p04 真实 TLS 验收新增三个 owner operation 查询的稳定拒绝/不可用断言。

## P12a-2 契约补充（2026-11-02，target 登记，尚未实现业务）

本节记录 P12 实施暴露、由用户 Q1–Q8 确认范围补齐的契约面。除特别说明外，下列 REST/target 操作均为 `x-contract-status: not_implemented`，不表示当前 Control 已有生产 handler。

### 实例级插件启停/配置（Q6）

旧 `/plugins/{id}/enable|disable|config` 只描述单插件（reference）开关，**不得**机械映射到安装/实例；一个安装可有多个实例，操作对象必须明确。目标显式实例对象路径为：

- `PUT /api/v1/installations/{installationId}/instances/{instanceId}/plugin-state`：请求体含相同 installation/instance id、desired `RUNNING|STOPPED`、可选 `expected_version`；owner `RuntimeService.SetDesiredState`。202 是持久受理，不是进程已启动/停止。
- `GET /api/v1/installations/{installationId}/instances/{instanceId}/configuration`：owner `RuntimeService.GetInstanceConfig`；返回 Runtime 已保存期望配置/版本、Host 观察事实、插件 ACK/应用事实三组投影；密钥不返回。
- `PUT /api/v1/installations/{installationId}/instances/{instanceId}/configuration`：owner `RuntimeService.UpdateConfig`；幂等键 + `expected_version`；只持久化 Runtime 期望配置，Host/插件是否应用是独立事实。

响应必须把 **Runtime 期望状态/配置版本、Host 观察到的受管进程启动/退出、插件确认配置已应用** 三类事实分开，并带 `operation` 引用供查询进度；Host 离线/UNKNOWN 只表达 blocked/unknown，不伪造 stopped 或成功。Control 不得复用旧 plugin enable 路径。

### health.read 权限 50

`policy.proto` 新增 `PERMISSION_HEALTH_READ = 50`（0–49 不变）。匿名 `/health/live` 与 `/health/ready` 保持匿名；受权 `GET /api/v1/health` 要求显式 `health.read`（P12 permission 50），返回四依赖聚合与逐组件详情，不向匿名探针泄漏组件信息。

### target Control WebSocket：P12 v1 无 WS

P12 v1 target **不提供** Control WebSocket 服务端；`api/asyncapi.yaml` 的频道/帧是冻结目标契约并整体登记靶向 `not_implemented`（P12 v1 无 WS transport）。断流恢复以 REST operation 快照 `GET /api/v1/operations/{ownerService}/{operationId}` 为唯一权威 resync 源：客户端恢复后必须全量刷新，不得假设错过事件；未来若实现 WS，优先复用已登记的 `operations`/`operation.updated`、`health.updated` 等频道名与帧语义，不新增传输级私有协议。

### /logs 与 /audit 聚合契约

- 权威事实归各业务所有者；Control 只做只读聚合，不跨库读取、不复制事实。P12e 已声明并实现 Identity/Runtime 四个只读 owner RPC（P12e 契约点）：`IdentityService.ListAudit`/`ListLogs` 与 `RuntimeService.ListAudit`/`ListLogs`，caller 分别为 Control platform peer（Identity，需本主体 `audit.read`/`log.read` 显式 assignment）或 Control 当前委托（Runtime，需在线 `audit.read`/`log.read`），scope 固定 `RESOURCE_CATALOG` id `control`；Control 的 REST 聚合接线仍由 P12c 后续完成，未接线前返回 `503 UNAVAILABLE`，不得返回成功空页。
- `/logs` 需显式 `log.read`（48），只返回运维分类、脱敏后的结构化日志，默认保留 7 天；`/audit` 需显式 `audit.read`（49），返回业务事务内提交的必要审计事实，最小保留 180 天；二者互不蕴含，每行携带 owner 归属，分页按 `occurred_at`+稳定 ID，v1 不接受自由过滤条件。owner RPC 只返回本服务库内事实；响应脱敏字段固定为 entry_id/actor/resource/request_id/trace_id/method/rejection/authorization_version/occurred_at 与日志 severity/component/message，日志 message 是闭集 `request.recorded`/`request.rejected`，不含自由文本、凭据、消息正文或模型秘密。
- 凭据、消息正文、模型秘密和平台原始内容不得进入日志/审计投影；Control 不缓存延长保留期。

### 状态与字段统一

- P12c/P11d 的 REST 转发层与 Owner RPC 的 target 状态是两层事实：REST 操作可先实现转发，但 owner RPC 未 target 验收时该操作仍标 `not_implemented`；已实现 target override 的 REST 操作在后续证据齐备时再翻转，不以转发成功冒充 owner 完成。
- `/tasks` 列表行使用显式 `TaskSummary`（id/name/state/created_at），不再复用泛型 `GenericResource`；`TargetOperationRef.resource` 为可选：owner 快照没有资源时必须省略，不得补造占位资源。实例/操作目标响应继续以 OpenAPI typed schema 为唯一来源，P12c/P11d 原始 proto JSON 直出属待修实现差异，不得反向放宽契约。

### P12c-5 已实现（2026-11-02，P12a-2 契约）

- `PUT /api/v1/installations/{installationId}/instances/{instanceId}/plugin-state` → Runtime
  `SetDesiredState`：body `installation_id`/`instance_id` 必须与 path 一致（否则 400），`desired`
  仅 `DESIRED_RUNNING|DESIRED_STOPPED`，`expected_version` 可带且必须为正十进制字符串；Control 经
  一次性委托（plugin.manage、instance scope）+ Idempotency-Key 转发，返回三类事实：
  - `runtime`：Runtime 保存的 desired/version/observed_at/desired_config_version；
  - `host`：host_id/registered/process_outcome/observed_at（来自 Runtime 状态投影，Host 未确认时为
    `EXECUTION_UNKNOWN`，绝不声称进程已启动/停止）；
  - `plugin`：apply_state/applied_config_version/observed_at；仅当 applied≥desired 且非零时才是
    `CONFIG_APPLY_APPLIED`，否则明确 `CONFIG_APPLY_PENDING`/`CONFIG_APPLY_UNAVAILABLE`。
  成功响应含 owner operation 引用；409 版本冲突原样映射。**不使用旧 `/plugins/{id}/enable`**。
- `GET/PUT /api/v1/installations/{installationId}/instances/{instanceId}/configuration` → Runtime
  `GetInstanceConfig`/`UpdateConfig`：GET 需 plugin.config.read，返回 runtime.configuration/
  desired_config_version/delivery_id/apply_state/observed_at + host/plugin 事实；PUT 需 plugin.manage、
  必带 expected_version（正十进制字符串），body 的 `configuration` 只写 Runtime 期望值（秘密 write-only
  不回显），响应明确 `CONFIG_APPLY_PENDING` 并附 operation；Host/插件未确认的应用状态不会被写成
  APPLIED。
- `GET /api/v1/health` 现在要求 `health.read`（Permission 50）委托校验：缺 token →401，Identity
  拒绝/不可用 →403/503；匿名 `/health/live`、`/health/ready` 保持不变。
- 写操作返回的 operation ref 可由 `GET /api/v1/operations/runtime/{operationId}?scope=...` 轮询
  （P12c-4）；202/200 都只代表已受理/当前快照，不代表完成。
- 未实现/延期：WS 目标承载（P12a-2 已批准延期，REST operation 快照为 resync 源）；Host/插件 ACK
  的持久证据来源仍由 P08a/b/c 继续补齐；模型管理 REST 归 P11d。
