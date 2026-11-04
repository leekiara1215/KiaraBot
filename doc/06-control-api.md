# 控制面板 API 设计

## 当前阅读入口（2026-11-04）

当前 Control 为独立 HTTPS 服务，经 Identity 验证会话和委托，向 Runtime、Identity、
Scheduler、Model 的 owner RPC 转发管理操作；不使用 reference `IControlApplication`
或 RuntimeSnapshot。目标 WS 已批准延期，以 REST operation 快照恢复状态。
本页 T/W 记录是同进程 reference 历史，P 记录保留各自日期作用域；具体实现、
测试注册、本机实测和目标环境验收统一见 [阶段证据矩阵](27-phase-exit-evidence.md)。

## 接入向导进程内模型

Control 接入驱动模型、配置校验和接口见 [接入方案](26-real-qq-onboarding.md#第一项进程内通用模型与驱动端口)。
此阶段不新增 HTTP/WS 路由，驱动执行、操作持久化、权限和向导尚未接线；账号与绑定仍由 Runtime 拥有。

## reference T08a 冻结决策（2026-10-30）

控制 API 使用同进程 C++ 应用端口 IControlApplication（core/reference/include/kiarabot/core/application/control_application.hpp），由组合根注入到 control/api；不新增跨进程管理 RPC，不改变当前 Python 适配器/SDK 和 TypeScript 面板。端口覆盖当前全部 REST 操作；输入为 bearer 凭据、request_id/trace_id/deadline（最长三十秒）、幂等键及类型化命令，禁止自报 actor/role。Core 每次重新核验会话、执行授权、事务和审计；端口返回显式 PortResult，资源文档在输出前脱敏，HTTP 层按 OpenAPI 校验映射。T08b 已实现并完成本机 REST 服务验收。

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

当前独立 Control 的组合根在 `control/api/app/main.cpp`，内部应用和 readiness 位于
`include/kiarabot/control/application/target/`，传输与远端 owner 适配位于 `src/target/`：

```text
control/api/
  app/main.cpp
  src/target/
    https_endpoint.cpp
    identity_auth_client.cpp
    owner_clients.cpp
    owner_gateway.cpp
    owner_gateway/
      internal.hpp
      common.cpp
      health.cpp principals.cpp catalogs.cpp operations.cpp
      accounts.cpp instances.cpp tasks.cpp audit_logs.cpp
      lifecycle.cpp models.cpp actions.cpp
  tests/
```

`owner_gateway.cpp` 仅创建公共请求上下文、按原顺序选择 11 个资源族 handler、转换顶层异常；
`common.cpp` 共享错误、委托、分页和转换辅助，各 handler 保留自己的方法、输入校验和授权顺序。
`owner_clients.cpp` 实现远端 owner 适配，handler 不拥有业务状态、不访问数据库。
旧 `src/http_server.cpp`、`http_mapper.cpp`、`event_stream.cpp`、`configuration.cpp` 属 reference 路径。

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

## WebSocket 频道（WS-01 目标契约，2026-11-04）

唯一来源为 [`api/asyncapi.yaml`](../api/asyncapi.yaml)。本节冻结新增 target 契约，
不宣称服务端或 Panel 已实现：target transport、全部 target 频道、新 payload 和 owner
`ListOperationChanges` 的对外能力登记仍是 `not_implemented`。WS-02 已完成 Runtime/Scheduler/Model
同事务持久 journal，WS-03 已通过 Runtime/Scheduler 真实 PG/mTLS owner RPC 与在线授权验收；尚未完成
整条 WSS/Panel 验收前不翻转状态。历史 T09a/W5 reference 频道继续保留原帧，
不能将 reference 成功当成目标六服务的证据。

目标连接为 WSS，每个 socket 绑定一个 owner、一个规范化的精确 scope：

- `/ws/operations?owner_service=runtime|scheduler|model|host`：OperationHello/OperationResume；
  runtime/model 用 `resource.read`，scheduler 用 `task.read`，host 用 `host.read`。
  Runtime 允许的资源种类同 GetOperation，Model 仅 `RESOURCE_MODEL`，Scheduler 仅
  `RESOURCE_TASK|RESOURCE_INSTANCE`，Host 仅此 endpoint 自己的一个 `RESOURCE_HOST`。
- `/ws/health`：owner=control，scope 恰为 `RESOURCE_CATALOG/control`，需 `health.read`。
- `/ws/adapters`、`/ws/plugins`：owner=runtime，分别为精确 `RESOURCE_ADAPTER`/`RESOURCE_INSTANCE`，
  分别需 `adapter.read`/`plugin.read`；`/ws/tasks` 为 scheduler 的精确 task/instance scope，需 `task.read`。
- `/ws/logs?owner_service=identity|runtime`：单 owner 的 `RESOURCE_CATALOG/control`，需显式 `log.read`；
  不派生 `audit.read`，不新增 audit 频道。
- `/ws/messages`：owner=runtime，精确 `RESOURCE_ACCOUNT` 和显式 `message.read`；
  仅 `received` 元数据，无正文、模型上下文或 REST 消息历史。
- `/ws/control?owner_service=control|runtime|scheduler|identity`：只汇聚该 owner、该精确 scope 内
  已实现且逐事件授权的事件，不聚合 operations，不推导跨 owner 总序或原子快照。

Operations 首帧仍为 hello/resume+scope，其他 target 频道使用 TargetScopedHello/Resume；
目标 scope 必须 1–127 个不同资源，按 kind/id 规范化。仅 health/logs 的显式控制目录可用 catalog，
不继承 child 授权、不拼接不同 assignment 的权限与资源。access token 只放首帧，不使用 query、
cookie 或浏览器自定义 Authorization 头认证。检查同源 Origin allowlist；非浏览器无 Origin 需显式配置。

所有事件保留 `schema_version/event_type/sequence/occurred_at/payload` envelope。
`operation.updated` 使用 owner 的 TargetOperation 和逐项 `owner_cursor`；OperationSessionReady
在 SESSION_READY 三字段之外返回该已处理边界的 `owner_cursor`，覆盖第一条事件前断线。
Control sequence 是 JS safe integer，owner sequence 为十进制 uint64 字符串；二者不能互换。
SESSION_READY 的 last_sequence 确认客户端已处理位置，不是尚未交付的服务端 head。

`adapter.updated` target payload 为 `{id,version,target:TargetAdapterRecord}`，
`plugin.updated` 为 `{id,version,target:TargetInstanceRecord}`；id/version 必须与 target 对应字段一致。
保留 online/lifecycle/注册事实，以及 Runtime desired、Host process_outcome、Plugin applied 版本，
不伪造旧 HEALTHY/PluginState，也不含 configuration/bootstrap。target task payload 使用闭集
ScheduledTaskState 与安全元数据，不推送 TaskRecord.payload。旧 reference payload 是独立分支，
目标状态分支保持 `not_implemented`；plugin.updated.health_state 必须等待真实 owner 健康事实。

新增 owner `RuntimeService/SchedulerService/ModelService/HostManagementService.ListOperationChanges`
复用 `observations.proto` 三个消息。请求 context/scope/可选 cursor/page_size；缺 cursor 时只捕获已提交
head，返回空 changes 及 next_cursor。每项变化带自身 cursor，整页消费后才能推进 next_cursor；默认32、
最大100条，每次RPC总期限500ms。每页重新在线授权，资源必须来自真实 Operation.ref.resource。
Operation 和 participant-only 修订与 journal 同 UoW 提交；提交序高水位不能越过尚未提交的低序号，
不能直接把 nextval 分配序当提交序。journal 保留600秒并有容量/持久 low-water；owner epoch 表示
数据集、跨普通进程重启保留，恢复旧备份、重建日志或重置 sequence 时改变。Control 不持业务库，
也不以临时收集本进程 operation ref 冒充完整 scope 订阅。

WS-03 owner 查询应用在打开本地只读 UoW 前核验实际 Control mTLS leaf 与证书绑定委派，
再调用当前 `Identity.CheckAuthorization`。Runtime 用 `resource.read`、Scheduler 用 `task.read`；
每次请求按 exact scope 与 owner audience 校验，客户端 deadline、gRPC deadline、进程预算和500ms
取最短期限。空 cursor 只捕获 head，持久 gap/epoch 不匹配以 typed `CONFLICT/RESYNC_SNAPSHOT`
拒绝；依赖不可用不返回缓存成功。返回 final journal 快照及脱敏闭集错误、participant 和 observed_at，
无 config、secret、模型正文或委派。此独立入口不扩展 Scheduler 原有 Runtime 平台调用白名单；
Model/Host 接入同一纯应用边界由其 owner 工作流继续完成。

会话绑定 principal、Identity session、authorization_version、channel、owner 和 scope，
每250ms启动或共享当前Identity检查（500ms预算）；最近成功确认超过750ms即停止发送并关闭，
撤权/到期后整体不超过1秒。replay/live/慢写排队均适用，不用本地缓存延长授权。
失效4401、权限拒绝/身份或scope绑定错4403、首帧非法4400、依赖不可确认/超时/内部或未实现源1011、
慢消费者/限流1013、停机1001。Owner CONFLICT+NOT_ACCEPTED+RESYNC_SNAPSHOT 保守映射
`RESYNC_REQUIRED/CURSOR_EXPIRED`；Control本地边界非法为INVALID_CURSOR、Control重启为SERVER_RESTART，
发resync后1000关闭。TIMEOUT/UPSTREAM_UNAVAILABLE不伪成功空页；4401回登录，4400/4403/1013停止自动重试。

首帧5秒、text帧64KiB、待写队列最多128帧或1MiB；replay每批32、每流最多4096事件/4MiB，
最多1024 retained sessions/总64MiB。byte/event额度淘汰明确导致resync，不静默丢帧；active连接和
认证RPC另有满足撤销时限的接纳额度，并为REST保留容量。session_id、token、proof、cursor原文、
bootstrap、配置、消息/模型正文和上游自由错误文本均不进日志、指标或事件。

分批实现完整范围：首批 operation/health/adapter/plugin；第二批 task/log；第三批 message metadata；
Host operation 接独立Host owner工作流。每批需真实producer与恢复证据再翻状态，未实现源认证后
以1011失败关闭，不能建立假空流。断流从权威REST重新拉快照；消息无历史投影，不能重建漏失历史。
异步状态通知不替代操作完成事实、审计或副作用ACK，BLOCKED/UNKNOWN不得推断成功。

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

Identity orders the full session snapshot by ascending creation time and then session ID
before it returns the first page. Login can generate session IDs in any order.

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

本节及其分批子节保留 2026-11-02 实施记录，较后接线以对应后续章节和
[证据矩阵](27-phase-exit-evidence.md) 为准。2026-11-04 的入口拆分位置见页首“目录”。
安装下创建实例、实例 plugin-state PUT、configuration GET/PUT 缺少
`RuntimeResourcesPort` 时，在原输入校验和委托认证顺序完成后、实际 owner 调用前返回
HTTP 503 / `UPSTREAM_UNAVAILABLE`、`retryable=true`，不访问空端口、不产生业务副作用。
原有非法输入与 401/403 的优先级保持；这是本轮单独修复的缺依赖路径。

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
群白名单（Runtime `ActionPolicy` 版本化）、任务查询/取消、模型配置/更新/密钥、主体目录 ListPrincipals（P12l 已实现）与
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
  P07f-2 已将 RuntimeService Get/UpdateActionPolicy owner 方法及 ActionPolicy 消息/字段、五种
  Action payload（react/delete/mute/kick/approve）登记为 target implemented；OpenAPI REST 的
  `x-contract-status` 仍按 owner-RPC 规则保持 not_implemented，REST handler 证据见 P12c-3。
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

### target Control WebSocket：WS-01 契约与实现状态

2026-11-04 已按 [获批扩展计划](29-approved-capability-expansion-plan.md) 更新本页
“WebSocket 频道”目标契约：单 owner/exact scope、有界持久 owner change feed、首帧和恢复。
`observations.proto` 与 AsyncAPI 是新的公共契约，全部新增 target 能力仍 `not_implemented`；
当前 target HTTPS 尚未接入 WS，REST operation 快照仍是当前权威恢复方式。后续继续实现与验收，
不再把历史延期或“只文档化”当作这项获批范围的终止条件。Host/Bootstrap 由其工作流提供真实owner
和秘密交付边界；WS永不推送bootstrap明文。

### WS-04 target transport 模块（实施中）

目标 WSS 使用现有锁定 Boost.Beast/Asio/OpenSSL，RFC6455 Upgrade、frame 与 TLS 由库处理。
Control 的配置新增 `control_api.websocket_origins`（明确 HTTPS Origin 列表，缺省空）和
`websocket_allow_missing_origin`（缺省 false）；两者缺省时所有 Upgrade 均拒绝，REST 继续服务。
浏览器 Origin 必须逐字匹配允许列表；非浏览器缺 Origin 仅显式允许时可进入首帧认证。

该模块另有有界 WSS worker、active socket/auth RPC 配额与 retained delivery session，保留 REST
四 worker。首帧5秒、文本帧64KiB、待写128帧/1MiB；过载1013，源不可用1011。每轮空闲轮询也
走当前 ValidateSession→IssueDelegation→owner ListOperationChanges，不等待业务变化才检查撤权。
会话绑定人类 principal、Identity session、授权版本、频道、owner、规范化 scope；恢复时重新在线
认证并核对已处理 sequence/owner cursor。事件应用后才推进客户端游标；REST 快照负责恢复当前状态。
恢复旧业务备份必须在后续受控离线工具实际 rotate dataset epoch，不能把普通进程重启等同新数据集。
本节描述正在实现的模块边界，全部 target WS 状态待真实 WSS/Panel 验收后统一登记。

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

### P12c-6 已实现：/logs、/audit 聚合（2026-11-02）

- `GET /api/v1/logs` 仅使用 `log.read`(48)，`GET /api/v1/audit` 仅使用 `audit.read`(49)；两者互不
  替代，缺 token→401，权限拒绝→403，非法参数/cursor/时间窗→400。
- 数据源：Identity 由 Control 以平台 peer 直连 `IdentityService.ListLogs/ListAudit`；Runtime 由
  Control 每请求执行一次 `IssueDelegation(audience=runtime, permission=48/49, scope=Catalog/control)`
  后调用 `RuntimeService.ListLogs/ListAudit`。任一权威源不可用/未配置→503 `UPSTREAM_UNAVAILABLE`，
  不返回部分页或成功空页；两源均可用但无数据时返回 200 空页（合法空结果）。
- 合并与游标：两源按 `occurred_at DESC, owner ASC, id DESC`（id 为十进制时按数值，否则按字典）
  合并；每行标注 `owner`（identity/runtime）。分页 token 是 **Control 复合 keyset**：分别携带
  Identity 与 Runtime 已消费到的 `occurred_at:id`（或显式 exhausted 标记），下一请求把各自
  cursor 精确还给对应 owner。不能把单一 owner 的 cursor 同时透传给两源：某 owner 一页只贡献
  合并页前缀时，另一 owner 中位于“合并页末行之前、该 owner 页末行之后”的行会被跳过；
  P12e-2 已修复并覆盖两页以上/同时间戳/无重复无遗漏测试。默认 page_size=20，最大 100；
  `occurred_from`/`occurred_until`（含端点，RFC3339）、`actor`、`resource`
  （JSON TargetResourceRef）为服务端校验过滤；不引入自由文本/任意字段查询。
- 投影/脱敏：仅按契约字段投影；日志 `level` 由 owner severity 闭集映射（INFO/WARN/DEBUG/ERROR），
  `message` 只来自 owner 已脱敏闭集；审计含 `id/occurred_at/owner/actor/action(method)/resource/
  request_id/rejection/account_id/conversation_id`；绝不返回凭据、消息正文、模型密钥、上游正文或
  自由错误文本。Control 不缓存审计/日志，不扩大 owner 保留期（audit 180 天、日志 7 天，见 doc/22）。
- **P12e-2 真 TLS 成功链（2026-11-02）**：真实浏览器 TLS → Control → 互认证 Identity/Runtime
  fixture 的 `ListAudit/ListLogs` 成功页验证：两源各返回权威行，Control 按上述顺序合并、标注
  owner、复合 token 翻页（2 行/页共 4 页）无重复无遗漏；`actor`/`resource`/时间过滤转发到
  两源且审计只回 identity 行；缺任一源仍 503，401/400/403 语义不变。真 PG16.4 另验证相同
  `occurred_at` 的 id DESC 与 keyset 全量分页唯一性（p03_postgres_database）。
- 与 P12d：REST 分页 `page.next_page_token` 直接回传；`owner`/`rejection` 可用于跳转 owner 操作；
  503 必须显示"数据源暂不可用"，不得渲染为"暂无日志/审计"；WS `logs` 频道仍为 P12a-2 延期项，
  REST 聚合不改变该状态。

### P12c-7 已实现：生命周期删除/恢复 REST（2026-11-02）

- `POST /api/v1/resources/{resourceKind}/{resourceId}/delete` → Runtime `BeginDelete`；
  `POST .../restore` → Runtime `RestoreResource`（仅 DELETED 且在 owner 30 天恢复窗口内）。
- Control 每请求执行一次 `IssueDelegation(audience=runtime, permission=resource.manage（ACCOUNT 为
  account.manage）, scope=路径 resource)`；body 必须含 `resource`（kind+id 与 path 一致）和正十进制
  `expected_version`，并必须有 `Idempotency-Key`；不一致/缺失→400，未登录→401，拒绝→403。
- 成功统一返回 **202 + `{operation:{owner_service,operation_id[,resource]}}`**；operation 是唯一进度
  权威，客户端轮询 `GET /api/v1/operations/runtime/{operationId}`。**BLOCKED 参与者的 operation 仍是
  成功受理（202），不得显示为删除/恢复失败**；CONFLICT（状态不允许/版本冲突）→409，NOT_FOUND→404，
  owner 不可用→503。
- 管理面不级联：Control 不直接删除数据、不绕过 owner 的 gate/参与者，不自动迁移；Host/Scheduler 等
  参与者由 Runtime operation 推进（未接线时保持 BLOCKED）。
- operation 查询分支：`runtime`（P13 BeginDelete/Restore/GetOperation）、`scheduler`、`model` 由 P12c-4
  已实现转发；`host` 不在 OpenAPI `/operations/{ownerService}` 枚举/owner RPC 映射内（P12a 未声明），
  Control 按非法 owner 返回 400，不伪造 host operation；participants/blocked/reason/next_attempt_at 由
  `/operations` 权威快照透出（P12c-4）。

## P06f/P07 adapter 观测补充（2026-11-02）

P07d 真实 adapter e2e 的事件 fan-out 断言现已覆盖：真实 adapter 入站消息 → Runtime `events(MessageReceived)` → 订阅 SDK plugin 的 `delivery_targets`（终态 ACK）→ 未订阅不投递、离线不伪造 target、重连后新事件投递。adapter 配置 `ONEBOT_SELF_ID` 只用于平台 bot 标识/入站来源校验，Runtime REST/WS 契约不变。

## P12f 退出验收矩阵（2026-11-02，main@3ac3761）

真实 `kiarabot-control` 进程 + 真实浏览器 TLS + fake Identity/Model/Runtime/Scheduler owner 的逐端点
验收已完成，详见 [P12f 报告](../reports/modules/architecture-control-panel.md)。结论：
- 已实测成功链：健康、Action 单条查询、principals、logs/audit 双 owner 合并、绑定/解绑、安装/实例创建、
  plugin-state/config、action-policy、tasks list/get/cancel、模型全管理（含 Disable/Enable）、runtime/
  scheduler/model operation、delete/restore 202+operation。
- 已实测错误语义：401/400/403/409/503；runtime operation 的 BLOCKED 以 200 快照返回，不当作失败；
  无源/不可用 503 不伪造空页；旧 `/plugins/{id}/*` 不复用返回 404。
- 未验收：目标六服务真实栈（owner 为 fake）、Panel 浏览器真实 HTTP、Host 参与者真实停止/插件 ACK、
  WS 频道（延期）、`listActionMetadata`（契约未声明）、operation host owner（未声明）。
- P12f 修复真实缺陷：owner gRPC `ABORTED/NOT_FOUND/RESOURCE_EXHAUSTED/UNIMPLEMENTED/INTERNAL`
  未映射，CONFLICT 曾被降级为 503；现已映射为 409/404/429/503/500。

## P05f Identity/Runtime 出口核对（2026-11-02，main@439fb9e）

- Control 的 P04 认证边界与 Identity owner 目标路径已由真实 TLS harness 覆盖：`p04_control_auth_https` 通过（严格输入、撤销、依赖故障、停机）；P04 本机 `p04_identity_*` 28 项通过（含真实 PG16.4/TLS daemon/daemon_keys 测试）。
- Runtime 的 owner 路径由 `p05_runtime_*` 4 项覆盖，其中 `p05_runtime_identity_e2e` 为真实双进程 mTLS/委托/PG16.4、允许与撤销后拒绝；这仍是本机证据，不代表目标六服务栈 live。
- target 状态：Identity 7/23 方法、Runtime 10/55 方法已登记 implemented；P04 认证链、P05 核心资源方法、ListPrincipals 等未翻项、`bootstrap_credential` 字段级与 P05b 委托缺口见 [P04 报告](../reports/modules/architecture-p04-identity.md)/[P05 报告](../reports/modules/architecture-p05-runtime.md) 的 P05f 段。
- Control 消费说明：P04 认证/撤销语义不变；P05 的 `runtime_endpoint` 必须配置且 Identity 的 Runtime/Scheduler/Model platform peers 已离线登记，否则 fail-closed/503；P05b 实例委托未确认前，Model 深链路不宣称完成。

## P10i/P14g Execution（2026-11-02，main@4e88fb4）

- `GET /api/v1/occurrences/{occurrenceId}` 已接线 Scheduler `GetOccurrence`：required `scope` 查询必须恰含一个资源（通常 `RESOURCE_ACCOUNT`），Control 以该 scope 发 `task.read` 一次性委托；成功返回 owner occurrence 投影（occurrence_id/task_id/state/handoff/accepted_event_id/attempts/last_error/时间戳），缺 scope/未登录/非法→400/401，owner 拒绝→403/404/409，owner 不可用→503。目标执行范围已翻转，REST OpenAPI marker 按 owner-RPC 规则保持 not_implemented。
- **Control 委派 scope 修复**：所有 owner-backed 请求禁止空 `ResourceScope`；`/health`（health.read）、`/principals`（identity.principal.read）、安装/实例创建（plugin.manage）改为显式 `RESOURCE_CATALOG id=control`，其余绑定/配置/策略/任务/模型/operation/delete-restore 使用契约要求的 exact resource scope。Identity owner 校验该显式 catalog/resource scope；P04/P12 真实 TLS/fixture 成功链已覆盖。
- **`/health/ready` 语义**：Control 进程 probe readiness 由组合根观测驱动——TLS 材料已加载、Control HTTPS 组合完成、Control→Identity 校验/撤销 channel READY 才 HEALTHY；Identity 不可达时返回 503 UNHEALTHY，而不是固定占位 false。受权 `/api/v1/health` 的四依赖聚合语义不变（Identity 故障整体未就绪，Runtime/Scheduler/Model 单点降级）。

## P12k REST target 状态一致性审计（2026-11-02，main@3f43c8f）

只读审计脚本：`scripts/quality/check-contract-status-consistency.rb`（Ruby，读 `api/openapi.yaml` +
`proto/tests/contract_status.json`，不修改契约）。分类规则：

- **A 应翻未翻**：target operations override=implemented 且有 evidence、无 `x-owner-rpc`、REST
  marker 缺失/not_implemented。本次仅 **`listModels`（GET /models）1 项**，已补
  `x-contract-status: implemented` 并更新描述。
- **B 规则保持 not_implemented**：带 `x-owner-rpc` 的 REST operation。`target_api_test.rb` 强制其
  REST marker 为 not_implemented（不论 target override/真实转发证据），因此 **59 项保持
  not_implemented**；包括全部 Model 管理/查询 REST（get/update/disable/enable/secret/config/
  call/resolve）、tasks/cancel、operation、logs/audit、delete/restore、action-policy、occurrence 等。
  这些 operation 的 target 执行范围/证据可以 implemented（P11d-2/P12c-8/P10h/P10i），但不能把 REST
  marker 翻成 implemented；否则 target_api 门禁失败。
- **C 无实现/证据**：**19 项**保持现状（多为 reference 兼容路径/未声明 target 或缺少 target
  evidence 的 auth/legacy/principal 等）；不伪造状态。
- **D/E 过度声明**：本次 **0 项**（无 marker implemented 但 target 未登记，也无 owner-rpc 却标
  implemented）。

结论：`target_api` 规则不放宽；Model 管理 REST 的“成功 e2e vs marker not_implemented”是 B 类
设计约束，不是漏翻；仅 `listModels` 属 A 类并在本任务修复。审计脚本在修复后应输出
`PASS (no A/D/E drift; B owner-rpc markers are documented)`。

## P12l：ListPrincipals 主体目录实现（2026-11-03，main@fc454bc）

- Identity `ListPrincipals` 已实现：Control platform peer 经真实当前凭据/SAN 认证；要求
  `identity.principal.read`（显式 assignment，默认角色不隐含）；按调用者当前获授资源范围过滤：
  `RESOURCE_CATALOG id=control` 为管理目录全域，其他 exact resource 仅返回与该资源绑定的主体及
  其可见 roles。只返回人类 principal；按 principal_id keyset 稳定分页（page_token opaque），
  `checked_at` 取真实 DB UTC；deleted/purged 永不返回。`GET /api/v1/principals` Control 转发不变
  （identity_endpoint + 一次性 delegation；缺源 503，不伪造空页）。
- target 登记：`operations.listPrincipals` + 3 个 Principal 目录消息 + 12 个字段 target
  implemented；`target_spec` 同步；OpenAPI REST `x-contract-status` 翻 implemented，而
  `x-target-contract-status` 按 target_api 认证投影规则保持 not_implemented（不是 owner-rpc REST
  marker 场景，规则不同）。仍不暴露凭据/assignment 内部。

## P12m：owner RPC scope_kinds 元数据补全（2026-11-03）

P14k 原生栈发现 Identity `CheckAuthorization` 对 4 个 owner 方法返回 INVALID_ARGUMENT：这些方法
缺少 `option (scope_kinds)`，descriptor-driven 方法策略无法接受任何精确 scope，Control 无法合法
委派。P12m 只补既有方法元数据（不新增字段/权限/RPC）：

- `RuntimeService.GetActionPolicy`、`UpdateActionPolicy` → `RESOURCE_ACCOUNT`（ActionPolicy 归账号）。
- `RuntimeService.QueryActionResult` → `RESOURCE_ACCOUNT`（P07 契约：`action.inspect` + 恰含一个
  `RESOURCE_ACCOUNT` 的显式 account scope；target_spec 原有 caller/permission 未定义 scope_kinds）。
- `ModelService.ListModelMetadata` → `RESOURCE_MODEL`（对照 `Chat`；列表请求的 scope 是一个或多个
  RESOURCE_MODEL）。
- `proto/tests/target_spec.json` 镜像以上 4 项，`p01_target_roundtrip`/`target_contract_test.py`
  校验 descriptor 与 target_spec 一致。
- 解锁：P14l 可复跑原生 models/action_policy/action_query owner 链；本项不改变任何 wire 字段、
  权限或 RPC，上一层的 Control REST marker/错误映射不变。

## Model 配置更新及 Provider 密钥轮换

2026-11-04 修复既有映射缺口：disable、enable、resolve 与其余要求幂等的 Model 写操作一样，
将必需 `Idempotency-Key` 原值写入 owner RPC 的 `RequestContext.idempotency_key`。
相同键的请求仍委托业务 owner 判断重放，不在 Control 缓存成功；缺失或空键拒绝且不调用 owner。
这项修复遵守现行 OpenAPI/proto，区别于本轮其余请求入口的职责拆分。

Model 配置更新及 Provider 密钥轮换在 Control 转发后按对应 owner RPC 的 `RESOURCE_MODEL` 描述符在线核验；Control 调用者及委托主体须对路径中的模型具有 `model.manage`。两条 PUT 均将 HTTP `Idempotency-Key` 原样写入 owner 请求上下文。

## P14m：/logs /audit Runtime 源 401 修复（2026-11-03）

native 栈 Control `/logs` `/audit` 的 Runtime 源曾稳定 UNAUTHENTICATED：Runtime 委派 claim
解析把 P12 权限 `log.read`(48)/`audit.read`(49) 按旧上限 46 拒绝，并把 `RESOURCE_CATALOG
id=control` 当作非 UUID scope 拒绝；两者都发生在 Identity 在线 CheckAuthorization 之前。
P14m 修正 Runtime 结构校验（不扩 proto、不改权限/接口）：权限按 known_permission 接受 48/49
（上限 health.read=50），Catalog scope 允许契约 id `control`，其他资源仍 UUID；最终授权仍由
`Identity.CheckAuthorization` 在线逐命令执行。授权成功且无数据时为 **200 空页**，权限拒绝/
过期/篡改分别稳定 PERMISSION_DENIED/UNAUTHENTICATED，不伪造成功。

## P12n：Control list 端点序列化与 OpenAPI 对齐（2026-11-03）

修复 `GET /api/v1/models`：handler 曾直接 `MessageToJsonString(ModelListModelMetadataResponse)` 输出
`{"models":[...],"page":{...}}`，而 OpenAPI `ModelPage` 要求
`{"items":[GenericResource{id,name,state,created_at?}],"page":PageInfo}`。现按 `task_page_json` 同类风格
显式映射：`id=model_id`，`name=alias`（空则 model_id），`state=LifecycleState`，page_size/next_page_token
来自 SnapshotPage；不再透出 raw proto 字段（如 config_version/provider_kind）到 list 投影。单条
metadata/config/call/operation 响应仍按各自 OpenAPI target schema 返回。

**全 Control list/查询端点序列化审计**（现有 yaml 为准，不修改契约意图）：
- `/tasks`：自定义 TaskPage `items/page`；`/tasks/{id}`、`/occurrences/{id}` 返回各自 target schema 的
  `task`/`occurrence` 对象；`/operations/{owner}/{id}` 为目标 Operation 投影。
- `/principals`：返回 `TargetIdentityListPrincipalsResponse`（principals/page/checked_at），与 OpenAPI
  target schema 一致，不是 raw-proto 泄露。
- `/logs`、`/audit`：自定义 `items/page`（LogPage/AuditPage），owner 标注与分页为显式映射。
- `/health`、`/auth/*`、绑定/安装实例/config/desired-state/action-policy/models 单条：均按各自
  OpenAPI schema 的显式或 target 投影返回；未实现的 list 端点（如 installations/instances/hosts/
  bindings 列表）无 handler，保持稳定 404/503，不伪造空页。
- 本次未发现其它 `{rawProtoRepeatedField:[...]}` 形式的 list 泄露；`ModelPage.items` 是唯一确定性
  shape 缺陷（P14k 修复 owner 链后真实 200 暴露），p12j_panel_live 现通过。

OpenAPI `ModelPage`/`GenericResource` 契约本身与意图一致，无需改 P12a；仅修实现映射。

## P05 目录授权接线（2026-11-04，已批准）

创建账号和安装的请求必须显式携带 `catalog`，类型为 `TargetCatalogRef`。Control 原样转发该目录，以账号管理或插件管理单一权限签发 Runtime 委托。实例创建以路径和 body 一致的父安装签发精确安装委托。创建不会自动授予新资源权限。目录族到 UUID 的权威映射由 Runtime 验证，Control 不从 catalog ID 推断资源族或赋予权限。

五类目录列表逐次使用真实 ValidateSession 的当前 AssignmentRecord。只选择同一方法读权限和 child kind 的精确资源，将目录作为独立引用加入同权限委托，不拼接权限与全部资源的笛卡尔积。可选 `scope` 查询只选择本次查询的明确 child 引用，最多 127 个；未提供时选择当前同权限 assignments，超过上限整体返回 RATE_LIMITED，不静默截断。不同选择是独立快照和分页，某批末页不代表全量目录末页。127 是委托逻辑上限；HTTPS 既有 8192 字节请求头上限仍适用，percent-encoded 查询须使用更小批次。目录权限和逐对象授权均由 Identity/Runtime 复核。Host 当前权威来源缺口保留其稳定拒绝。

参数缺失或错误 kind 返回 INVALID_ARGUMENT。未获授权、已撤权由 Identity 返回 PERMISSION_DENIED；依赖故障沿用 UPSTREAM_UNAVAILABLE，超时 TIMEOUT。创建响应的秘密字段继续按现有一次性交付边界处理。该项外部可观察变化同步 OpenAPI 和相关 Panel 请求，异步事件、Action/Model/Scheduler 契约无需同步，受保护基线保持只读。

资源写路径逐字转发调用者的 `Idempotency-Key` 到 Runtime RequestContext；缺失/空值或超过 128 字节返回 INVALID_ARGUMENT。同键换 request_id 不生成新业务幂等身份，回执内容仍由 Runtime 持久保存并重新授权后返回。Control 不合成回执。

P05 Control 可复用容器验收入口：`tests/architecture/p05_control_catalog_smoke.py --base-url https://localhost:PORT --ca-file /local/ca.pem --token-file /local/access-token --catalogs-file /local/catalogs.json --expect-host-gap --create-account`。catalog 文件为五个资源族到 UUID 的同部署配置；可选 `--selections-file` 显式指定独立批次子 UUID 数组。脚本覆盖正常列表、游标重复读取、空选择、跨族/缺目录拒绝及创建同键换 request_id 重试，不修改 Identity grants。创建后的精确 Get 拒绝、合法权限供给、真实 owner 撤权/持久回执重启仍须以真实 Identity/Runtime 验收，不得把 fixture 的授权矩阵当作该证明。

可加 `--create-installation` 验收安装创建；`--parent-installation UUID` 必须使用已获精确管理权限的既有父安装，验收实例创建。CreateInstance 沿用 OpenAPI 200 的原提交结果或当前授权投影语义：重试保持原实例及业务结果，当前投影的 `state.observed_at` 可以刷新，不能据此宣称完整冻结状态回执。冻结提交时状态及观察时间的本轮保证仅适用于 DisableAccount、BindAdapter、UnbindAdapter、SetDesiredState、UpdateConfig 五项获批管理操作；普通重放不包含 bootstrap 明文。

## 受控 bootstrap 交付模块（2026-11-04，BOOT-03）

Control的独立SecretDeliveryApplication/SecretDeliveryPort接受已获owner授权的投递意图，纯内层类型
不含protobuf/JSON/SQL/文件系统。调用方必须先完成Runtime与Identity授权；该模块不自行签发凭据、
不创建业务资源、不替代Runtime交付operation权威。本切片仅交付可复用模块，尚未将普通REST/Runtime
签发/报告/重发链路接入，不能宣称bootstrap字段级闭环或target能力完成。

意图绑定delivery_id、精确ACCOUNT/INSTANCE、固定recipient_id、credential_version与expected_version；
明文仅进入store输入，无秘密receipt/query响应。recipient配置由组合根离线提供，绑定固定资源、固定
peer文件名、目录及消费者UID/GID，客户端不能指定路径、owner、argv或env值。普通REST继续剔除明文，
旧create/bind省略recipient维持原行为。本模块的store同identity+材料幂等；同identity异材料、旧版本或
expected_version不匹配CONFLICT。新版本必须恰为expected_version+1；初次expected_version=0。

外层文件sink持久intent后再原子安装0600秘密文件，最后持久无秘密receipt。中断后的query只核对固定
文件的版本/受限摘要，匹配则补receipt，无法确认则返回UNKNOWN；缺receipt不等价未写。迟到旧worker
被版本CAS拒绝，不能覆盖新材料。IO在效果可能发生后失败返回ACCEPTANCE_UNKNOWN/QUERY_SAME_INTENT，
忙锁为UPSTREAM_UNAVAILABLE/Backoff；路径/owner/模式边界fail-closed。该sink不创建秘密数据库、不实现
加密协议，摘要只用外层成熟SHA-256适配器；文件与metadata均不由Control普通业务查询直接暴露。
