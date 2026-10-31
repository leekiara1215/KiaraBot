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

## 实时状态

服务端以 `SESSION_READY` 返回会话 ID 和确认序号；客户端断线后按退避重连并以 `resume` 发送 access token、会话 ID 和最后处理的序号。收到 `RESYNC_REQUIRED` 时清除旧会话，以 `hello` 建立新会话并重新拉取 REST 快照，重连状态显示"正在重新同步"而不是误导性的"已断开"。`4403`/`4400`/`1013` 是终止性失败，面板停止重连并显示精确原因；`4401` 回到登录。日志事件使用 `log.updated` 和 OpenAPI `LogEntry`。客户端允许未知事件和未知字段，权限页面展示服务端计算的有效权限。

## 管理动作与错误状态（W4）

面板只接线契约已就绪的 REST 操作，所有授权、校验和持久化仍由服务端执行：

- **插件**（`panel/src/views/plugins.tsx`）：`POST /plugins/validate`（只校验不启用）、`POST /plugins/{id}/enable|disable`（202 表示已持久化接受，不代表进程已启动或副作用已撤回）、`GET|PUT /plugins/{id}/config`（写请求携带 `expected_version`，409 冲突后提示重新读取；`delivery` 显示持久化与运行实例应用的区别）、`GET /plugins/{id}/health`。
- **会话**（`panel/src/views/sessions.tsx`）：`GET /auth/sessions` + `POST /auth/sessions/{id}/revoke`；可见范围由服务端决定，跨所有者返回 NOT_FOUND，撤销后 REST 立即失效、WebSocket 最迟一秒关闭。
- **Action 查询**（`panel/src/views/actions.tsx`）：`GET /actions/{actionId}` 与 `GET /actions/by-idempotency-key` 在 `api/openapi.yaml` 中标记 `x-contract-status: not_implemented`，v1 对任何调用方返回 NOT_FOUND。面板显示"未找到（契约标记 not_implemented）"并说明 NOT_FOUND 不代表动作失败，不伪造结果；等 control-api 接线后即可复用同一 UI。
- **状态与错误**：每个操作有加载/成功/失败状态；错误按 HTTP 状态分类（401 回登录、403 权限不足、404 未找到/未实现、409 冲突、429 限流并显示 `Retry-After`、503 依赖不可用、504 超时、400 参数无效、其他 5xx），并展示 `status`/`code`/`request_id`。按钮按服务端 `permissions` 禁用，但隐藏/禁用不代替服务端鉴权。
- 渲染异常由 `ErrorBoundary` 兜底为可重试卡片；日志与配置值按文本渲染，不写 `localStorage`。

## 契约漂移检测（W4）

`panel/src/api/client.ts` 用 `operation('<path>', '<method>')` 声明端点，其类型参数是生成的 `paths` 键：契约中改名/删除操作会让 `npm run lint`（`tsc --noEmit`）失败。`npm run check:contracts` 再把同一张表与 `api/openapi.yaml` 逐项比对（当前 23 个端点），并校验 `control-stream.ts` 的 `KNOWN_EVENTS` 等于 AsyncAPI 中未标记 `x-contract-status: not_implemented` 的事件类型（当前 `log/adapter/plugin/task.updated` 4 个）。`health.updated` 与 `message.updated` 在 v1 不产生，面板不会把它们当作刷新信号。

## 本地运行和检查

`panel/` 使用 React、TypeScript 和 Vite。`npm ci` 根据锁文件安装依赖，并从 `api/openapi.yaml`、`api/asyncapi.yaml` 生成类型；生成目录、`node_modules/` 和 `dist/` 均不提交。默认 `npm run dev` 使用内存 mock API 和 mock WebSocket，输入任意非空用户名和密码可登录。仅在连接真实控制 API 时设置 `VITE_API_MODE=live`，可使用 `VITE_API_BASE` 和 `VITE_WS_URL` 指定端点；生产环境必须使用 HTTPS/WSS。

access token、refresh token 和流会话仅保存在内存；到期前调用 refresh 接口轮换，刷新页面后需要重新登录。列表显示服务端快照，已知资源事件（log/adapter/plugin/task.updated）与重连/重同步触发快照刷新；面板不计算权限。列表支持 `page_size`（20/50/100）与 `page_token` 翻页。检查命令（在 `panel/` 下执行）：`npm ci`、`npm run check:contracts`、`npm run lint`、`npm test`、`npm run build`。

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
- 未实现（仍为交接项）：messages 频道/`health.updated`、REST Action 查询生产者、`/groups` 数据源、插件配置 delivery 的实时推送（面板靠 GET 轮询观察）；面板不在前端伪造这些能力。
- 文档影响：页面集合从 9 页增加到 10 页（新增动作页）；管理动作、错误分类、契约漂移检测、live 测试前置、mock 覆盖均同步到本文件与台账。

## T09b 真实服务集成

保留 React/TypeScript/Vite 与生成契约类型。登录后读取 `/auth/me` 展示服务端角色/权限；导航按服务端权限展示，但授权仍由服务器执行。任一受保护 REST 请求的 401 或 WebSocket 4401 清空内存凭据并回到登录；403 保留登录并显示权限错误。刷新轮换后更新 API 和流使用的令牌；退出调用会话撤销。

真实默认流端点为 `/ws/control`。收到首个 SESSION_READY 后才装载 REST 视图；所有已知资源事件及重新连接触发刷新，异步加载旧响应被取消展示，分页刷新返回首个快照。RESYNC_REQUIRED 清除旧流凭据后重新 hello；未知事件只推进已处理序号。服务端返回的日志按文本渲染，不渲染 HTML、不写持久化浏览器存储。

验收增加使用真实 C++ Core 子进程的 TypeScript HTTP/WebSocket 集成；mock 保留独立 UI 测试。本机 HTTP/WS 及带证书验证的 HTTPS/WSS 测试代理验收，不替代生产代理部署和 T07b 真实平台验证。
