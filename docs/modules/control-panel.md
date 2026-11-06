# Control Panel 模块设计（panel）

> TypeScript + React + Vite 控制面板，是 Control API 的浏览器客户端，不实现业务规则。
> 契约唯一来源为 [`api/openapi.yaml`](../../api/openapi.yaml) 与
> [`api/asyncapi.yaml`](../../api/asyncapi.yaml)；生成类型只由 `npm run generate` 产出且不入库。
> 架构总览见 [`docs/architecture.md`](../architecture.md)，部署形态见 `deploy/docker/panel.Dockerfile`
> 与 `deploy/docker/panel-nginx.conf.template`。

## 1. 模块定位与职责

- 展示服务端资源与状态，提交用户操作；所有数据经 Control REST / WSS 获取，前端状态只做缓存与展示。
- 认证生命周期：登录、到期前 refresh 轮换、401 回登录、403 保留登录并提示、退出时撤销会话。
- 权限展示：以服务端 `/auth/me` 返回的 `permissions` 过滤导航与按钮；隐藏按钮不代替服务端鉴权。
- 页面：reference 资源页（总览/机器人/适配器/插件/模型/权限/任务/日志/审计/动作）与 P12 目标管理页
  （账号、安装与实例、操作进度、任务、模型配置、健康、预研总览）。
- 不负责：数据库、Core/适配器内部状态、权限计算、消息路由、模型调用、密钥回显或持久化。
- 安全底线：access/refresh token 与流会话仅存内存，不写 `localStorage`/cookie；秘密只显示“已配置”
  与版本，不显示内容。

## 2. 边界与依赖

- 契约：`api/openapi.yaml`（REST/错误/分页/权限）、`api/asyncapi.yaml`（WS 首帧/envelope/close
  code/limit）；`panel/src/api/client.ts` 的 `operation()` 描述符与 `src/stream/target-protocol.ts`
  的冻结常量均受这两个文件校验。
- 生成：`openapi-typescript` 生成 `src/generated/openapi.d.ts`，`@asyncapi/modelina` 经
  `scripts/generate-asyncapi.mjs` 生成 `src/generated/asyncapi.ts`；二者在 `.gitignore` 中，不提交。
- 运行时依赖仅 `react`/`react-dom`；开发/测试依赖见 `panel/package.json`（Vite 5、Vitest 2、
  Testing Library、jsdom、js-yaml、`@asyncapi/parser`）。
- 不导入 `core/`、`infrastructure/`、适配器或数据库代码；与后端只通过 HTTP/WS 契约通信。
- 部署：静态构建后由 nginx 托管，`/api/`、`/ws/`、`/health/ready` 反代到同机 Control；
  生产同源 HTTPS/WSS 由部署层终止。

## 3. 进程/组件结构

- 入口：`src/main.tsx` 创建 `ApiClient`、`MockServer`/`WebSocket` 工厂与 `ErrorBoundary`，
  按 `VITE_API_MODE !== 'live'` 选择 mock 或 live；默认 REST base `/api/v1`、WS `/ws/control`。
- 应用外壳：`src/App.tsx` 管理登录、principal、导航、分页 revision、reference 流与页面路由；
  页面级错误用 `ErrorBoundary`/`FailureNotice` 呈现。
- REST 客户端：`src/api/client.ts` 声明端点描述符、`Idempotency-Key`、`Authorization: Bearer`、
  401 竞态保护、`ApiError`（status/code/request_id/retry_after）与 JSON query 编码（`scope`/`catalog`）。
- reference 流：`src/stream/control-stream.ts` 连接 `/ws/control`，hello/resume，`KNOWN_EVENTS`
  五个事件触发刷新，4401/4403/4400/1013 终止重试，其余断线 1s→16s 退避。
- target 流：`src/stream/target-stream.ts` 每 socket 一个 channel/owner/精确 scope，hello/resume，
  首帧 5s、`owner_cursor`（BigInt 字符串）与 Control `sequence` 分离，旧 socket 消息被丢弃；
  `target-protocol.ts` 提供帧编解码、scope 规范化、close code/resync 常量与 URL 构造。
- REST 快照同步：`src/stream/snapshot-sync.ts` 以 generation/dirty 合并快照读取，恢复时丢弃过期响应。
- 视图：`src/views/p12-live.tsx`（P12 管理页与 `OperationTracker`）、`p12-preview.tsx`（P12 预研骨架）、
  `plugins.tsx`、`sessions.tsx`、`actions.tsx`、`logs-audit.tsx`；共享组件在 `src/ui.tsx`。
- Mock：`src/mock/server.ts` 提供 REST 路由、分页/错误注入、`MockSocket` 与资源事件；
  `src/p12/{catalog,fixtures}.ts` 提供预研页面/契约状态/fixture。

## 4. 数据与状态

- 会话：access/refresh token 及 principal 存于 React 状态与 `ApiClient` 内存；刷新页面需重新登录。
- 刷新：在 `expires_in - 60s` 定时调用 `/auth/refresh`，成功后更新 API 与流 token provider，
  流通过 `reauthenticate()` 以同一 Identity session resume，保留 sequence/owner_cursor。
- Reference 流状态：`sessionId`/`sequence`/连接状态；`SESSION_READY.last_sequence` 是已处理边界，
  不是服务端未送达 head；resync 清 session 后重新 hello。
- Target 流状态：`session_id`、Control `sequence`（JS safe integer）、`owner_cursor`
  （`source_service/epoch/scope/sequence`，sequence 用 BigInt 比较）；scope/channel 切换会销毁旧会话。
- 快照状态：`SnapshotSync` 的 generation/inFlight/dirty/appliedAt；恢复与跳序提升 generation。
- 页面状态：列表页保存当前 `page_token`/`page_size`（20/50/100）、刷新 revision；操作页保存表单、
  `expected_version`、最近 operation 引用与成功/失败提示。
- 不持久化任何凭据、游标或秘密；日志/消息只按服务端脱敏内容以文本节点渲染，不写 HTML。

## 5. 关键流程/状态机

- 登录：`POST /auth/login` → 存 token → `GET /auth/me` → 按 `permissions` 渲染导航 → 启动 reference
  流并加载当前页；登录失败显示服务端稳定错误。
- 刷新/失效：到期前 refresh；REST 401 且请求仍属当前 token 时清会话回登录（旧请求 401 不杀新会话）；
  WS 4401 同样回登录；403 保留登录并显示权限不足。
- 页面数据：进入页面/`revision` 变化时按端点加载；列表使用 `page_token` 翻页，刷新回到第一页；
  写操作携带 `Idempotency-Key`，读操作不携带。
- Reference 流：连上后发 hello/resume → `SESSION_READY` → 触发一次快照刷新；已知事件只触发刷新；
  未知事件/字段只推进序号；`RESYNC_REQUIRED` 清 session、重新 hello 并全量刷新。
- Target `OperationTracker`：写操作返回 `operation` 引用后，先触发 owner `GetOperation` 权威快照读取，
  再建立 `/ws/operations?owner_service=...`；`operation.updated` 只触发复核，200/202 只表示读取/受理，
  `BLOCKED`/`UNKNOWN` 保持未知；流不可用时按 10s REST 快照回退，断流不重发管理写。
- Target 流恢复：`SESSION_READY` 先记 cursor 再拉 REST；读取期间事件标 dirty 并在完成后刷新；跳序/
  畸形帧/owner cursor 不匹配/RESYNC 提升 generation 并重新 hello；恢复尝试超过 3 次以 `protocol` 终止。
- P12 管理页：账号绑定/解绑、ActionPolicy、安装/实例创建与三类事实（runtime/host/plugin）、
  实例 plugin-state/config、任务查询/取消、模型元数据/配置/更新、生命周期删除/恢复、主体目录。
- Mock 模式：P12 页面渲染预研骨架并展示“未接线”，不向 target 端点发请求；reference 页由 mock REST/
  socket 驱动，供无后端开发与 UI 测试。

## 6. 接口与契约

- REST 端点表：`src/api/client.ts` 的 `endpoints` 覆盖 55 个 operation（auth/health/列表/插件管理/
  会话/Action/P12 target 端点），每个用 `operation('<path>','<method>')` 绑定生成 `paths` 类型。
- 分页/过滤：`page_size`/`page_token` 原样透传；`/logs`、`/audit` 使用 `occurred_from`、
  `occurred_until`、`actor`、`resource`（JSON `TargetResourceRef`）过滤，按服务端 `occurred_at+ID`
  顺序消费 opaque token，前端不重排、不跨页合并。
- 幂等/版本：非 GET 请求自动生成 `Idempotency-Key`（`crypto.randomUUID()`）；带版本写请求发送
  `expected_version`（十进制 uint64 字符串）；202/204 按空响应处理。
- WS reference：`/ws/control` 的 hello/resume、`SESSION_READY`、`RESYNC_REQUIRED` 与事件 envelope
  来自 AsyncAPI；`health.updated` 不作为刷新信号。
- WS target：`targetChannelUrl()` 由默认 `/ws/control` 派生 `/ws/operations?owner_service=...`；
  首帧携带 access_token/scope（resume 另带 session_id/last_sequence，operations 带 owner_cursor），
  token 从不进 URL；close code 4400/4401/4403/1011/1013/1001，RESYNC 理由三值。
- 契约漂移检查：`scripts/check-contracts.mjs` 校验 AsyncAPI 可解析、SessionReady/Resume 必填字段、
  面板端点是否存在于 OpenAPI、P12 预览端点契约状态、catalog/fixture 引用、`KNOWN_EVENTS` 与
  implemented 事件一致、target 冻结常量与 AsyncAPI 一致，并用 `openapi-typescript` 验证可生成。
- 生成物与类型：`client.ts` 只从生成物读 schema 类型；`npm run lint`（`tsc --noEmit`）在端点改名时
  因 `paths` 键失配而失败。

## 7. 权限与安全

- 服务端授权：导航/按钮按 `/auth/me` 的 `permissions` 显示，但每个操作仍由 Control 逐请求校验；
  面板不计算角色、不拼接 scope、不把 display permission 当授权。
- 凭据：token 只在内存与请求头；不写持久化存储/cookie/URL；日志与错误提示不打印 token。
- WS：仅首帧提交 token（不放在自定义头或 URL）；浏览器 Origin 由 Control allowlist 校验；
  4401 清会话、4403/4400/1013 停止自动重试并显示原因。
- 渲染安全：React 文本节点渲染服务端日志/配置，不注入 HTML；`JsonPreview` 只作 `<pre>` 文本；
  秘密字段只显示“已配置/版本”，不提交 provider secret 明文。
- 权限页/日志审计页：`log.read` 与 `audit.read` 分开，页面不互相回退；缺权限显示 403，不显示空数据。
- 前端边界：mock 模式与 live 模式共用同一 `ApiClient`，mock 不可绕过 live 鉴权逻辑；无 `dangerouslySetInnerHTML`。

## 8. 错误/失败/恢复语义

- 错误分类：`describeError` 将 401 映射为会话失效、403 权限不足、404 未找到（含 not_implemented）、
  409 冲突、429 限流（显示 `Retry-After`）、400 参数无效、503 依赖不可用、504 超时，其他为服务端错误；
  `FailureNotice` 展示 status/code/request_id。
- 数据源不可用：`/logs`、`/audit`、`/principals`、P12 owner 路由 503 时显示“数据源暂不可用”，
  不渲染为空列表；NOT_FOUND 不推断动作/任务执行失败。
- 流失败：reference 4401/4403/4400/1013 终止并给出原因，其他按 1s→16s 退避；target 4401 回登录、
  4403/4400/1013 停止重试、1011/1000 退避 resume，恢复失败超过上限以 `protocol` 停止。
- 快照恢复：resync/跳序/首次 SESSION_READY 触发权威 REST 重拉；流不可用时页面继续展示 REST 快照并
  标注“实时不可用 + 最后确认时间”，不阻塞独立授权的 REST 视图。
- 渲染失败：`ErrorBoundary` 显示可重试卡片，不白屏；单个页面加载失败保留其他页面可用。
- 登录失效/退出：401/4401 清空内存会话；退出先 `POST /auth/sessions/{id}/revoke` 再清本地状态。

## 9. 可观测性与运维

- 构建/运行：`npm ci`（`postinstall` 生成类型）、`npm run dev`（mock 或 live）、`npm run build`
  （Vite 静态产物）；live 需 `VITE_API_MODE=live`，可选 `VITE_API_BASE`、`VITE_WS_URL`。
- 检查命令：`npm run generate`、`npm run check:contracts`、`npm run lint`、`npm test -- --run`、
  `npm run build`；`panel/README.md` 记录前置条件。
- 测试环境：Vitest + jsdom（`panel/vite.config.ts`、`tests/setup.ts`）；`live.test.ts` 用
  `// @vitest-environment node`，需要 C++ 构建与 `control/api/tests/.venv`，缺失时按原因跳过。
- 部署：`deploy/docker/panel.Dockerfile` 用 Node 22.14 构建、nginx-unprivileged 1.27 托管；
  `/health/live` 静态 200，`/health/ready` 反代 Control readiness；`deploy/compose.core.yml` 中
  panel 以 101:101 非 root、只读根文件系统运行，暴露 8082 并依赖 control healthy。
- 运行时无可观测指标/日志采集，排障依赖浏览器控制台与 Control 日志/审计；nginx 只做代理与探针转发。

## 10. 测试与验收证据

- Mock/UI 集成：`tests/integration.test.tsx`（登录/刷新/分页/插件管理/会话撤销/Action 404/事件刷新/
  4401、4403、4400、1013/错误分类/401 竞态/ErrorBoundary）。
- 流与恢复：`tests/target-stream.test.ts`（owner/scope/token 不进 URL、cursor/sequence 分离、resume/
  replay/跳序/resync/首帧超时/失败关闭）、`tests/snapshot-sync.test.ts`（dirty/generation/过期快照）、
  `tests/app-recovery.test.tsx`（REST 先于 SESSION_READY、token 轮换 resume）、
  `tests/p12-live-recovery.test.tsx`（OperationTracker 复核/1013/resync/慢回退）。
- 页面：`tests/p12-live.test.tsx`（账号/安装实例三类事实/operation 非完成/模型权限/主体 503/健康
  DEGRADED 与 503/删除恢复）、`tests/p12-preview.test.tsx`（仅 mock 预研、权限过滤、target 字段约束）、
  `tests/logs-audit.test.tsx`（owner 标注/过滤/opaque 分页/503 与 403/400）。
- 真实服务：`tests/live.test.ts` 用 `live_fixture.py` 启动真实 C++ reference HTTP/WS，验证登录/轮换/
  403/401、先建流后快照、事件刷新与断线 resume；缺构建产物或 venv 时整组跳过。
- 契约门禁：`scripts/check-contracts.mjs` 在 `npm test` 之外的独立脚本中执行；CI 定义于
  `.github/workflows/ci.yml`（protocol/contracts 与 python-panel job 均跑面板检查）。
- 验收边界：mock/vitest 不能替代真实 target WSS 全频道联调；真实 target 栈的面板验证以
  `tests/architecture/p14_native_single_host.py`（CTest `p12j_panel_live`）为准。

## 11. 已知限制与后续

- 目标 scoped 流目前只接线 `/ws/operations`；health/logs/adapters/plugins/tasks/messages 的 target
  频道尚未被页面消费，实时刷新仍主要依赖 reference `/ws/control` 与 REST 轮询。
- 无 catalog/scope 自动发现：P12 页要求操作者手工输入 resource scope JSON 与 catalog UUID；
  Control 尚无 catalog 选择端点，输入错误由服务端 400/403/404 拒绝。
- P12 预研页（`src/views/p12-preview.tsx` + `src/p12/catalog.ts`）在 mock 模式只展示契约骨架与
  Q1–Q8 依赖矩阵，不发起真实请求；不能作为功能已接线的证据。
- 页面能力仍偏只读：机器人/适配器/模型/任务等 reference 页只展示服务端 GET 结果；插件页提供
  validate/enable/disable/config/health，但旧 `/plugins/{id}/enable` 不映射到实例语义。
- `check:contracts` 只能发现端点缺失、已知 schema 字段变化与 target 冻结常量漂移，无法发现 HTTP
  方法语义变化、事件 payload 运行时形状或权限语义漂移；这类问题只会在 live/验收中暴露。
- live 套件依赖 C++ 构建产物与 `control/api/tests/.venv`，在纯前端环境会被跳过；CI 自动触发当前
  未启用（仅 `workflow_dispatch` 或 `CI_AUTO_ENABLED`），面板检查需手动运行。
- 无 i18n（文案硬编码 zh-CN）、无 CSP/安全头配置（由反代承担）、无前端指标；可访问性仅覆盖
  基础 keyboard/focus/role 属性，无自动化 a11y 测试。
- `crypto.randomUUID()` 在非安全上下文不可用；http 部署（非 localhost）时写请求会先抛错，生产必须
  HTTPS。
- 生成物不入库，新环境必须先 `npm ci`/`npm run generate` 才能通过 lint/build；契约文件变化需同步
  重新生成并运行 `check:contracts`。
