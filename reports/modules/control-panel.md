# 控制面板（kiarabot-control-panel）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/node/uv/protoc/docker）。`panel/package-lock.json`（7292 行）只核对依赖清单，未逐行阅读。

## 1. 范围与规模

- 目录/文件（全部被 git 跟踪的 `panel/` 文件，16 个；`src/generated/` 与 `node_modules/`、`dist/` 被忽略）：
  - 运行时代码 8 个：`src/App.tsx`(90)、`src/api/client.ts`(69)、`src/stream/control-stream.ts`(66)、`src/mock/server.ts`(54)、`src/main.tsx`(12)、`src/styles.css`(1 行压缩)、`src/vite-env.d.ts`(1)、`index.html`(1)
  - 脚本 2 个：`scripts/check-contracts.mjs`(25)、`scripts/generate-asyncapi.mjs`(10)
  - 测试 3 个：`tests/integration.test.tsx`(89)、`tests/live.test.ts`(82)、`tests/setup.ts`(4)
  - 配置 4 个：`package.json`(34)、`tsconfig.json`(1)、`vite.config.ts`(3)、`.gitignore`(4)
- 规模：约 **546 行**（不含 `package-lock.json` 7292 行）；其中源码 297 行、脚本 35 行、测试 175 行、配置 39 行。
- 关键入口与代码地图（file:line）：
  - 9 页导航定义 `src/App.tsx:6`、`src/App.tsx:9-13`；页面数据映射（全部只读）`src/App.tsx:28-42`
  - 登录/刷新/角色/流/退出装配 `src/App.tsx:46-74`；登录页 `src/App.tsx:76-79`；总览 `src/App.tsx:80-84`；资源列表 `src/App.tsx:85-90`
  - REST 客户端（手工路径 + 生成类型）`src/api/client.ts:22-68`；401 竞态保护 `src/api/client.ts:52-65`
  - WebSocket 会话/重连/重同步 `src/stream/control-stream.ts:13-65`
  - 内存 mock（REST + socket）`src/mock/server.ts:17-54`；mock/live 切换 `src/main.tsx:7-11`
  - 契约生成 `package.json:7,13` + `scripts/generate-asyncapi.mjs:1-10`；契约检查 `scripts/check-contracts.mjs:1-25`
- 对应设计文档：`doc/07-control-panel.md`（模块设计）、`doc/06-control-api.md`（API/WS 语义）、`doc/18-architecture-baseline.md:211-245`（第七层与“至少需要管理”清单，受保护只读）、`doc/16-development-tasks.md:305-319`（T09b 台账）、`README.md:97-105`（本地检查命令）、`api/openapi.yaml`、`api/asyncapi.yaml`（唯一契约）。

## 2. 模块职责（依据）

- 基线/doc 自述职责：控制面板是 API 的客户端，“负责展示状态和提交用户操作，不拥有任何核心业务规则”（`doc/07-control-panel.md:5`）；只通过 API 访问业务数据（`AGENTS.md` 架构边界；`doc/18-architecture-baseline.md:32`）。
- 基线要求的管理范围（受保护文档，只读引用）：机器人账号；NoneBot2 适配器连接；插件安装/启用/停用；插件配置；AI 模型和 API Key；群组白名单；用户和角色权限；会话和上下文；定时任务；日志和审计；运行状态和健康检查（`doc/18-architecture-baseline.md:234-245`）。`doc/18-architecture-baseline.md:182` 另明确“控制面板可以启用、禁用和配置插件”。
- 契约来源：REST 24 个路径 / 25 个 operation 见 `api/openapi.yaml`（`paths` 43-500；分页/无过滤/插件 202 与配置 expected_version 语义在 `x-control-policy` 13-41）；WebSocket `/ws/control` 与 6 个子频道、首帧 hello/resume、SESSION_READY、RESYNC_REQUIRED、close 码见 `api/asyncapi.yaml:18-29,35-139`。面板不导入任何 Core/适配器代码（全目录无此类 import，符合 `doc/07-control-panel.md:38`）。

## 3. 已实现能力（逐条带证据）

| # | 能力 | 证据（file:line） | 对应测试 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 登录（POST /auth/login），令牌仅存内存，不写 localStorage | `src/api/client.ts:30-34,58-60`；`src/App.tsx:47,71` | `tests/integration.test.tsx:29-45`（断言 `setItem` 未被调用） | 完整 |
| 2 | 到期前自动 refresh 轮换，并让后续 REST/WS 使用新 access token | `src/App.tsx:65-70`；`src/api/client.ts:35-40`；流随 tokens 重建 `src/App.tsx:60-64` | `tests/integration.test.tsx:19-28`；`tests/live.test.ts:49-51` | 完整 |
| 3 | 401 → 清内存凭据回登录（含“旧请求 401 不杀新会话”竞态） | `src/api/client.ts:52-65`（`requestToken === this.accessToken`）；`src/App.tsx:51-52` | `tests/integration.test.tsx:78-89`；`tests/live.test.ts:52-54` | 完整 |
| 4 | 403 保留登录并显示权限错误 | `src/App.tsx:44,74,82,89` | `tests/integration.test.tsx:46-52`；`tests/live.test.ts:47-48` | 完整 |
| 5 | 读取 /auth/me 展示服务端角色，导航按服务端 permissions 过滤（面板不计算权限） | `src/App.tsx:49,56,72-73`；服务端返回展开权限列表 `core/src/application/control_crypto.cpp:125-136`、`core/src/application/control_service.cpp:396` | `tests/live.test.ts:42-44` | 完整 |
| 6 | 退出登录调用会话撤销 `POST /auth/sessions/{id}/revoke` | `src/App.tsx:74`；`src/api/client.ts:27` | `tests/live.test.ts:52`（直接调 ApiClient） | 完整 |
| 7 | 9 个导航页全部实现为**只读列表**（总览/机器人/适配器/插件/模型/权限/任务/日志/审计） | `src/App.tsx:9-13,28-42,80-90`；client 方法 `src/api/client.ts:41-50` | `tests/integration.test.tsx:33-44`（逐页断言 API 路径与首行内容） | 完整（只读部分） |
| 8 | 权限页合并 `/users` 与 `/groups`（跨页聚合） | `src/App.tsx:37-40,18-26`（`allPages` 含重复 token 防护） | `tests/integration.test.tsx:43`（断言 /groups 被请求） | 完整 |
| 9 | 分页：page_token 下一页 / 返回首页；刷新回到第一页 | `src/App.tsx:87-89`；`src/api/client.ts:51` | 无（mock 无 next_token） | 部分 |
| 10 | WebSocket hello/resume：SESSION_READY 后连接成功，序列从 `last_sequence` 继续 | `src/stream/control-stream.ts:30,50-55` | `tests/integration.test.tsx:57-75`；`tests/live.test.ts:56-80` | 完整 |
| 11 | 断线指数退避重连（1s→16s 封顶），重连成功后触发快照刷新 | `src/stream/control-stream.ts:39-41,50-55` | `tests/integration.test.tsx:67-68,73` | 完整 |
| 12 | RESYNC_REQUIRED → 清 session/sequence → 重新 hello → 刷新快照 | `src/stream/control-stream.ts:56-59` | `tests/integration.test.tsx:69-73` | 完整 |
| 13 | 已知事件（health/log/adapter/plugin/task/message.updated）只触发刷新；未知事件/字段只推进序号 | `src/stream/control-stream.ts:60-64` | `tests/integration.test.tsx:65-66`；`tests/live.test.ts:69-73` | 完整 |
| 14 | WS 4401 回登录、4403 停止 | `src/stream/control-stream.ts:37-38` | 无（mock socket 不会发 close code） | 部分 |
| 15 | mock 模式（REST + mock socket），live 用 `VITE_API_MODE/VITE_API_BASE/VITE_WS_URL` 切换 | `src/main.tsx:7-11`；`src/mock/server.ts:17-38` | 全部 mock 测试依赖 | 完整 |
| 16 | 契约类型生成：openapi-typescript + Modelina，`postinstall` 自动生成；生成物不提交 | `package.json:7,13`；`scripts/generate-asyncapi.mjs:5-10`；`.gitignore:4`；`src/api/client.ts:1-14`；`src/stream/control-stream.ts:1` | `check:contracts` 内 `openapiTS` 生成 `scripts/check-contracts.mjs:24` | 完整 |
| 17 | 契约检查脚本（有限断言，见 §3.2） | `scripts/check-contracts.mjs:7-24` | CI 步骤 `.github/workflows/ci.yml:116` | 部分 |
| 18 | 文本渲染日志/消息（不注入 HTML） | `src/App.tsx:35-36,89`（React 文本节点） | `tests/live.test.ts:77-79`（日志不含 token/密码） | 完整 |
| 19 | 基础可访问性：nav aria-label、`role="status"/"alert"`、图标 `aria-hidden`、可见 focus 环、表单 label | `src/App.tsx:74,78,83`；`src/styles.css:1`（`button:focus-visible,input:focus-visible`） | 无自动化 | 部分 |

### 3.1 逐页实现程度（要求 1）

| 导航页 | 标签/图标 | 数据来源（API） | 展示列 | 管理动作 | 结论 |
| --- | --- | --- | --- | --- | --- |
| 总览 | 总览 ◫（`App.tsx:10`） | `GET /health`（`App.tsx:82`） | 健康状态文案 + 组件卡片（component/state/reason） | 仅“刷新”按钮（重取快照，`App.tsx:74`）；无重启/降级/依赖详情/告警操作 | 只读展示 |
| 机器人 | 机器人 ◉ | `GET /bots`（`App.tsx:30`） | 名称/标识/状态 | 无创建、编辑、删除、上下线、账号绑定 | 只读列表 |
| 适配器 | 适配器 ⌁ | `GET /adapters`（`App.tsx:31`） | 标识/实现/状态/能力 | 无连接/断开/重配/健康探测 | 只读列表 |
| 插件 | 插件 ◇ | `GET /plugins`（`App.tsx:32`） | 名称/版本/运行时/状态 | 无安装/校验/启用/停用/配置读取/配置写入/健康查询（API 均已提供，见 §4-G1） | 只读列表 |
| 模型 | 模型 ✦ | `GET /models`（`App.tsx:33`） | 名称/标识/状态 | 无提供商增删改、API Key 管理、连通性测试 | 只读列表 |
| 权限 | 权限 ♙ | `GET /users` + `GET /groups`（`App.tsx:37-40`） | 类型/名称/角色/有效权限/所属群组 | 无用户/群组创建、成员增删、角色或权限编辑、会话管理 | 只读聚合列表 |
| 任务 | 任务 ◷ | `GET /tasks`（`App.tsx:34`） | 名称/标识/状态 | 无创建/触发/启停/删除/查看执行历史（API 也只有 GET，见 §4-G2） | 只读列表 |
| 日志 | 日志 ≡ | `GET /logs`（`App.tsx:35`） | 时间/级别/组件/消息 | 无级别/组件/时间过滤、无搜索、无实时追加（log.updated 只触发整页重取）、无导出 | 只读列表 |
| 审计 | 审计 ▤ | `GET /audit`（`App.tsx:36`） | 时间/操作者/操作/资源 | 无过滤（时间/操作者/动作）、无导出 | 只读列表 |

补充事实：状态值只有总览的 `HealthState` 做了中文映射（`App.tsx:83`），其余页面把服务端枚举原样展示（如 `PLUGIN_STATE_RUNNING`、`SCHEDULED`、`INFO`，`App.tsx:30-36`），未按 `PluginState/HealthState` 本地化，也未对未知枚举做兜底。

### 3.2 `api/client.ts` 与 `api/openapi.yaml` 的契约同步机制（要求 2）

- 生成器：`openapi-typescript ../api/openapi.yaml -o src/generated/openapi.d.ts` + Modelina 生成 `src/generated/asyncapi.ts`，由 `postinstall` 在 `npm ci` 时自动执行（`package.json:7,13`）；生成目录被忽略（`.gitignore:4`），仓库不含生成物。
- 类型使用方式：`client.ts` 只从生成物取 `components.schemas` 与一个 `paths['/auth/login']['post']['requestBody']`（`client.ts:1-14`）——即 **schema 级类型**；所有 REST 端点路径是手写字符串（`client.ts:42-51`），并未从 `paths` 派生。
- `check:contracts` 实际检查（`scripts/check-contracts.mjs`）：
  1. 用 `@asyncapi/parser` 解析 `api/asyncapi.yaml`，断言无 severity=0 错误、文档存在（`7-11`）；
  2. 断言 `SessionReady.required` 恰为 `[type, session_id, last_sequence]`、`Resume.required` 恰为 `[type, access_token, session_id, last_sequence]`（`13-14`），并存在 `session-ready` message（`15`）；
  3. 用 `js-yaml` 直接加载 `api/openapi.yaml`（**不做 OpenAPI lint/结构校验**），断言 `Group/User/LogEntry` 的 `allOf[1].required` 含指定字段（`18-21`）、`LogPage.items.$ref` 指向 `LogEntry`（`22`）、`LogStreamEnvelope.allOf[1].properties.event_type.enum[0] === 'log.updated'`（`23`）；
  4. 最后 `await openapiTS(openapiPath)` 只验证“能生成”（`24`）。
- 能否发现漂移：
  - 能发现：上述 6 组字段/结构被删改、`openapi.yaml` 语法损坏到无法生成、AsyncAPI 解析失败。
  - **不能发现**：端点路径或 HTTP 方法改名（例如 `/bots`→`/bot`、`/plugins/{id}/enable` 语义变化），因为 `client.ts` 的路径是字符串字面量，`paths` 类型只在登录请求体处被使用；同理 panel 的“已知事件白名单”（`control-stream.ts:64`）与 `asyncapi.yaml` 的 6 个 `*.updated` 是否一致也没有检查。这类漂移只会在运行时 404/不刷新时暴露。
  - 生成物与 `client.ts` 之间的一致性仅靠 `npm run lint`（`tsc --noEmit`）间接保证（`package.json:10`），且只覆盖被引用的 schema 类型。

### 3.3 live vs mock 双模式（要求 3）

- mock 覆盖的接口（`src/mock/server.ts:25-36`）：`POST /auth/login`（空用户名/密码→401）、`POST /auth/refresh`（无条件成功并返回 rotated token）、`GET /auth/me`、`*/*/revoke`（204）、`GET /health`、`GET /bots|/adapters|/plugins|/models|/groups|/users|/tasks|/logs|/audit`；无 `Authorization`→401，`denied` 集合→403，其余→404。
- mock **未覆盖**：`/health/live`、`/health/ready`、`GET /auth/sessions`、`POST /plugins/validate`、`POST /plugins/{id}/enable|disable`、`GET|PUT /plugins/{id}/config`、`GET /plugins/{id}/health`、`GET /actions/{id}`、`GET /actions/by-idempotency-key`；也不模拟分页 `next_page_token`、429/500/503/504、会话/权限在流中失效。`MockSocket.send` 对任何首帧都回 `SESSION_READY`（`server.ts:48-51`），不校验 token、不推任何资源事件、从不发 4401/4403。
- live 切换：`VITE_API_MODE !== 'live'` 时用 mock（`main.tsx:7-11`），live 时 REST base 取 `VITE_API_BASE || '/api/v1'`，WS 默认 `ws(s)://location.host/ws/control`（`main.tsx:9,11`）。
- live 下的认证/刷新/401/403：与 mock 共用同一 `ApiClient`——access token 放 `Authorization: Bearer`（`client.ts:59`），非 GET 自动加 `Idempotency-Key`（`client.ts:58`，覆盖契约对 login/refresh/revoke 的要求 `openapi.yaml:537-542`），401 清会话回登录、403 保留并展示（`client.ts:61-65`、`App.tsx:44`），`204/202` 视为空响应（`client.ts:66`）。
- 分页：只透传 `page_token`，从不发送契约支持的 `page_size`（1-100，默认 20，`openapi.yaml:513-516`），因此默认 20 条/页且无法调整。
- 错误展示：仅 403 有定制中文提示并带错误码（`App.tsx:44`）；其余状态直接展示服务端 message 或 `HTTP <status>`（`client.ts:64`），`requestId` 未展示，429 的 `Retry-After`、503/504/TIMEOUT 无专门处理，refresh 失败直接登出（`App.tsx:68`）。
- live 安全边界：面板不做 HTTPS/WSS 强制或警告（`main.tsx:11` 只按页面协议推导 wss）；`crypto.randomUUID()`（`client.ts:58`）在非安全上下文（非 localhost 的 http）不可用，会先抛错而不是提示；生产 TLS 终止由反代负责（`doc/06-control-api.md:210`）。

### 3.4 `control-stream.ts`（要求 4）

- 连接：构造时注入 `factory(url)`（浏览器 `WebSocket` 或 mock，`App.tsx:62`）；`start()/stop()` 生命周期；`onopen` 发首帧：有 `sessionId` 发 `resume`（含 session_id + last_sequence），否则发 `hello`（`control-stream.ts:30`）。
- 重连：`onclose` 中 `4401` 停止并回登录、`4403` 停止（不重试），其余进入 `min(1000*2^retry,16000)` 退避重连（`37-41`）；`retry` 在 SESSION_READY 时清零（`52`）。
- resync：收到 `RESYNC_REQUIRED` 清 session/sequence、状态置 `resync`、主动关闭 socket，随后按上一条路径以 `hello` 重开（`56-59`）。
- 事件应用：严格校验 envelope（`schema_version/sequence/occurred_at/payload`），序列单调递增，只对 6 个已知 `*.updated` 调 `onRefresh()`（`60-64`）；未知 event_type 只推进序号（符合 `doc/06-control-api.md:127,129-136`）。
- 状态展示：`connecting/connected/disconnected/resync` 四态映射为中文（`App.tsx:14`），侧栏圆点 + 主区 `role="status"` 双处展示（`App.tsx:74`）；首次 SESSION_READY 前主区显示“正在建立会话…”（`App.tsx:74`）。
- 已知缺陷：`4403` 后状态被置为 `disconnected`，但 UI 文案固定为“连接已断开，正在重试”（`App.tsx:14`），实际不会重试且没有权限错误提示；`RESYNC_REQUIRED` 分支先置 `resync` 随即被 `onclose` 覆盖为 `disconnected`（`control-stream.ts:56-59` → `39`）；`streamReady` 只在会话失效/退出时复位（`App.tsx:51,74`），流断开后旧快照仍被当作已就绪展示；无客户端心跳/超时检测（依赖 TCP close）。

## 4. 缺口清单

| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |
| G1 | 面板无任何业务管理动作（创建/编辑/删除/启停/配置/触发/撤销他人会话），基线“至少需要管理”11 项无一项可操作；其中 7 个**契约已提供**的端点未被接线：插件 validate/enable/disable/config GET/PUT/health、`GET /auth/sessions`、`GET /actions/{id}`/`by-idempotency-key` | 未接线 | P1 | `src/api/client.ts:41-50`（只有 GET 列表）；`src/App.tsx:28-42`；`api/openapi.yaml:229-343,449-482,89-122`；`doc/18-architecture-baseline.md:234-245,182`；`doc/07-control-panel.md:98` | 基线要求的“可管理”控制面实际是只读看板；插件配置下发闭环（`doc/06-control-api.md:227-231`）在面板侧无入口 |
| G2 | 机器人账号、模型/API Key、群组白名单、用户/角色权限、任务触发、审计过滤导出在 **API 契约中也没有写入口**（`/bots`、`/models`、`/groups`、`/users`、`/tasks`、`/audit` 仅 GET） | 未实现（契约缺口） | P1 | `api/openapi.yaml:364-448,345-363`（均只有 get）；`doc/06-control-api.md:212`（首版无用户创建/插件安装 API）；`doc/18-architecture-baseline.md:234-245` | 面板无法补齐这些管理动作，需契约/控制 API 先行（待 contracts/control-api 确认） |
| G3 | 缺少 `会话和记忆`、`系统设置` 两个目标页面；插件页只有“市场/列表”没有“管理” | 未实现 | P1 | `doc/07-control-panel.md:44-57`（目标 12 项，实际 9 项）；`src/App.tsx:9-13` | 与模块文档自述的目标页面集合不一致 |
| G4 | 不消费 `/health/live`、`/health/ready`；无告警/阈值/通知；总览只有 `/health` 快照 | 未实现 | P2 | `src/api/client.ts:41`；`api/openapi.yaml:44-72`（根路径探针）；`doc/06-control-api.md:225` | 运维探针状态（进程存活 vs 就绪）在面板不可见；`DEGRADED/UNHEALTHY` 无主动提示 |
| G5 | 契约漂移检测不完整：路径/方法未从 `paths` 派生，`check:contracts` 只断言 6 组 schema 结构和“可生成”，不校验端点存在性、事件白名单、字段可空性 | 半实现 | P1 | `scripts/check-contracts.mjs:13-24`；`src/api/client.ts:42-51`；`src/stream/control-stream.ts:64` | 契约改路径/事件名时面板静态检查全绿、运行时 404 或静默不刷新 |
| G6 | 分页只有“下一页/回首页”，不发 `page_size`，无总页数/总条数；无任何过滤、搜索、排序 UI（契约 v1 明确不接受过滤参数，客户端筛选亦未实现） | 半实现 | P2 | `src/App.tsx:87-89`；`src/api/client.ts:51`；`api/openapi.yaml:32-38,513-520`；`doc/06-control-api.md:92` | 日志/审计/用户等大数据集只能逐页翻，无定位手段 |
| G7 | `npm test` 不能按文档独立运行：`live.test.ts` 硬编码 `../control/api/tests/.venv/bin/python` 并依赖 `build/proto/core/kiarabot-core`、`build/proto/control/api/kiarabot-control-credentials`；`doc/07`/README 的检查命令未提这些前置条件 | 未验证 / 文档漂移 | P1 | `tests/live.test.ts:19`；`control/api/tests/live_fixture.py:1-14`；`control/api/tests/conftest.py:17,28,49`；`doc/07-control-panel.md:76`；`README.md:99-105` | 新环境按文档执行 `npm test` 直接失败，面板验证实际依赖 C++ 构建与 uv 环境 |
| G8 | CI 中面板检查被放在 `contracts` job（而非 build-test），且 workflow 自动触发被注释、仅 `workflow_dispatch`；`build-test` job 完全不涉及面板 | 未接线 / 未验证 | P1 | `.github/workflows/ci.yml:3-17,26-47,110-119` | 面板 lint/测试/构建不随 PR 执行，回归靠人工手动 Run |
| G9 | WS 4403 后无权限错误提示，且状态文案“正在重试”与实际（已停止重连）不符；resync 状态被立即覆盖 | 契约不一致 | P2 | `src/stream/control-stream.ts:37-39,56-59`；`src/App.tsx:14` | 频道被拒/权限被收回时用户看到误导性提示 |
| G10 | `streamReady` 断线后不复位、重登录后 `page` 不复位，可能停在被导航隐藏的页面并继续渲染旧快照 | 半实现 | P2 | `src/App.tsx:50-51,59-64,47,74` | 会话切换后 UI 状态与权限不一致（服务端仍会 403 兜底） |
| G11 | 错误展示单一：仅 403 有定制文案；429/503/504/TIMEOUT/网络错误无分类、无 `Retry-After`、不展示 `request_id`；refresh 失败直接登出无重试 | 半实现 | P2 | `src/api/client.ts:61-65`；`src/App.tsx:44,68` | 排障缺请求标识，瞬时故障导致掉登录 |
| G12 | mock 不模拟分页、资源事件、4401/4403、429/5xx；`MockSocket` 不校验 token | 测试缺失 | P2 | `src/mock/server.ts:35-36,48-51` | 翻页刷新、实时事件刷新、错误状态、流鉴权失败均无法在 mock 下验证 |
| G13 | 无错误边界（无 ErrorBoundary/componentDidCatch）；无 i18n（全部硬编码 zh-CN，`<html lang="zh-CN">`）；无 a11y 自动化测试；表格无 `caption/scope`，状态枚举原样展示 | 未实现 / 测试缺失 | P2 | `src/App.tsx`（全文无错误边界）；`index.html:1`；`src/App.tsx:74,78,83`；`src/styles.css:1` | 渲染异常整页白屏；无法切换语言；可访问性回归无守护 |
| G14 | 实时日志页不是流式追加：`log.updated` 触发整页重取并把分页重置到第一页 | 半实现 | P2 | `src/stream/control-stream.ts:64`；`src/App.tsx:87-89` | 高频日志下“实时日志”体验为反复刷新首页 |
| G15 | 无 CSP/安全头配置、无生产 HTTPS 强制校验；`crypto.randomUUID()` 在非安全上下文会抛异常 | 未实现 | P2 | `src/api/client.ts:58`；`src/main.tsx:7-11`；`panel/` 无部署配置 | 误用 http 部署时写请求（含登录）前端报错；安全头依赖反代 |

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/07:42` 当前页面包含 9 页，“主要展示服务端资源” | 与代码一致（9 页只读） | `src/App.tsx:9-13,28-42` |
| `doc/07:44-57` 目标页面 12 项（含“会话和记忆”“系统设置”，且“插件市场和插件管理”） | 仅实现 9 页；无会话/记忆、系统设置；插件只有列表 | `src/App.tsx:9-13` |
| `doc/07:95-101` 验收标准含“插件启停、配置和日志页面不需要直接导入 Core 代码” | 面板确实不导入 Core，但**不存在**插件启停/配置页面，无法据此验收 | `src/api/client.ts:41-50`；`src/App.tsx:28-42` |
| `doc/18:234-245` 基线“至少需要管理”11 项；`doc/18:182` “可以启用、禁用和配置插件” | 面板对这些均为只读展示，零管理动作 | `src/App.tsx:28-42`；`src/api/client.ts:41-50` |
| `doc/18:419` “TypeScript/React 面板已实现，默认 mock，可切换 live” | 骨架+只读页+mock/live 属实；但按基线 7.1 的管理口径只有展示层 | 同上 |
| `doc/07:76` 检查命令仅列 `npm ci/check:contracts/lint/test/build` | `npm test` 需要 `control/api/tests/.venv` 与 `build/proto` 下 C++ 可执行文件；文档未说明 | `tests/live.test.ts:19`；`control/api/tests/conftest.py:17,28,49`；`doc/07:76` |
| `doc/07:15,38` “OpenAPI 生成客户端” | 生成的是**类型**，REST 端点与路径是手写字符串，未由 `paths` 派生 | `src/api/client.ts:1-14,42-51` |
| `doc/16:305-319` T09b“已完成并验收”，验收项为保留技术栈/登录刷新 401/403/断线恢复重同步/角色来自服务端/日志脱敏 | 5 项均有对应测试，与代码一致；但验收范围不含任何管理动作，T09b 的“真实集成”中唯一写操作（插件 disable）是测试直接用 `fetch` 调 API，未经过面板代码 | `tests/live.test.ts:39-81`（`:69-71` 直接 fetch）；`doc/16:313-318` |
| `doc/07:109-111` T09b 契约细则（导航按权限、401/4401 回登录、403 保留、轮换更新流令牌、先 SESSION_READY 后快照、已知事件+重连刷新、旧响应取消、分页回首页、RESYNC 后 hello、未知事件只推进序号、日志纯文本、不写持久化） | 逐项均能找到实现，未发现不一致 | `src/App.tsx:72-73,51-70,87-89`；`src/stream/control-stream.ts:37-64`；`src/api/client.ts:52-65` |
| `doc/06:220` “进程重启丢弃内存流会话，旧客户端收到 SERVER_RESTART 并重建快照” | 契约里 `SERVER_RESTART` 是 `RESYNC_REQUIRED.reason` 的取值（`api/asyncapi.yaml:29`），不是独立消息；面板按 `RESYNC_REQUIRED` 统一处理（正确），但文档措辞易被读成独立消息 | `api/asyncapi.yaml:29,318-324`；`src/stream/control-stream.ts:56-59` |

## 6. 跨模块影响（只陈述，不替其他模块下结论）

- → `contracts`（api/）：面板 7 个“契约已就绪但未接线”的端点（插件 validate/enable/disable/config/health、会话列表与撤销、actions 查询）如需进入面板，需要控制 API 侧确认响应/权限语义；另外 7 类管理动作（G2）在 OpenAPI 中根本不存在写端点，面板无法先行实现（待 contracts/control-api 确认）。
- → `control-api`：面板依赖 `/auth/me` 返回**展开的**权限列表（而非字面 `*`）；已核实 Core 侧 `control_permissions()` 返回展开列表（`core/src/application/control_crypto.cpp:125-136`），若服务端改为返回 `*`，面板导航将全部隐藏（`App.tsx:73` 用 `includes` 精确匹配）。面板还依赖 `/health` 的 `Health.components[].component/state/reason` 字段与 9 个列表接口的分页 `next_page_token`。
- → `ci-cd`：面板的全部检查定义在 `contracts` job（`.github/workflows/ci.yml:110-119`），而该 workflow 自动触发已停用（`.github/workflows/ci.yml:3-17`）；`build-test` job 不涉及面板（待 ci-cd 确认是否合入）。
- → `operations`：`deploy/compose.{staging,prod}.yml:75-78` 已预留 `panel` 服务镜像 `kiarabot-panel`，但仓库内没有面板 Dockerfile/静态站点构建产物入口（`panel/` 无 Dockerfile），生产镜像与反代（HTTPS/WSS、CSP）仍归 T11b（待 operations 确认）。
- → `toolchain`：`tests/live.test.ts` 使面板测试耦合 `build/proto` 下的 C++ 产物与 `control/api/tests/.venv`（`conftest.py:17,28,49`），面板验证不能脱离 C++/uv 环境独立执行（待 toolchain 确认是否有意为之）。
- → `doc/18`（受保护）：第 7.1 节“至少需要管理”与本实现的“只读面板”存在范围性差异，是否收窄基线或补实现由人类维护者决定；本报告不修改该文档。

## 7. 覆盖结论

- 一句话结论：panel 是一个**结构清晰、契约类型化、认证/刷新/流恢复均做得较扎实的只读看板**；九页全部实现但零管理动作，契约中已就绪的插件/会话/动作管理端点未接线，且其测试与 CI 接线存在可复现性缺口。
- 覆盖度估计：**约 41/70 个功能点完整覆盖，3 个部分覆盖，26 个未覆盖**。口径（按本报告枚举）：
  1. 认证与会话 7 点（登录/refresh/401/403/权限导航/退出撤销/不持久化）→ 7 完整；
  2. 九页只读展示 9 点 → 9 完整；
  3. 管理动作 14 点（插件校验/启用/停用/配置读/配置写/插件健康、会话列表+撤销他人、机器人账号、适配器连接、模型/API Key、群组白名单、用户角色权限、任务触发、审计导出/过滤）→ 0 完整（其中前 7 点 API 已就绪属面板未接线，后 7 点 API 亦缺）；
  4. 实时流 8 点（hello/resume/SESSION_READY 门控/RESYNC/退避重连/已知事件/未知事件/4401/4403）→ 7 完整 + 1 部分（4403）；
  5. live-mock 双模式 6 点（mock 认证与列表/mock 分页/mock 事件/mock 错误码、live 环境切换、live TLS 强制）→ 2 完整（mock 认证与列表、live 环境切换）+ 1 部分（mock 错误码/错误展示）+ 3 未覆盖（mock 分页、mock 事件、live TLS 强制）；
  6. 契约同步 4 点（生成、check:contracts 断言、漂移检测、生成物不入库）→ 3 完整 + 1 部分（漂移检测）；
  7. 测试与 CI 4 点（mock 集成、真实 Core 测试、CI 定义、CI 自动触发）→ 3 完整 + 1 未覆盖；
  8. 体验/质量 6 点（基础 a11y、a11y 自动化、i18n、错误边界、日志过滤、分页大小）→ 1 完整 + 5 未覆盖；
  9. 目标页面集合 12 项（`doc/07:44-57`）→ 9 完整 + 3 未覆盖（会话和记忆、系统设置、插件管理部分）。
  合计：完整 7+9+0+7+2+3+3+1+9 = 41；部分 0+0+0+1+1+1+0+0+0 = 3；未覆盖 0+0+14+0+3+0+1+5+3 = 26 → 41+3+26 = 70（“部分”计入分母但不计入完整）。

## 8. 未核实事项

- 未运行 `npm ci / npm run lint / npm test / npm run build`（本机无 node/npm，且按简报不得安装依赖）：无法确认 TypeScript 严格模式是否通过、`tests/live.test.ts` 在 CI 中是否实际通过、`jsdom` 环境下 `crypto.randomUUID` 与 Node 22 全局 `WebSocket` 是否可用（`tests/live.test.ts:62`、`src/api/client.ts:58`）。
- `check-contracts.mjs` 中 `openapiTS(openapiPath)` 的默认校验深度、`@asyncapi/parser` 版本对 `js-yaml` 直读的覆盖范围未实跑确认；仅依据源码断言判断其覆盖边界。
- mock 中 `satisfies Schemas['BotPage'|'AdapterPage'|...]`（`src/mock/server.ts:4-15`）是否与生成物完全匹配未编译验证；mock 的 `page: {}` 假定 `PageInfo` 字段全部可选，`api/openapi.yaml` 的 `PageInfo` 定义未逐字段核对。
- 未核实控制 API 在真实服务下的分页行为（`page_size`/`next_page_token` 生成条件）与 9 个列表接口的实际响应字段，因此“分页 UI 在 live 下可用”只是静态推断；面板只在 mock 上验证过无 `next` 的场景。
- 未核实 `doc/16` 之外是否有其他台账/验收记录宣称面板具备管理动作；也未核实 T11b 面板生产镜像的构建方式（仓库无 `panel/Dockerfile`，`deploy/compose.*.yml:75-78` 仅引用镜像名）。
- 未核实 `api/openapi.yaml` 的 `PluginConfigDelivery`/`PluginConfigUpdate` schema 细节与面板未来接线所需的兼容字段（`api/openapi.yaml:708-733` 只读到 required `expected_version, values`），该细节留给 control-panel 实现任务或 contracts 分析确认。
