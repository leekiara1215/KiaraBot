# TypeScript 控制面板设计

## 责任

控制面板是 API 的客户端，负责展示状态和提交用户操作，不拥有任何核心业务规则。

## 推荐技术

第一版建议：

```text
TypeScript
Vue 3 或 React
Vite
OpenAPI 生成客户端
WebSocket 客户端
```

具体前端框架可以替换，不能改变 API 契约。

## 目录

下图为目标目录；当前代码集中在 `src/App.tsx`、`src/api/`、`src/stream/` 和 `src/mock/`。

```text
panel/
  package.json
  src/
    api/
    features/
    routes/
    components/
    websocket/
  public/
  tests/
```

`src/api/` 使用 OpenAPI/AsyncAPI 生成的客户端类型；面板不直接导入 `core/`、`infrastructure/` 或适配器代码。

## 页面分区

以下为目标页面，不表示全部已实现。当前页面包括总览、机器人、适配器、插件、模型、权限、任务、日志、审计和动作（Action 查询）。插件页提供启用/禁用/配置/校验/健康动作，权限页提供会话列表与撤销，动作页提供 Action 查询接线；其余页面只读展示服务端资源。管理动作只调用 `api/openapi.yaml` 已就绪的端点，面板不实现业务规则。

```text
登录
总览 Dashboard
机器人和账号
适配器连接
插件市场和插件管理
模型提供商
群组与用户权限
会话和记忆
定时任务
实时日志
审计记录
系统设置
```

## 数据规则

- 所有服务器数据来自 API；
- 前端状态只做缓存和展示；
- 表单提交前可做体验性校验，服务端仍必须再次校验；
- 不把 Token 或 API Key 写入浏览器持久化存储；
- 日志和消息内容按 API 返回的脱敏结果展示；
- 前端不直接调用 NapCat 或数据库。

## 实时状态（WS-01 目标契约，2026-11-04）

本节规定新增 target 客户端行为，尚未声称 Panel 已接线；全部 target WS 仍 `not_implemented`，
历史 reference ControlStream 与 mock 成功不代表目标 WSS 验收。频道、帧、payload和限额以
[`api/asyncapi.yaml`](../api/asyncapi.yaml) 为唯一来源。

每 socket 绑定单一 owner 与明确 scope；operations URL 用 `owner_service=runtime|scheduler|model|host`，
首帧 OperationHello/Resume 携带 scope。其他 target 频道使用 TargetScopedHello/Resume，scope为
1–127个不同精确资源（health/logs仅显式control目录）。operations的权限沿owner REST：
runtime/model resource.read、scheduler task.read、host host.read。面板不以display_permissions裁决真实授权，
不拼接资源和权限，不把catalog授权当child授权；多个owner使用独立连接与cursor。

成功 SESSION_READY 确认已处理sequence；OperationSessionReady额外给对应owner_cursor，
第一条事件前断线也可resume。Control sequence限定JS safe integer，owner uint64是十进制字符串，
用bigint/字符串处理，不转Number。收到重复事件去重，跳序/边界错进入恢复，不能把服务端head当作
自己已处理。未知事件/字段允许前向兼容。流会话与token仅在内存，token轮换保留同Identity session
的cursor，用新access token重连认证；旧socket或旧scope回调不能推进新会话。

先建立流再拉权威REST；快照加载期间有事件则记dirty，加载完成再刷受影响资源，防止旧请求覆盖
新视图。operation.updated触发owner GetOperation，并保持200/202仅受理或读取成功、
BLOCKED/UNKNOWN不等于完成；断流不得重发管理写。RESYNC_REQUIRED携带CURSOR_EXPIRED、
INVALID_CURSOR或SERVER_RESTART时丢弃旧流、重新hello并全量刷新当前可见资源与operation tracker。
普通断线按退避resume；4401清会话回登录；4400/4403/1013停止自动重试并显示协议/权限/限流原因；
1011按有界退避恢复，不能显示正常空资源。WS不可用不阻挡独立授权REST，显示快照最后确认时间。

目标adapter/plugin分别消费TargetAdapterRecord/TargetInstanceRecord安全状态分支；显示online、
lifecycle、Runtime期望、Host执行结果与Plugin应用版本，不把Host离线/UNKNOWN映射为停止，
不把desired=RUNNING写成插件已运行。target task只消费安全元数据，消息只有received metadata，
不显示正文、模型输出或重建历史；bootstrap、token、cursor原文不进URL、存储或日志。

首批接operation/health/adapter/plugin，第二批task/log，第三批message metadata，Host operation
随独立Host owner接入。未完成批次明确不可用，不用reference事件白名单或成功空流代替。
`plugin.updated.health_state`仍待真实owner健康事实，不从配置ACK推导健康。

## 管理动作与错误状态（W4）

面板只接线契约已就绪的 REST 操作，所有授权、校验和持久化仍由服务端执行：

- **插件**（`panel/src/views/plugins.tsx`）：`POST /plugins/validate`（只校验不启用）、`POST /plugins/{id}/enable|disable`（202 表示已持久化接受，不代表进程已启动或副作用已撤回）、`GET|PUT /plugins/{id}/config`（写请求携带 `expected_version`，409 冲突后提示重新读取；`delivery` 显示持久化与运行实例应用的区别）、`GET /plugins/{id}/health`。
- **会话**（`panel/src/views/sessions.tsx`）：`GET /auth/sessions` + `POST /auth/sessions/{id}/revoke`；可见范围由服务端决定，跨所有者返回 NOT_FOUND，撤销后 REST 立即失效、WebSocket 最迟一秒关闭。
- **Action 查询**（`panel/src/views/actions.tsx`）：`GET /actions/{actionId}` 与 `GET /actions/by-idempotency-key` 在 `api/openapi.yaml` 中标记 `x-contract-status: not_implemented`，v1 对任何调用方返回 NOT_FOUND。面板显示"未找到（契约标记 not_implemented）"并说明 NOT_FOUND 不代表动作失败，不伪造结果；等 control-api 接线后即可复用同一 UI。
- **状态与错误**：每个操作有加载/成功/失败状态；错误按 HTTP 状态分类（401 回登录、403 权限不足、404 未找到/未实现、409 冲突、429 限流并显示 `Retry-After`、503 依赖不可用、504 超时、400 参数无效、其他 5xx），并展示 `status`/`code`/`request_id`。按钮按服务端 `permissions` 禁用，但隐藏/禁用不代替服务端鉴权。
- 渲染异常由 `ErrorBoundary` 兜底为可重试卡片；日志与配置值按文本渲染，不写 `localStorage`。

## 契约漂移检测（W4）

`panel/src/api/client.ts` 用 `operation('<path>', '<method>')` 声明端点，其类型参数是生成的 `paths` 键：契约中改名/删除操作会让 `npm run lint`（`tsc --noEmit`）失败。`npm run check:contracts` 再把同一张表与 `api/openapi.yaml` 逐项比对（当前 23 个端点），并校验 `control-stream.ts` 的 `KNOWN_EVENTS` 等于 AsyncAPI 中未标记 `x-contract-status: not_implemented` 的事件类型（当前 `log/adapter/plugin/task/message.updated` 5 个）。`health.updated` 在 v1 不产生，面板不会把它当作刷新信号；`message.updated` 已实现并作为刷新信号，但消息事件没有 REST 投影——面板只触发既有刷新，不新增消息 UI、不展示正文或历史。

## 本地运行和检查

`panel/` 使用 React、TypeScript 和 Vite。`npm ci` 根据锁文件安装依赖，并从 `api/openapi.yaml`、`api/asyncapi.yaml` 生成类型；生成目录、`node_modules/` 和 `dist/` 均不提交。默认 `npm run dev` 使用内存 mock API 和 mock WebSocket，输入任意非空用户名和密码可登录。仅在连接真实控制 API 时设置 `VITE_API_MODE=live`，可使用 `VITE_API_BASE` 和 `VITE_WS_URL` 指定端点；生产环境必须使用 HTTPS/WSS。

单机 reference MVP 可用 `python3 scripts/local/start.py start --ui` 启动 live 页面，详见
[单机运行说明](24-single-host-mvp.md)。原生 fetch 在注入 API 客户端时绑定浏览器全局上下文，
避免作为类成员调用时出现 Illegal invocation；HTTP 错误仍按现有契约展示，不切换为 mock。

access token、refresh token 和流会话仅保存在内存；到期前调用 refresh 接口轮换，刷新页面后需要重新登录。列表显示服务端快照，已知资源事件（log/adapter/plugin/task/message.updated；message 事件只触发既有刷新）与重连/重同步触发快照刷新；面板不计算权限。列表支持 `page_size`（20/50/100）与 `page_token` 翻页。检查命令（在 `panel/` 下执行）：`npm ci`、`npm run check:contracts`、`npm run lint`、`npm test`、`npm run build`。

`npm test` 包含 mock 集成测试与真实服务测试。真实服务测试需要 C++ 构建产物（默认 `build/proto`，可用 `KIARABOT_ACCEPTANCE_BUILD=build/full` 指向 full 预设）与 `control/api/tests/.venv`（`uv sync --project control/api/tests`）；缺少前置时该套件以原因信息跳过（`console.warn` 输出缺失项），不会让 mock 测试失败。

mock 覆盖：登录/刷新、`/auth/me`、会话列表与撤销、9 个资源列表（`/logs` 与 `/auth/sessions` 带 `next_page_token` 分页）、插件 validate/enable/disable/config GET/PUT/health、Action 查询的 not_implemented 404；并支持测试注入 429/5xx、4401/4403/4400/1013 关闭码和资源事件推送。

## 服务接入与验收边界

- T08b 已提供认证/RBAC、Group/User/LogEntry 与真实 REST 服务，响应由 OpenAPI schema 验证。
- T09a 已提供首帧认证、主体绑定、十分钟保留、重放及 RESYNC_REQUIRED；日志复用 REST LogEntry。
- T09b 增加 TypeScript 客户端对真实 Core 的登录、轮换、401/403、事件刷新和断线恢复测试；同时保留 mock UI 测试。
- 本机 HTTPS/WSS 通过临时可信证书的测试代理验证；生产代理配置、真实平台及部署恢复仍等待 T11b/T07b。

## 未来 C++ 控制面板

未来如果使用 C++ 编写桌面控制面板，它只需要复用：

- OpenAPI REST 接口；
- WebSocket 频道；
- 认证和错误模型。

因此 TypeScript 前端不能把关键业务逻辑写在组件内部。

## 验收标准

- 页面可以独立使用 mock API 开发；
- 插件启停、配置和日志页面不需要直接导入 Core 代码；
- 刷新页面后状态从 API 恢复；
- 断线时有明确状态提示和自动重连；
- 没有把密钥保存到 localStorage。

## 代码启动句

> 请只实现基于 `api/openapi.yaml` 的 TypeScript 控制面板骨架、认证、总览、适配器、插件、模型、权限和实时日志页面，使用 mock API 开发，不连接数据库、NapCat 或 Python 进程。

## W4 管理动作与契约漂移（2026-10-31）

- 状态：**实现完成；mock 测试、契约检查、lint 与 build 在本机实际运行**；真实服务测试需 C++ 构建与 `control/api/tests/.venv`，本机按文档前置执行（见执行记录）。远端 CI 未运行。
- 范围：仅 `panel/` 与 `doc/07-control-panel.md`、`doc/16` 台账；未修改 `core/`、`control/`、`proto/`、`api/`、`deploy/`。
- 未实现（仍为交接项）：`health.updated`、REST Action 查询生产者、`/groups` 数据源、插件配置 delivery 的实时推送（面板靠 GET 轮询观察）；messages 频道已由 W5 闭环（见末节）；面板不在前端伪造这些能力。
- 文档影响：页面集合从 9 页增加到 10 页（新增动作页）；管理动作、错误分类、契约漂移检测、live 测试前置、mock 覆盖均同步到本文件与台账。

## T09b 真实服务集成

保留 React/TypeScript/Vite 与生成契约类型。登录后读取 `/auth/me` 展示服务端角色/权限；导航按服务端权限展示，但授权仍由服务器执行。任一受保护 REST 请求的 401 或 WebSocket 4401 清空内存凭据并回到登录；403 保留登录并显示权限错误。刷新轮换后更新 API 和流使用的令牌；退出调用会话撤销。

真实默认流端点为 `/ws/control`。收到首个 SESSION_READY 后才装载 REST 视图；所有已知资源事件及重新连接触发刷新，异步加载旧响应被取消展示，分页刷新返回首个快照。RESYNC_REQUIRED 清除旧流凭据后重新 hello；未知事件只推进已处理序号。服务端返回的日志按文本渲染，不渲染 HTML、不写持久化浏览器存储。

验收增加使用真实 C++ Core 子进程的 TypeScript HTTP/WebSocket 集成；mock 保留独立 UI 测试。本机 HTTP/WS 及带证书验证的 HTTPS/WSS 测试代理验收，不替代生产代理部署和 T07b 真实平台验证。

## W5 messages 频道事件与面板边界（2026-10-31）

- `panel/src/stream/control-stream.ts` 的 `KNOWN_EVENTS` 增加 `message.updated`（当前 5 个：`log/adapter/plugin/task/message.updated`），由 `npm run check:contracts` 与 AsyncAPI 中未标 `x-contract-status: not_implemented` 的事件自动比对，不靠手工维护；`health.updated` 仍是唯一未实现的已知事件。
- `message.updated` 的 payload 只有 `message_id`、`conversation_id`、`direction`，v1 只产生 `received`，不含消息正文。频道没有 REST 消息投影，因此面板收到事件时只触发既有视图刷新（与 `log/adapter/plugin/task.updated` 相同的刷新路径），不新增消息页面、不缓存或重建消息历史，也不展示正文。
- 订阅要求每用户显式 `message.read`（admin/operator/viewer 默认都不持有）；未授予时服务端仍以 4403 关闭，面板按终止性失败提示并停止重连。
- 测试：`panel/tests/integration.test.tsx` 的已知事件刷新用例增加一次 mock `message.updated` 投递断言（被接受并触发刷新），并保留未知事件不触发的断言；未新增 UI。

## P12d Panel 真实接线（2026-11-02）

- 状态：**live 骨架接线完成；P12c Control 后端并行开发中，target 端点契约仍标 not_implemented，请求可能返回 404/501，Panel 显式显示「P12c 后端未实现」且不伪造成功**。仅改 `panel/` 与本文档；未改 `api/`、`proto/`、`control/`、`core/`。
- 范围：`panel/src/api/client.ts` 新增 P12 目标端点描述符与 live 方法（认证/公开探针分根路径、多占位符 URL、账号/安装/实例/operation/task/model/action metadata/action policy/bind-unbind/desired-state/config），`panel/src/views/p12-live.tsx`（7 页 live 实现），`panel/src/App.tsx`（默认 live；mock 模式保留全 mock 预览与「P12 预览」导航），`panel/src/mock/server.ts`（P12 端点 mock 响应），`panel/tests/p12-live.test.tsx`。
- 已接线：`/health/live`、`/health/ready`（公开根路径）、受权 `/api/v1/health` 聚合；账号列表；账号↔adapter 绑定/解绑；账号 ActionPolicy（群白名单/禁言踢出开关/每日预算，带 expected_version）；安装列表；实例配置读取与更新、desired-state（以实例为对象，不复用 `/plugins enable`）；operation 进度轮询（受理/执行/受阻/完成/失败/取消/过期 + participants/stalled/reason/next_attempt_at）；任务读取与取消；模型元数据（model.read）与完整只读配置（model.manage）；动作元数据（action.inspect + 显式账号 scope，NOT_FOUND 只显示未找到）。
- Q 口径：NOT_FOUND 不推断动作失败；日志/审计按 Q2 **阶段性延期**（本版不调用 `/logs`、`/audit`，总览显式标记）；密钥不回显（模型页只显示「已配置」且完整配置受 model.manage 门禁）；会话撤销沿用既有 SessionsPanel；未裁定/未接线的账号创建/停用、安装/实例创建、插件密钥轮换、ListPrincipals 主体目录显式保留待后续。
- WS/REST：沿用 `ControlStream`；WS 收到 RESYNC_REQUIRED/重连 SESSION_READY 触发 revision，live 页面从 REST 权威快照重新加载（有 mock 测试覆盖 resync 后重新请求 `/accounts`）。
- 检查（本机实际运行）：`npm test -- --run` **34 passed / 2 skipped（live.test.ts 真后端套件按缺构建产物跳过，预期）**；`npm run lint` 通过；`npm run check:contracts` 通过（45 个 panel 端点、5 个 known events，OpenAPI/AsyncAPI 漂移检查）；`npm run build` 通过。真实 P12c Control/REST/WS 未运行：P12c 并行开发，合并后需补真实 live 验收并翻转对应 target 状态。
- 与 P12a/P12c 差异：Panel 只消费已冻结的 P12a 路径/权限，不新增或改契约；后端 handler 未在 main，live 页以 404/501 显式降级。P12c 合并后如字段/权限有变，需同步 client 类型与页面映射。

## P12b Panel 管理面预研骨架（2026-11-02）

- 状态：**骨架/类型/mock 预览完成，未接真实后端**。本段只新增 Panel 侧 target 管理面预览；未修改 `api/`、`proto/`、`control/`、`core/`，未接 live 请求，未新增 UI 依赖。生产管理页接线等待 P12 与 Q1–Q8 决策。
- 范围：`panel/src/p12/catalog.ts`（页面/契约/依赖声明）、`panel/src/p12/fixtures.ts`（`Schemas['Target*']` 约束的 mock fixture）、`panel/src/views/p12-preview.tsx`（骨架渲染）、`panel/src/App.tsx`（独立「P12 预览（未接线）」导航分组）、`panel/src/api/client.ts`（target `operation()` 描述符与 target 类型别名）、`panel/scripts/check-contracts.mjs`（漂移检查扩展）、`panel/tests/p12-preview.test.tsx`。
- 页面：P12 预研总览（Q1–Q8 依赖矩阵）、账号、安装与实例、操作进度、任务（目标记录）、模型配置（只读）、健康与探针，共 7 页。每页展示 target 端点（method/path/operationId/权限/契约状态）、mock fixture 表格与所依赖决策；页面统一标记「P12 预研 · 未接线」。
- 契约与数据边界：所有端点经 `operation()` 声明并受生成 `paths` 类型与 `check:contracts` 双重校验；target 端点仍为 `x-contract-status: not_implemented`，脚本固定其状态防止误标。fixture 用 `Schemas['TargetAccountRecord']` 等 target 类型约束，仅 mock 模式渲染；live 模式显示「未接线」说明，不向 target 端点发请求。现网 reference 页面（总览/插件/权限/日志/审计/动作等）行为不变。
- 未实现（交接项）：账号详情/停用/绑定，安装与实例启停/配置交互，operation 轮询与进度推送，任务详情/取消，模型配置读取与密钥轮换，健康组件聚合，以及 Q1–Q8 相关的动作查询、审计/日志归属、群白名单、主体目录、权限词表、v1 plugins 迁移与 P12 完成度口径——全部等待用户决策后接线。
- Q1–Q8 依赖：Q1 动作查询授权；Q2 审计/日志查询归属；Q3 groups/群白名单语义；Q4 ListPrincipals 主体目录；Q5 健康聚合契约；Q6 v1 `/plugins*` 迁移；Q7 新权限词表与固定角色；Q8 P12 完成度口径。页面与决策的映射见 `panel/src/p12/catalog.ts` 的 `DECISIONS`，预览页直接展示。
- 检查（本机实际运行）：`npm test -- --run` 27 passed / 2 skipped；`npm run lint` 通过；`npm run check:contracts` 通过（34 个 panel 端点、5 个 known events，含 P12 端点状态/目录引用/fixture target 类型断言）；`npm run build` 通过。真实服务/真实 Host 未运行。

## P12d-2 Panel live 扩展（2026-11-02）

- 状态：**P12c-2/3/4 已合入的 owner 端点已在 Panel 接线；mock 测试、lint、build 与契约漂移检查在本机实际运行**。真实 Control/REST/WS 联调需 P12c 的 C++ 构建与测试 venv（本 worktree 未构建，`live.test.ts` 按既有条件跳过）。
- 范围：仅 `panel/` 与本文档（及新增 `panel/README.md`）；未改 `api/`、`proto/`、`control/`、`core/`。需要契约变化的事项登记在下方“需 P12a 契约点”，Panel 不自造端点。
- 已接线：
  - 主体目录：`GET /api/v1/principals` 人类主体分页（`principals/page/checked_at`），在“权限”页展示；401 由全局会话处理，403 显式权限不足，503 显示“数据源暂不可用”而非空目录。
  - 操作进度：`GET /api/v1/operations/{ownerService}/{operationId}?scope=...` 带 scope 轮询，展示 state/execution_outcome/participants/stalled/observed_at/reason/next_attempt_at；HTTP 200 只表示快照读取成功，**不表示完成**；`EXECUTION_UNKNOWN` 明确标注不得推断完成，轮询只在终态停止。
  - TaskPage：`/tasks` 列表按 `items/page` 分页并提供 `scope`；详情/取消也带 scope。取消返回“已受理”，不伪称任务已停止。
  - 账号：`/accounts` 列表支持显式 `catalog` 查询；适配器绑定/解绑使用绑定 `expected_version`，成功响应中的 `operation` 进入统一轮询组件；409 由既有 `describeError` 提示冲突。
  - 安装与实例：`/installations`（catalog + 分页）与安装/实例创建（`POST /installations`、`POST /installations/{id}/instances`）；实例 config 按 `config.instance/desired_version/values/applied_version/apply_state/delivery_id/fence/error/observed_at` 展示，版本按字符串显示；desired-state/config 更新带 `expected_version` 并把返回的 operation 交给轮询组件；创建响应不展示 bootstrap 密钥。
  - 模型：`/models` 列表 + `/models/{id}` 元数据 + `/models/{id}/configuration`（model.manage）；展示 `secret_configured` 与版本，密钥内容和版本号不回显；更新使用 `expected_version` 并轮询 operation。
  - 健康：`/health/live`、`/health/ready` 探针与受权 `/api/v1/health` 四依赖（identity/runtime/scheduler/model）；HTTP 200+DEGRADED 显示“部分依赖降级”，Identity/凭证链异常 HTTP 503 显示“Identity 未就绪”，不把缺失组件或 503 展示为正常/空数据。
  - 日志/审计：权威源未接线返回 503 时显示“数据源暂不可用（503）”，不显示“暂无数据”。
  - 断流：WS 不可用（无服务端/断流）时允许装载 REST 快照并标注“非最新”；重连收到 SESSION_READY 后再次重拉快照。未知事件/旧流不用于推断完成。
- 未接线/保持待 P12a：
  - Model `DisableModel`/`EnableModel` 未在 OpenAPI 声明，Panel 不发明端点，只显示待契约说明。
  - 实例级插件 enable/config 路径未冻结（Q6），Panel 仅展示实例 config/desired-state，不复用 `/plugins enable`。
  - WS 目标/事件定义未新增；`health.updated` 仍为 not_implemented，健康仍靠受权 REST 聚合。
  - 模型 provider secret 轮换不进入本页提交（仅显示已配置/版本），需要受控轮换流程与审批口径。
- 需 P12a 契约点：
  1. `/tasks` 的 Control owner gateway 实际要求 `scope` query（见 control/api/tests/owner_gateway_test.cpp），但 `api/openapi.yaml` 的 `listTasks` 只声明 page 参数；Panel 已按 owner 语义附 scope，建议契约补参数。
  2. `/accounts`、`/installations` 列表要求 `catalog`（TargetCatalogRef）但没有 catalog 发现端点；Panel 只能手工输入 catalog ID，需要 P12a/Control 给出 catalog 选择来源。
  3. TaskPage/operation 的 200/202 “受理 ≠ 完成”已由各自 owner operation 表达；若后续增加统一 operation 推送事件，需要 AsyncAPI 目标/事件冻结。
- 检查（本机实际运行）：`npm test -- --run` **41 passed / 2 skipped**（live.test.ts 真后端套件按缺陷构建产物跳过）；`npm run lint` 通过；`npm run check:contracts` 通过（50 个 panel 端点、5 个 known events）；`npm run build` 通过。

## P12d-3 Panel 实例三类事实/health.read/TaskSummary/日志审计对齐（2026-11-02）

- 状态：**P12c-5 实例 plugin-state/configuration 端点已接线；P12e 尚未合入，logs/audit live 仍按 503 暂不可用渲染**。mock 测试、lint、build、check:contracts 本机实际运行；真实 Control/REST 需 C++ 构建与测试 venv，`live.test.ts` 按既有条件跳过。仅改 `panel/` 与本文档。
- 实例事实（`deploy/...` 不涉及）：新增
  `PUT /installations/{installationId}/instances/{instanceId}/plugin-state`、
  `GET/PUT /installations/{installationId}/instances/{instanceId}/configuration` 的 client 类型与方法；安装与实例页改为“安装+实例对象路径”，分别展示三类独立事实：
  - **runtime**：desired / desired_config_version / version / observed_at（期望，不代表 Host/插件已执行）；
  - **host**：host_id / registered / process_outcome / observed_at（`EXECUTION_UNKNOWN` 明确显示“结果未知（不等于已停止）”）；
  - **plugin**：apply_state / applied_config_version / observed_at；只有 `CONFIG_APPLY_APPLIED` 显示“已确认应用”，非 APPLIED 显示处理中/未确认/已拒绝/不可用，**不显示完成**。
  desired-state 与 configuration 更新均带 `expected_version`，返回的 operation 进入统一轮询组件；configuration 只读展示 runtime.configuration（服务端脱敏）与十进制版本字符串，秘密永不回显。旧 `/instances/{id}/config` 仅保留 client 兼容，UI 已迁移到新对象路径。
- 健康页：受权 `/api/v1/health` 401/403 显示“未授权/无权限”，不显示为 unhealthy/Identity 故障；匿名 `/health/live`、`/health/ready` 继续独立展示。HTTP 200+DEGRADED 与 HTTP 503 的 `CAPABILITY_NOT_SUPPORTED`（聚合未实现）分别渲染；`UPSTREAM_UNAVAILABLE` 503 才显示 Identity/依赖不可用。四依赖缺失显示“未上报”，不推断正常。
- Tasks：列表消费 TaskPage 的 `TaskSummary`（id/name/state/created_at），分页 token/scope 原样转发；详情/取消继续使用 owner 语义。operation 轮询沿用 P12c-4 统一形状（state/execution_outcome/participants/stalled/observed_at/reason/next_attempt_at），200 不等于完成、EXECUTION_UNKNOWN 不推断。
- 日志/审计：**P12e 未合入**，Control 对 `/logs`、`/audit` 返回 503 时显示“数据源暂不可用（503）/P12e 未合入”，不显示空数据；client 保持 `occurred_at + 稳定 ID` 的服务端排序语义与 opaque page_token，原样透传、不在前端重排/跨页合并。mock 提供日志分页与审计 503 可测路径。需 P12e 合入后补真实 live 验收并确认字段/顺序/401/403/503 稳定。
- 未接线/未冻结（显式标记）：Model Disable/Enable 无契约；旧 `/plugins/{id}/enable|config` 不复用（实例用 P12c-5 对象路径）；WS 目标/`health.updated` 事件未冻结；模型 secret 轮换不提交（仅已配置/版本）。
- 需 P12c/P12e 同步点：
  1. **P12c**：确认 `setInstancePluginState`/`get|updateInstanceConfiguration` 的 handler 状态与字段（三个 facts、operation、错误码）与 OpenAPI 一致；当前 contract 仍 `not_implemented`，handler 若返回 404/501，Panel 以“后端未实现”显式降级。
  2. **P12e**：`/logs`、`/audit` 权威源接线后，按 occurred_at+ID 的 opaque page token 分页；Panel 已预留透传与 503 语义，需 P12e 提供稳定字段（LogEntry/AuditEntry、occurred_at、id）并跑真实 live 验收。
  3. **P12a**：`/tasks` 的 scope 查询、catalog 发现来源仍未在契约中补齐（沿用 P12d-2 记录）。
- 检查（本机实际运行）：`npm test -- --run` **45 passed / 2 skipped**（live.test.ts 缺 C++ 构建/venv 跳过）；`npm run lint`、`npm run build` 通过；`npm run check:contracts` 通过（53 个 panel 端点、5 个 known events）。

## P12d-4 Panel /logs /audit live 与 P12c-6 分页/脱敏/权限验收（2026-11-02）

- 状态：**P12c-6 尚未合入**（Control owner gateway 仍对 `/api/v1/logs`、`/api/v1/audit` 返回 503；OpenAPI 仍 `x-contract-status: not_implemented`）。P12d-4 已完成前端类型/映射、独立分区页面、mock 测试；live 保持 503 显式降级，待 P12c-6 合入后补真实分页/过滤/owner 标注/401/403/503/400 验收。仅改 `panel/` 与本文档。
- 页面：新增 `panel/src/views/logs-audit.tsx`，App 将“日志”“审计”导航路由到该独立组件（不再复用通用 ResourceView）。两分区权限独立：日志仅 `log.read`，审计仅 `audit.read`，互不回退；页面文案显式说明。`/logs`、`/audit` 分别调用 `api.logPage`/`api.auditPage`，不存在混合请求。
- 分页/过滤映射（待 P12c-6 冻结确认）：显式发送 `page_token`/`page_size`、`occurred_after`/`occurred_before`、`actor`、`resource`、`owner`；opaque token 原样透传，不在前端按 occurred_at+ID 重排或跨页合并。mock 提供 occurred_at+owner+稳定 ID 行、时间窗/actor/resource/owner 过滤与 keyset 式下一页，覆盖映射行为。
- Owner 标注：服务端行若带 `owner_service`/`owner` 即展示（identity/runtime 等）；字段缺失时显式显示“未标注（待 P12c-6）”，不猜测 owner。P12c-6 需确认该字段名/形状并回填契约。
- 脱敏/错误：日志只显示 `message`/component/level/occurred_at（服务端已脱敏）；审计只显示 actor/action/resource/request_id/account/conversation；不显示凭据、消息正文、模型密钥或任何自由文本外的秘密。401 显示未授权（会话/权限问题），403 显示权限不足，400 显示请求参数无效，503 显示“数据源暂不可用（503）/P12c-6 未接线”且不显示空数据。
- 需 P12c-6 复验点：
  1. `/logs`、`/audit` 200 聚合真正接通 Identity/Runtime ListAudit/ListLogs；确认 `log.read` 与 `audit.read` 分别授权、不可互相替代；
  2. 确认分页/过滤 query 名称与语义（当前 Panel 映射为 `occurred_after|occurred_before|actor|resource|owner`；若 P12c-6 采用不同名字需同步 client/mock/tests）；
  3. 确认 owner 标注字段名（`owner_service` 优先）及响应行字段，Panel 当前兼容缺省并标注未标注；
  4. 复验 400（非法时间窗/参数）、401（未登录/会话失效）、403（缺权限）、503（owner 数据源不可用）稳定映射，并跑真实 Control 进程 live 验收（若环境可跑）或 p04 验收路径。
- 未接线/未冻结：P12c-6 聚合与真实 live；P12e owner RPC 已声明但 Control 未接线；日志/审计 filter 契约未冻结；`health.updated`/WS 目标仍延期。
- 检查（本机实际运行）：`npm test -- --run` **49 passed / 2 skipped**（live.test.ts 缺 C++/venv 跳过）；`npm run lint`、`npm run build` 通过；`npm run check:contracts` 通过（53 个 panel 端点、5 个 known events）。

## P12d-5 Panel /logs /audit 真实验收 + 生命周期 UI + P12 验收清单（2026-11-02）

- 状态：**P12c-6 已合入，Panel 按冻结契约修正字段并完成 p04 owner-gateway 验收；真实目标六服务 Control live 未运行（本机未启动目标栈），用 p04 路径替代并在此明确标注**。P12c-7 未合入，生命周期删除/恢复 UI 以 mock + 显式“待后端”实现；P12 首批验收项清单见下。
- **P12c-6 字段修正**（按 `control/api/src/target/owner_gateway.cpp` 实现）：
  - 过滤参数为 `occurred_from`、`occurred_until`（RFC3339）、`actor`、`resource`（JSON `TargetResourceRef`，如 `{"kind":"RESOURCE_TASK","id":"..."}`）；**没有 `owner` 过滤**（owner 是响应标注，不是过滤参数）。
  - 响应行 owner 字段为 `owner`（`identity`/`runtime`）；Panel 优先读 `owner`，兼容旧 `owner_service`，缺失显式“未标注（待 P12c-6）”。
  - 合并顺序 `occurred_at DESC, owner ASC, id DESC`，opaque keyset token 原样透传（不重排/跨页合并）；空页只在 200 成功且 items 为空时显示。
  - 400（非法时间/cursor/resource/actor）、401（未登录）、403（缺 log.read/audit.read）、503（任一 owner 源不可用，不返回部分页）分别显示未授权/权限/参数/数据源不可用；mock 对未知旧参数名严格返回 400 防回归。
  - logs/audit 分区独立：只走各自路径与权限，audit 不显示到 logs，反之亦然；脱敏字段仅 owner/occurred_at/level/component/message 或 audit 的 actor/action/resource/request_id/account/conversation/rejection。
- **真实/验收证据（本机实际运行）**：
  - `ctest -R p12_control_owner_gateway`（main build，P12c-6 C++ owner-gateway 验收；fake Identity/Runtime owner sources + delegation）**PASS 0.14s**：覆盖 200 跨源合并、owner 标注、occurred_at 排序、log.read/audit.read 独立、过滤转发、400、401、403、503、opaque keyset token。
  - `tests/live.test.ts` 对参考 C++ HTTP/WS（reference core）**2 passed**；该 fixture 不是目标六服务 Control，响应无 P12c-6 `owner` 字段，因此不作为 P12c-6 live 证据；本机未启动目标栈，**真实目标 Control live 验收未运行**（原因：需四库/PG/Identity/Runtime 全栈；本机当前只具备 reference fixture）。
  - Panel mock/unit：`npm test -- --run` **51 passed / 2 skipped**（live.test.ts 条件跳过）；`lint`、`build`、`check:contracts`（55 个 panel 端点/5 known events）通过。
- **生命周期删除/恢复 UI（P12c-7 未合入）**：新增 client 描述符/方法（`/resources/{resourceKind}/{resourceId}/delete|restore`）与安装实例页“资源删除与恢复”面板；mock 返回 operation，404/501 显示“P12c-7 后端未实现”。operation 进度展示 state/execution_outcome/participants/stalled/observed_at/reason/next_attempt_at；**BLOCKED 显式标注为等待依赖/参与者，不是失败**；只有终态（SUCCEEDED/FAILED/CANCELLED/EXPIRED）才显示完成/失败；提示删除后 **30 天**恢复窗（P13a）与 409 CONFLICT 需重读版本；HTTP 200/202 仅受理，不显示删除完成。
- **P12c-7 复验点**：确认 `/resources/{kind}/{id}/delete|restore` handler 状态/请求字段（resource、expected_version）与 200/202/operation 形状；确认 BLOCKED/reason/participants、30 天恢复窗与 409 映射；确认 operation owner/终态语义；合入后移除 mock 待后端提示并补真实 live/UI 验收。
- **P12 首批验收清单（Q8）**：

| 验收项 | Panel 状态 | 后端/live 状态 |
|---|---|---|
| 会话撤销 | 已接线 | 参考 live 通过；目标 Control live 未跑 |
| 账号列表/Adapter 绑定解绑 | 已接线（catalog/expected_version/operation） | target owner 未跑真实栈 |
| 安装/实例创建与启停配置 | 已接线（P12c-5 三类事实） | target owner 未跑真实栈 |
| 插件配置 | 既有 reference 页面通过 live；实例配置走 P12c-5 | target live 未跑 |
| Action 元数据 | 已接线（action.inspect） | target live 未跑 |
| 任务查询/取消 | 已接线（TaskSummary/owner scope） | target live 未跑 |
| 模型配置/更新/密钥 | 已接线读/更新；secret 仅显示已配置/版本 | target live 未跑；Disable/Enable 无契约 |
| 群白名单/ActionPolicy | 已接线（group whitelist/开关/预算） | target live 未跑 |
| 主体目录 principals | 已接线（分页/503） | target live 未跑 |
| 健康四依赖 | 已接线（DEGRADED/401/503 语义） | target live 未跑 |
| 日志/审计 | P12c-6 字段已修正；分区/过滤/分页/错误语义 | p04 owner-gateway PASS；目标 Control live 未跑 |
| 生命周期删除/恢复 | mock + 显式待 P12c-7 | **未接线** |
| 模型 Disable/Enable | 未接线（无契约） | 阻塞 P12a |
| 旧 /plugins enable/config、WS 目标/health.updated | 不复用/延期 | P12a/后续 |

- 未接线清单：真实目标六服务 Control live 验收（本机无目标栈）；P12c-7 生命周期 REST；模型 Disable/Enable；旧 `/plugins` 启停/配置不复用；WS 目标/`health.updated` 延期；模型密钥轮换提交（仅标记已配置/版本）。

## P12f 验收更新（2026-11-02，覆盖上文旧 Q8 状态）

真实 Control 进程的 P12f 矩阵（真 TLS + fake owners）显示：Control 侧首批写/读端点已可成功返回，
Panel 侧仍只到“已接线路由+mock transport 单测”，尚未做真实浏览器 HTTP live。逐项最新状态以
[P12f 报告](../reports/modules/architecture-control-panel.md) 为准：
- 已实测（Control+fake owner）：会话撤销、绑定/解绑、安装/实例/plugin-state/config、action-policy、
  任务查询/取消、模型全管理（Disable/Enable 已有 REST）、principals、logs/audit、健康、operation、
  delete/restore 202+operation。
- 仅路径/未接线：`listActionMetadata`（契约 not_implemented）；operation host owner（未声明）；旧
  `/plugins/{id}/*` 不复用；WS logs/health.updated 延期。
- 未验收：真实目标栈（六服务/PG16.4）、Panel 浏览器真实 HTTP live、真实 QQ/模型 Provider、Host 真实
  participant 停止/插件 ACK。
- Q8 中“模型 Disable/Enable 无契约”在 main@3ac3761 已不成立：REST 已由 P11d 接线并有真 TLS 200 证据。

## P12j Panel API client 对原生目标栈 live 验证（2026-11-03，fc454bc）

- 复用 P14f/P14i 原生单机栈（真实五进程 + PG16.4 + 临时 PKI + Control HTTPS），Panel client HTTP 矩阵严格模式 PASS。
- 真实结果：登录/refresh 200；`/health/live|ready` 200 HEALTHY；受权 `/api/v1/health` **200 HEALTHY**（四个真实 owner 组件）；未认证/bad token 401；非法 filter 400。
- 已知 gap：principals 503 CAPABILITY_NOT_SUPPORTED（P12l ListPrincipals 待实现）；logs/audit/tasks/models/action/delete-restore 仍为 stable known-gap（P14j 对齐 scope/参数中），严格模式按 Lead 口径记录为 known-gap，不伪造成功；依赖故障 503。
- 复现命令/CTest `p12j_panel_live`、完整矩阵见 [P12j 报告](../reports/modules/architecture-p12j-panel-live.md)。pre-fix 4e88fb4 全部 owner 400 的对照也保留在报告。
