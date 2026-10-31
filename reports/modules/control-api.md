# 控制 API（kiarabot-control-api）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/uv/node/protoc，仅有 python3.9/ruby/clang++）。
> 只读仓库；本文件是本模块唯一写入物，未修改任何代码、契约或 doc。

## 1. 范围与规模

- 目录/文件：`control/api/`（23 文件 / 2238 行）
  - 源码 967 行：`src/http_server.cpp`(326)、`src/http_mapper.cpp`(266)、`src/event_stream.cpp`(201)、`src/configuration.cpp`(53)、`src/credentials.cpp`(21)，头文件 4 个共 100 行；
  - 测试 837 行：`tests/*.py` 8 个共 684 行、`tests/event_stream_test.cpp`(81)、`tests/mock_server.cpp`(72)；
  - 构建/文档/锁文件 434 行：`CMakeLists.txt`(20)、`README.md`(63)、`tests/pyproject.toml`(7)、`tests/uv.lock`(344)。
- 契约（属 contracts 模块，本模块的输入）：`api/openapi.yaml`(840)、`api/asyncapi.yaml`(452)。
- 关键入口与代码地图：
  - 传输/连接生命周期：`control/api/src/http_server.cpp:19-162`（WebSocket）、`:163-247`（HTTP Connection）、`:249-325`（HttpServer 监听/停止）。
  - REST 路由 + 错误/HTTP 状态映射：`control/api/src/http_mapper.cpp:107-265`；统一错误体 `:18-29`；状态码来自 `core/src/application/control_application.cpp:156-180`。
  - 事件流（握手/重放/合并推送）：`control/api/src/event_stream.cpp:42-200`。
  - 私有配置读取：`control/api/src/configuration.cpp:11-52`；凭据工具：`control/api/src/credentials.cpp:5-21`。
  - 组合根（唯一生产接线点）：`core/app/main.cpp:223-271`；应用端口接口：`core/include/kiarabot/core/application/control_application.hpp:123-142`；真实实现：`core/src/application/control_service.cpp`。
  - 构建与测试注册：`control/api/CMakeLists.txt:1-20`（`:13-18` 的 pytest 目标只在 `KIARABOT_ENABLE_INFRASTRUCTURE=ON` 且 `tests/.venv` 存在时注册）。
- 对应设计文档：`doc/06-control-api.md`、`api/openapi.yaml`、`api/asyncapi.yaml`；历史快照 `doc/13-control-api-contract-handoff.md`；台账 `doc/16-development-tasks.md:262-303,524-559`；基线（只读）`doc/18-architecture-baseline.md:211-246,404,418`；运维 `doc/08-operations.md:199-220`。

## 2. 模块职责（依据）

- `doc/06-control-api.md:5`：控制 API = 同进程 C++ 应用端口 `IControlApplication`，由组合根注入；不新增跨进程管理 RPC；端口接收 bearer 凭据/request_id/trace_id/deadline/幂等键 + 类型化命令，**禁止自报 actor/role**；Core 每次重验会话、授权、事务与审计；HTTP 层按 OpenAPI 映射。
- `doc/06-control-api.md:21-23`：不直接连 NapCat、数据库、插件进程；只调用 Core 应用服务；协议为 REST+JSON、WebSocket、OpenAPI、AsyncAPI。
- `doc/06-control-api.md:112-143`：认证规范（Argon2id、access 15 分钟、refresh 30 天旋转）、分页规范、WS 频道/首帧/重放/resync 规范。
- `doc/06-control-api.md:216-221`（T09a）：同一监听提供 `/ws/control` 与 health/logs/adapters/plugins/tasks/messages 频道，100ms 合并推送，10 分钟保留，先 SESSION_READY 再重放。
- `doc/08-operations.md:199-205`：私有 0600 JSON 配置、`kiarabot-control-credentials`、回环监听、64KiB 请求体/16KiB 头/连接有界、登录限流、"不启用宽泛 CORS、不信任代理头"。
- `doc/18-architecture-baseline.md:404,418`：基线允许控制 API 由 `kiarabot-core` 进程内托管；REST+WS 已实现。
- 契约唯一来源：REST = `api/openapi.yaml`（`doc/06-control-api.md:5,15`），WS = `api/asyncapi.yaml`（`doc/06-control-api.md:140`）。

## 3. 已实现能力（逐条带证据）

### 3.1 OpenAPI path → control/api 实现映射（逐 path）

说明：
- 所有 path 的 HTTP 状态由 `http_mapper.cpp:18-29` + `core/src/application/control_application.cpp:156-180` 统一映射：INVALID_ARGUMENT=400、UNAUTHENTICATED=401、PERMISSION_DENIED=403、NOT_FOUND=404、CONFLICT=409、RATE_LIMITED=429（Retry-After: 60）、TIMEOUT=504、UPSTREAM_UNAVAILABLE/PLUGIN_UNAVAILABLE/CAPABILITY_NOT_SUPPORTED=503、其它=500；`request_id`/`trace_id` 每请求新生成，错误体含 `details:{}`。
- 未匹配的 path 或 method 一律 404（**没有 405**，`http_mapper.cpp:239-241`）；任何非 `page_size`/`page_token` 的 query 参数、重复 query 键、GET 带 body 一律 400（`:118-142,242-245`）；重复 header 由传输层 400（`http_server.cpp:191-201,214-220`）。
- "RBAC 权限"列为 Core `control_policy()` 计算并在 `control_service.cpp:325-329` 强制执行的权限；HTTP 层不做授权（见 §3.6）。

| # | OpenAPI path（方法） | operationId | 映射位置 | 状态码（声明 → 实际） | 幂等/分页 | RBAC 权限 | 判定 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | GET /health/live | getProcessLiveness | http_mapper.cpp:143-153 | 200/400/500 → 200 恒 `{"state":"HEALTHY"}`、400（query/body/非 GET）、500（异常兜底）；无 503 | 无 | public（无认证） | 完整（不触碰存储，符合 T11a） |
| 2 | GET /health/ready | getProcessReadiness | http_mapper.cpp:147-152 | 200/503/400/500 | 无 | public | 完整（调 `IControlApplication::readiness()` 写事务探针；503 带 retryable） |
| 3 | GET /auth/me | getCurrentPrincipal | http_mapper.cpp:171-172 | 200 + 400/401/403/429/500/503/504 | 无 | session.read | 完整；403 实际不可达（所有合法角色都有 session.read，见 control_service.cpp:159） |
| 4 | GET /auth/sessions | listSessions | http_mapper.cpp:213-236 | 200/400/401/403/429/500/503/504 | 分页（默认 20、1..100、opaque token、绑定用户+角色+资源+页大小、600s 快照） | session.read；非 admin 仅自己（control_service.cpp:593-607） | 完整 |
| 5 | POST /auth/sessions/{sessionId}/revoke | revokeSession | http_mapper.cpp:185-187 | 204/400/401/403/404/409/429/500/503/504 | 幂等（key 16..128；replay→204） | session.revoke；跨主体 NOT_FOUND（control_service.cpp:397-402） | 完整 |
| 6 | POST /auth/login | login | http_mapper.cpp:166-168 | 200 + 400/401/409/429/500/503/504 | 幂等（作用域=规范化用户名；同键同载荷 replay 原 TokenPair，撤销/代次失效→401） | public | 完整（Argon2id；未知用户名也做等价验证；30 次/用户名/分钟） |
| 7 | POST /auth/refresh | refreshToken | http_mapper.cpp:169-170 | 200 + 400/401/409/429/500/503/504 | 幂等（作用域=原 session_id；旧 refresh 换新键→401；代次校验） | refresh.credential（实现跳过角色检查，凭 refresh digest，control_service.cpp:325-328,372-374） | 完整 |
| 8 | GET /health | getHealth | http_mapper.cpp:173-174 | 200/503/400/401/403/429/500/504 | 无 | health.read（全角色） | 完整；**每次 GET 会提交一次写事务**（control_service.cpp:477-483） |
| 9 | GET /adapters | listAdapters | http_mapper.cpp:213-236 | 200 + 通用错误 | 分页 | adapter.read | 完整（未配置 `adapter_id` 时真实空集） |
| 10 | GET /plugins | listPlugins | 同上 | 200 + 通用错误 | 分页 | plugin.read | 完整 |
| 11 | POST /plugins/validate | validatePlugin | http_mapper.cpp:175-180 | 200 + 400/401/403/409/429/500/503/504 | 幂等 | plugin.manage | 完整（不支持 runtime 以 200 + `CAPABILITY_NOT_SUPPORTED` 文本表达，不返回 503） |
| 12 | POST /plugins/{pluginId}/enable | enablePlugin | http_mapper.cpp:188-195 | 202 + 400/401/403/404/409/429/500/503/504 | 幂等 | plugin.manage | 完整（只持久化投递开关，不声称进程已启动） |
| 13 | POST /plugins/{pluginId}/disable | disablePlugin | http_mapper.cpp:193-195 | 202 + 同上 | 幂等 | plugin.manage | 完整 |
| 14 | GET /plugins/{pluginId}/config | getPluginConfig | http_mapper.cpp:196-198 | 200 + 400/401/403/404/429/500/503/504 | 无 | plugin.config.read | 完整（可选 `delivery` 观察值） |
| 15 | PUT /plugins/{pluginId}/config | updatePluginConfig | http_mapper.cpp:202-209 | 200 + 400/401/403/404/409/429/500/503/504 | 幂等（`expected_version` 乐观锁；敏感字段 400） | plugin.config.write | 完整（replay 只回持久化，不含 delivery） |
| 16 | GET /plugins/{pluginId}/health | getPluginHealth | http_mapper.cpp:199-201 | 200 + 400/401/403/404/429/500/503/504 | 无 | plugin.read | 完整 |
| 17 | GET /audit | listAuditEntries | http_mapper.cpp:213-236 | 200 + 400/401/403/429/500/503/504 | 分页（occurred_at,id；600s） | audit.read（仅 admin，control_application.cpp:85-87） | 完整；全量构建后切页（见 §4 #7） |
| 18 | GET /bots | listBots | 同上 | 200 + 通用错误 | 分页 | bot.read | 完整（数据=适配器账号配置） |
| 19 | GET /models | listModels | 同上 | 200 + 通用错误 | 分页 | model.read | 完整（需配置 `KIARABOT_MODEL_NAME`，否则空集） |
| 20 | GET /groups | listGroups | 同上 | 200 + 通用错误 | 分页 | group.read | **半实现：无群组数据源，恒空**（`control_service.cpp:587-699` 无 Groups 分支） |
| 21 | GET /users | listUsers | 同上 | 200 + 400/401/403/429/500/503/504 | 分页 | user.read（仅 admin） | 完整 |
| 22 | GET /tasks | listTasks | 同上 | 200 + 通用错误 | 分页 | task.read | 完整（取自运行时快照任务） |
| 23 | GET /actions/by-idempotency-key | getActionByIdempotencyKey | http_mapper.cpp:181-182 | 200 + 400/401/403/404/429/500/503/504 | 无（header 作为查询键） | action.read | **未接线：只过滤 owner=="control/"+用户 id 的动作，而全仓无该 owner 的生产者 → 实际永远 404**（`control_service.cpp:489`） |
| 24 | GET /actions/{actionId} | getActionResult | http_mapper.cpp:210-212 | 200 + 同上 | 无 | action.read | **未接线：同上**；且未实现 7 天保留/`ACCEPTED` 过期判定（对照 `core/src/application/action_router.cpp:153-190`） |
| 25 | GET /logs | listLogs | http_mapper.cpp:213-236 | 200 + 400/401/403/429/500/503/504 | 分页（occurred_at,id） | log.read（operator/admin） | 完整但数据源耦合：日志来自 `ControlOptions.logs`（SQLite 构建才有，`core/app/main.cpp:227-230`），内容=审计条目的派生日志 |

补充：`/health/live`、`/health/ready` 在契约中 `servers:/`（根路径），实现只在根路径提供（`http_mapper.cpp:143`）；其余在 `/api/v1/` 前缀下（`http_mapper.cpp:166-235`），与 `openapi.yaml:10,45,59` 一致。

### 3.2 AsyncAPI channel/event → event_stream.cpp 映射

事件流机制（先给结论）：频道 = URL `/ws/<channel>`（`http_server.cpp:202-211`，不含 `?%#`）；首帧只接受 `hello`/`resume`，第二帧起任何文本帧直接 4400（`http_server.cpp:49-52`）；100ms 轮询（`:74-89,264-272`）；每会话序号 + 10 分钟/4096 条环形重放（`event_stream.cpp:144-158,173-195`）。

| AsyncAPI channel / 消息 | 实现证据 | 输出内容/语义 | 判定 |
| --- | --- | --- | --- |
| `/ws/control` | `event_stream.cpp:8-15,19-29` | 聚合 kEvents 通道；按 Core 权限过滤 | 部分：health/message 两类事件从不产生（见下） |
| `Hello` → `SESSION_READY` | `event_stream.cpp:45-72,106-111`；`asyncapi.yaml:276-282,301-308` | `type/access_token/last_sequence(必须 0)`；回 `session_id`(=32 位实例前缀+64 位随机)+确认的 `last_sequence` | 完整 |
| `Resume` → 重放/`RESYNC_REQUIRED` | `event_stream.cpp:73-105`；`asyncapi.yaml:309-324` | 重新认证+身份/频道/权限绑定；`sequence<oldest`→CURSOR_EXPIRED、`>session.sequence`→INVALID_CURSOR、id 前缀属旧进程→SERVER_RESTART、仍在连接→INVALID_CURSOR；resync 帧后 close 1000 | 完整（resync 三理由与 asyncapi.yaml:29 一致） |
| `/ws/health` + `health.updated` | `event_stream.cpp:8-15,128-151`；`control_service.cpp:716-722` | **投影 health 是常量 `HEALTHY`+单个 storage 组件，永不变化 → 该事件永不产生**；且与 REST `/api/v1/health`（`control_service.cpp:443-476`，含 adapter/plugin 聚合）语义不一致 | **未实现**（握手成功但永远静默） |
| `/ws/logs` + `log.updated` | `event_stream.cpp:11-15,128-151`；`control_service.cpp:675-685` | 仅 log.read 持有者；payload 为 OpenAPI `LogEntry` 形状（含 name/id） | 完整（数据源见 §4 #11） |
| `/ws/adapters` + `adapter.updated` | `control_service.cpp:635-640` | 仅配置 adapter_id 时有数据；connect/disconnect 触发 | 完整 |
| `/ws/plugins` + `plugin.updated` | `control_service.cpp:608-630,730-737`；`event_stream.cpp:140-143` | payload 裁成 `{id,version,state}`；`_config_version` 变化也会触发（期望配置变化通知） | 完整 |
| `/ws/tasks` + `task.updated` | `control_service.cpp:664-673` | `{id,name,state}` | 完整 |
| `/ws/messages` + `message.updated` | `event_stream.cpp:8-15`（有 message.read 映射、无 messages 事件）、`control_crypto.cpp:125-136`（权限清单无 message.read）、`control_service.cpp:700-744`（投影无 messages） | 握手必然 4403；无任何 message.* 事件 | **未实现/契约不一致**（见 §4 #1） |
| 首帧边界与 revision 语义 | `event_stream.cpp:54-57,98-110`；`asyncapi.yaml:22,283-292` | `hello.last_sequence` 必须 0；`SESSION_READY.last_sequence`=客户端已接受的重放边界；序号每会话单调、按频道会话独立；无全局 revision；重放先于实时，单批 32（`:186-193`）；队列>64 帧 → 1013（`http_server.cpp:94-96`，**契约未列 1013**） | 基本完整（1013 未记录） |

握手/订阅/断线恢复关键实现点：
- 首帧 5s 超时（`http_server.cpp:29-39`）→ 4400；握手完成后按 100ms 轮询（`:64-67,74-89`）。
- 断线：`detach` 记录 `detached_at`，会话与事件保留 10 分钟（`event_stream.cpp:160-172,196-200`），期间仍每 100ms 更新（见 §4 #4）；超时后会话删除→ resume 得 CURSOR_EXPIRED。
- 撤销/权限变化/access 过期：每 100ms 重新认证（`event_stream.cpp:115-126`）→ 失败码 4401/4403/1011 并关闭（`event_stream.cpp:30-34`；`http_server.cpp:84-87`），测试断言 <1s（`test_stream.py:48-54`）。
- 容量：会话 1024（`event_stream.cpp:68-70` → 1011）、每流事件 4096/10 分钟、慢消费者队列 64 → 1013。

### 3.3 认证、token 轮换、密码学、失败锁定、TLS

| 能力 | 证据 | 说明 |
| --- | --- | --- |
| Argon2id 密码哈希 | `core/src/application/control_crypto.cpp:44-58` | 配置只存 `$argon2id$...`（`control_service.cpp:158` 强制前缀）；未知用户名也验证首个用户哈希，避免用户名枚举时序差（`:272-276`） |
| 随机 token/会话 ID | `control_crypto.cpp:59-64` | 32 字节随机 → 64 位 hex；access/refresh 每代独立随机 |
| 令牌仅存 keyed BLAKE2b 摘要 | `control_crypto.cpp:65-74`；`control_service.cpp:389-391,298-304` | DB 中无明文 token（`test_http.py:49-51` 断言） |
| 会话/幂等状态加密（secretbox） | `control_crypto.cpp:75-103`；`control_service.cpp:41-48,577-578` | 整个状态文档 secretbox 加密后放 `$control` 命名空间 |
| 登录 / refresh 轮换 | `control_service.cpp:369-394` | access 900s、session 30 天、generation+1；旧 access 立即 401（`test_http.py:40-44`） |
| 撤销 | `control_service.cpp:397-402`；`test_stream.py:48-54` | REST 立即 401；WS <1s 关闭 4401 |
| 失败锁定/限流 | `control_service.cpp:251-277` | 只做 `30 次/规范化用户名/分钟` 限流（第 31 次 429 + Retry-After 60），**无账号锁定、无全局/IP 维度限流** |
| 幂等 | `control_service.cpp:330-363,564-585` | 作用域=actor(或用户名/原 session_id)+operationId+资源+键；keyed digest fingerprint；同键异载荷 409；成功副作用+receipt+审计同事务；失败不缓存；24h 保留；token 回放校验会话有效+代次+access 未过期 |
| 私有配置 | `configuration.cpp:12-26` | O_NOFOLLOW、常规文件、属主=当前用户、严格 0600、1B..1MiB |
| 密钥工具 | `credentials.cpp:5-18`；`doc/08-operations.md:203` | `--key` 生成 32 字节 hex；否则 stdin 读密码输出 Argon2id 哈希 |
| TLS | 代码无 TLS（仅 `Tcp::v4` 回环，`http_server.cpp:305-307`） | 契约/README 明确由同机反向代理终止 HTTPS/WSS（`control/api/README.md:43-46`、`doc/08-operations.md:205`）；`tests/test_tls.py:14-64` 用临时证书+本地 TLS relay 验证 HTTPS 登录 + WSS 握手/事件，并校验服务端证书；**未验证生产代理、证书管理、mTLS**（T11b 未验收） |

### 3.4 控制面数据来源：Core 端口 vs control/api 自身

| 数据/行为 | 来源 | 证据 |
| --- | --- | --- |
| `/api/v1/auth/*`、`/health`、全部列表、插件 validate/enable/disable/config/health、Action 查询 | `IControlApplication::invoke` → `ControlService`（Core 端口） | `http_mapper.cpp:246`；`control_application.hpp:123-127`；`core/app/main.cpp:259-270` |
| `/health/live` | control/api 直接合成（进程能响应即 200） | `http_mapper.cpp:143-153` |
| `/health/ready` | Core 端口 `readiness()`（默认实现返回 503，见 `control_application.hpp:129-134`；生产 `control_service.cpp:184-199` 用写事务探针） | `http_mapper.cpp:147-152` |
| WS 事件数据 | Core 端口 `stream_snapshot(metadata)` 每 100ms 拉取脱敏投影 | `http_server.cpp:261-263`；`event_stream.cpp:42-44,115-159`；`control_service.cpp:700-744` |
| WS 会话、session_id、序号、重放缓冲、事件类型/裁剪、合并语义 | control/api 自身状态/推导，**不在 Core** | `event_stream.cpp` 全文 |
| request_id/trace_id、deadline(30s)、page_size 默认 20、Retry-After: 60、HTTP 错误体/状态码、WS close code、队列/连接上限 | control/api 自身 | `http_mapper.cpp:108-112,18-29,226-236`；`http_server.cpp:31-33,94-96,205,277` |
| 角色→权限、登录/刷新/撤销、幂等、审计、分页快照、资源投影、健康聚合、插件状态 | Core（`ControlService`/`control_application.cpp`/`control_crypto.cpp`） | 见 §3.1 各 path 列 |
| mock 数据 | 仅测试夹具 `control/api/tests/mock_server.cpp`（契约测试用），生产无 mock | `mock_server.cpp:11-56`；`conftest.py:122-128` |

是否存在绕过 Core 规则的风险（只陈述事实）：
1. HTTP 层不做任何授权/业务判断，全部委托 Core（符合 `doc/06:5` 的设计），但 `/health/live` 完全绕过 Core（设计如此）。
2. WS 的"频道→权限"映射由 transport 自己维护（`event_stream.cpp:8-10`），与 asyncapi 声明重复；REST 的 operation→permission→role 三重一致性有 Ruby 契约测试对照编译后的 Core（`proto/tests/api_contract_test.rb:100-130`），**但 WS 频道权限映射没有任何对照检查**——`message.read` 这类不可授予权限因此未被发现（见 §4 #1）。
3. WS 授权实际依据 Core 计算的 `permissions` 数组（`event_stream.cpp:19-29`）而非客户端自报，且 Core 的 `stream_snapshot` 自身再按角色裁剪（`control_service.cpp:723-739`），属双重过滤，未发现身份自报绕过。
4. control/api 生成的 `ControlMetadata` 可填 request_id/trace_id/deadline，但 Core 会校验长度/时限（`control_application.cpp:107-120`），不能伪造 actor/role（`ControlMetadata` 无身份字段，`control_application.hpp:15-21`）。
5. 插件 manifest 投影白名单过滤字段（`control_service.cpp:620-630`），不会把 entrypoint/config schema 泄漏到 REST；但 `$control.manifests` 内容本身由 Core PluginHub 写入，未经 OpenAPI schema 校验（见 §4 #16）。

### 3.5 其他已实现能力（测试覆盖见 §3.7）

| # | 能力 | 证据 | 对应测试 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 回环监听、拒绝外部地址、端口解析 | `http_server.cpp:295-316` | `test_operations.py:21-37`（间接） | 完整 |
| 2 | 64KiB body / 16KiB header / 128 HTTP 连接 / 128 WS 上限 | `http_server.cpp:168-169,205,277` | `test_http.py:130-144`（超长 Content-Length、重复授权头） | 完整（见 §4 #5） |
| 3 | 统一错误体与状态码映射 | `http_mapper.cpp:18-29`；`control_application.cpp:156-180` | `test_http.py` 全篇、`control_service_test.cpp:279-290` | 完整 |
| 4 | 不可变分页快照（600s、绑定、末页省略 token） | `control_service.cpp:403-431` | `test_http.py:91-105` | 完整 |
| 5 | 幂等（登录/刷新/撤销/validate/enable/disable/config PUT） | `control_service.cpp:330-363,564-585` | `test_http.py:33-88`；`test_config_delivery.py:69-72` | 完整 |
| 6 | 审计（成功、拒绝、鉴权失败；审计满阻断写） | `control_service.cpp:233-244,364-366,577-585` | `test_http.py:83-88`；`test_operations.py:73-84` | 完整 |
| 7 | 插件配置下发闭环 delivery 状态 | `control_service.cpp:549-556`；`plugin_config_delivery.cpp` | `test_config_delivery.py:36-101` | 完整（依赖 SDK 环境） |
| 8 | 进程探针 live/ready 语义与启动门禁 | `http_mapper.cpp:143-152`；`core/app/main.cpp:223-270` | `test_operations.py:21-71` | 完整 |
| 9 | WS 首帧认证、重放、resync、撤销关闭 | `event_stream.cpp:45-114` | `event_stream_test.cpp:15-81`；`test_stream.py` | 完整（见 §4 #1/#2） |
| 10 | 启动失败路径（坏密钥、错误权限、坏 schema） | `configuration.cpp:12-26`；`control_service.cpp:143-183` | `test_operations.py:87-103` | 完整 |
| 11 | TLS 终止代理链路（HTTPS 登录 + WSS 事件） | 代理侧无代码，测试用 relay | `test_tls.py:14-64` | 仅测试验证，非生产认证 |

### 3.6 测试覆盖（逐文件：覆盖什么 / 缺什么）

| 测试文件 | 覆盖 | 缺口 |
| --- | --- | --- |
| `tests/event_stream_test.cpp`（CTest `control_event_stream`） | 非法首帧 4400、频道拒绝 4403（含 messages）、身份/频道绑定、SESSION_READY 边界、重放顺序、10 分钟过期 CURSOR_EXPIRED、32 条分批、撤销 4401、权限变化 4403 | 握手超时、队列溢出 1013、已在连时 resume→INVALID_CURSOR、sequence>水位→INVALID_CURSOR、SERVER_RESTART（在 py）、health/messages 事件（不可能）、会话上限 1024→1011、detach 10 分钟清理、慢消费者。
| `tests/test_http.py` | 14 条读路由 schema+401、viewer 403（logs/users/audit/config）、Action 查询 404、分页 6 种非法参数、登录幂等 replay、轮换后旧 access 401、refresh 同键 replay/换键 401、重启后会话与幂等恢复、DB 无明文 token/密码、撤销→401、写幂等/版本冲突/敏感字段 400、审计动作与计数、不可变分页跨用户/资源/页大小 400、SQLite 锁→409 且无副作用、登录限流 30/429、oversize/重复授权头 400 | mock 与 real 双跑（`conftest.py:122-128`）但 **Action 查询成功路径无测试（不可达）**；/health/ready 在 mock 下 503 未覆盖；端到端 504 TIMEOUT（core 单测用 fake 应用断言过映射，`control_service_test.cpp:279-290`）、CAPABILITY_NOT_SUPPORTED 503、容量型 429（receipts/page cursor/sessions）、审计满→503、权限矩阵逐操作允许路径未穷举、CORS/OPTIONS/405、Authorization scheme 大小写、Content-Type charset、64KiB 边界完整链路、并发同键写。 |
| `tests/test_stream.py` | 真实进程下 live plugin 事件、先流后快照、resume 重放逐帧相等、撤销 4401<1s、频道权限（viewer→logs 403、admin→messages 403、plugins ok）、跨身份 resume 4403、重启 SERVER_RESTART、坏 token 4401、负数/非法 hello 4400 | **health/logs/adapters/tasks 事件均无测试**；10 分钟保留/32 分批仅 C++ 单测；1013、握手超时、access 过期→4401、permission 变更→4403、刷新后新 access resume、同会话多流、消息事件。 |
| `tests/test_config_delivery.py` | 离线 PUT→SDK 连接后 APPLIED、applied_version、幂等 replay 不重复应用、REJECTED(INVALID_ARGUMENT)/REJECTED(TIMEOUT)、恢复、SDK 停止→UNAVAILABLE、双进程重启保留 version 且清空 applied、disable→UNAVAILABLE | 需真实 SDK venv/插件，CI 外不可跑；并发写、disabled 状态下的写入、mock 端 delivery、跨用户可见性未覆盖。 |
| `tests/test_operations.py` | live 200 独立于存储锁、ready 锁下 503、/health 503、迁移重复/损坏/锁（属 infrastructure）、迁移失败时 daemon 不宣告 control_start、/logs 脱敏（无密码/token）、viewer /audit 403、坏密钥/0644 配置 fail-closed | 优雅关闭、监听占用/EADDRINUSE、ready 并发、日志 10 分钟保留、`/api/v1/health` 组件语义。 |
| `tests/test_tls.py` | 本地 TLS relay：HTTPS POST /auth/login、WSS /ws/plugins、事件推送；使用自签 CA 校验服务端证书 | **不是 Core 的 TLS**；无 mTLS/客户端证书（与 `doc/16:540` 表述不符，见 §5）、无证书轮换/HSTS/代理头注入测试、依赖 openssl CLI。 |

## 4. 缺口清单

| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |
| 1 | **`messages` 频道永久 4403，且无 `message.updated` 事件投影**：asyncapi 声明 `messages: message.read`，但 Core 权限清单（16 项）不含 `message.read`，配置只有 role 无附加权限字段，`control_permissions()` 也不可能返回它；事件流 kEvents 无 messages | 未实现 | **P1** | `event_stream.cpp:8-15`；`core/src/application/control_crypto.cpp:125-136`；`control_service.hpp:11-16`；`configuration.cpp:46-49`；`api/asyncapi.yaml:20-21,126-139`；`doc/06-control-api.md:13,122,136,218` | 7 个声明频道中 1 个不可用；doc/06 承诺的"显式 message.read 策略"没有实现入口（含管理员）；客户端无法订阅消息元数据；单测把 4403 当预期（`event_stream_test.cpp:37`），说明缺口被固化为行为 |
| 2 | **`/ws/health` 永不推送 `health.updated`，且流内 health 投影与 REST 健康聚合不一致**：stream_snapshot 的 health 是常量 `HEALTHY`+storage；REST /health 会聚合 adapter/plugin 并返回 DEGRADED | 半实现 | **P1** | `control_service.cpp:716-722` vs `:443-476`；`event_stream.cpp:11-15,128-151`；`api/asyncapi.yaml:56-69,411-417`；`doc/06-control-api.md:131,218` | 健康频道名存实亡；控制面板无法通过 WS 感知健康变化，只能轮询 REST；与 T09a "推送资源变化" 声明不符 |
| 3 | **两个 Action 查询 REST 端点无数据生产者**：查询过滤 `owner=="control/"+user_id`，全仓只有 `plugin/*`、`adapter/*` owner；REST 也没有创建 Action 的入口；且查询不实现 7 天保留/`ACCEPTED` 过期判定，过期后返回旧结果而非 NOT_FOUND | 未接线 | **P1** | `core/src/application/control_service.cpp:484-515`（owner 过滤 :489、无过期判定）；`core/src/application/action_router.cpp:153-190,222-230`；`api/openapi.yaml:449-482,691-707`；`doc/06-control-api.md:161,163` | 契约声明的 Action 查询功能对控制面板不可用（面板也未调用，见 §6）；若未来接线，保留期语义会与契约/桥接查询不一致 |
| 4 | **事件流 100ms 轮询对每会话做 2 次全量存储读取 + 状态解密，且对已断开会话继续 10 分钟；单 io 线程串行、与 REST 共享 Core/存储互斥**：`tick()` 遍历 sessions_（含 detached）→ `stream_snapshot()` 内部先 `invoke_locked`（begin+load+decrypt）再 `store_.read()`，每个会话每 100ms 一次，全部在 HTTP io 线程上 | 半实现 | **P1**（对外暴露后上升 P0） | `event_stream.cpp:160-172`；`http_server.cpp:264-272`；`core/src/application/control_service.cpp:208-221,700-740`；`infrastructure/src/sqlite_runtime_store.cpp:229-238`；`doc/16-development-tasks.md:531` | 只要有合法低权账号即可建满 1024 个流会话（断线仍保留 10 分钟），把单线程拖入持续全量 DB 读取+解密，REST 与共享 RuntimeStore 一起变慢；无 per-principal 连接配额、无快照复用/批处理；无负载/容量测试（`README.md:58-59` 自认非负载认证） |
| 5 | **生产可用性面缺失**：REST 无通用限流（仅登录）、无每用户连接/流配额、无 CORS/OPTIONS、无 keep-alive、无优雅关闭（`stop()` 直接 `io.stop()`，不发送 WS close、不等在途请求）、无 TLS（依赖反代，T11b 未验收）、无访问日志 | 未实现 | P1 | `http_server.cpp:228,317-322`；`doc/08-operations.md:205`；`control/api/README.md:43-53`；`doc/16-development-tasks.md:597-601` | 部署到真实反向代理/多用户前不具备生产可用性；一个用户可占满 128 个 WS/连接；重启时客户端看到异常断开而非 close 帧；浏览器跨源需自建同源代理 |
| 6 | **无请求级访问日志、不传播入站 trace**：request_id/trace_id 每请求随机生成，忽略 `traceparent`/`X-Request-ID`，成功响应不回传 request_id；HTTP 层不打日志（只有 Core 审计/存储事务派生日志） | 未实现 | P2 | `http_mapper.cpp:108-111`；`http_server.cpp:222-239`（无日志）；`core/src/runtime/runtime_log.cpp:34-56`（日志仅来自新增审计条目） | 跨面板/Core 的请求追踪断链；成功 GET 无可观察记录；排障依赖客户端 |
| 7 | **分页快照按完整资源集复制进内存**：每次首页请求把全部资源数组存入 `pages_`，最多 1024 个游标 × 资源全集；`/audit` 上限 10 万条、sessions 4096、logs 4096，且每次列表都全量构建+排序再切页 | 半实现 | P2 | `control_service.cpp:403-431`（`page.items = resources(...)` :409、`pages_` 上限 :423-425）；`:587-699,686-698` | 任意 session.read 用户可请求 `/auth/sessions?page_size=1` 1024 次，长期占用内存；审计列表单次请求成本随数据量线性增长（10 万条还要做 Struct 转换） |
| 8 | **方法不允许返回 404 而非 405，无 OPTIONS/HEAD**；`Authorization` scheme 大小写敏感（仅 `Bearer `）；Content-Type 必须严格等于 `application/json`（拒绝 `; charset=`）；401 无 `WWW-Authenticate` | 半实现 | P2 | `http_mapper.cpp:114-116,157-159,239-241`；`http_server.cpp:222-239` | 非浏览器/严格 HTTP 客户端互操作摩擦；无法用于自描述 API；CORS 预检失败（叠加 #5） |
| 9 | **优雅关闭缺失**：`HttpServer::stop()` 先 `io.stop()` 再 join，无连接排空、无 1001/1012 close、无超时；WS 客户端收到 1006 异常关闭 | 未实现 | P2 | `http_server.cpp:317-322`；对照 `doc/08-operations.md:189` 的"停止信号取消流、等待处理器退出" | 重启/滚动升级期间客户端需全量重连与重同步，台账的关闭语义对控制监听不成立 |
| 10 | `/api/v1/health`（非变更操作）每次提交一次写事务（BEGIN IMMEDIATE），健康轮询会与业务写竞争 SQLite | 半实现 | P2 | `control_service.cpp:477-483` | 运维高频探针（Compose/脚本）放大写锁压力；与 T11a "验证存储可写" 的意图一致但代价高 |
| 11 | `/logs` 并非独立运行日志：数据来自 `LoggingRuntimeStore` 把每条新审计条目转成日志（operation=operationId、outcome=OK/错误码），成功 GET 不产生日志；仅 SQLite 构建且 sink 存在时有数据 | 半实现 | P2 | `core/src/runtime/runtime_log.cpp:34-56`；`control_service.cpp:675-685`；`core/app/main.cpp:227-230`；`doc/08-operations.md:209` | `/logs` 与 `/audit` 高度重叠，无法用于访问/性能排障；无 SQLite 构建时恒空 |
| 12 | `/groups` 无数据源，恒返回空页（契约声明 Group 列表） | 半实现 | P2 | `control_service.cpp:587-699`（无 Groups 分支）；`api/openapi.yaml:398-414` | 面板群组页面永远为空；"未配置资源返回真实空集合"（`doc/06:212`）的兜底，但对群组域属未实现 |
| 13 | 登录容量可被低权用户耗尽：4096 会话上限是全局的，合法用户可用 30 次/分钟反复登录占满后，其他人登录得到 429 | 半实现 | P2 | `control_service.cpp:263-265,376-378`；`api/openapi.yaml:41` | 契约内行为，但缺少按用户的会话配额/清理策略；与 #4 叠加形成可用性风险 |
| 14 | 错误码/契约打磨：WS 慢消费者用 1013、握手超时用 4400（"非法首帧"语义），都不在 asyncapi `close_codes`；`/auth/me` 等声明了不可达 403；Action 查询成功路径无 200 可达 | 契约不一致 | P2 | `http_server.cpp:94-96,127-138`；`api/asyncapi.yaml:28`；`api/openapi.yaml:73-88,449-482` | 客户端生成的关闭码处理不完整；契约与实际可产生状态不完全一致 |
| 15 | **测试缺口**：无 health/messages 事件测试；无端到端 504 TIMEOUT（仅 core 单测 fake 断言映射，`core/tests/control_service_test.cpp:279-290`）、无 503 CAPABILITY_NOT_SUPPORTED、容量型 429（sessions/receipts/page cursor）、审计满 503；无 1013/握手超时/access 过期/权限变化关闭；无 CORS/OPTIONS/405、优雅关闭、并发同键写、64KiB 边界完整链路；mock 端无 ready/config delivery | 测试缺失 | P2 | `tests/` 全文；`CMakeLists.txt:13-18`（acceptance 目标依赖 `.venv` 才注册）；`doc/16-development-tasks.md:546`（历史 16/16，本机不可复跑） | 上述缺口全部无回归保护；CI 自动触发已暂停（`ci.yml:3-17`），测试实际未持续执行 |
| 16 | 插件 public manifest 从 `$control.manifests` 直接投影，未经 OpenAPI schema 校验；若注册 manifest 缺 `name/api_version/runtime`，`/plugins` 会返回不合 schema 的文档；运行时无 schema 校验（只有测试校验） | 未验证 | P2 | `control_service.cpp:608-630`；`plugin_hub.cpp:195`；`doc/06:5`（"HTTP 层按 OpenAPI 校验映射"） | 契约漂移只能靠测试发现；与契约文档的"按 OpenAPI 校验"表述不符 |
| 17 | 凭据 CLI 输入不关闭终端回显、无 TTY 检查、无 `--help`；启动诊断只输出 `error_code=INTERNAL`（坏密钥/用户配置/监听失败都不区分） | 半实现 | P2 | `credentials.cpp:13-18`；`core/app/main.cpp:403-405`；`control_service.cpp:143-183` | 密码可能进入终端回滚缓冲；运维定位启动失败成本高 |
| 18 | 无内置 TLS、无 IPv6、无 SO_REUSEADDR（固定端口重启可能偶发 EADDRINUSE），`listen` 只接受字面 `127.0.0.1:<port>` | 未实现 | P2 | `http_server.cpp:296-315`；`README.md:43-46` | 均为已知设计限制（T11b/反代承担），但会以启动失败/连接异常形式暴露给运维 |

> 未发现 P0 级鉴权绕过、明文凭据落库、路径缺失或契约路径无法路由的问题：25 个 OpenAPI 操作全部有路由；RBAC/审计/加密/撤销路径在代码与测试中成立。最深的问题是 #1-#4（P1）。

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/06:218`（T09a）"同一监听提供 `/ws/control` 及 health/logs/adapters/plugins/tasks/messages 频道"；`api/asyncapi.yaml:20-21,126-139` 声明 messages 频道与"显式策略（含管理员）" | messages 频道握手必 4403；无 message.updated；无 message.read 授予机制 | `event_stream.cpp:8-15,19-29`；`control_crypto.cpp:125-136`；`control_service.hpp:11-16`；`event_stream_test.cpp:37` |
| `doc/06:218` "推送变更后的当前状态"、`doc/16:531`（T09a）事件流实现 | health 投影是常量，health.updated 永不产生；健康状态只能靠 REST | `control_service.cpp:716-722` vs `:443-476` |
| `doc/16:540` "TLS 测试使用临时证书并**启用客户端证书校验**" | 测试只把自签证书当 CA 校验**服务端**证书，客户端不提供证书，无 mTLS | `tests/test_tls.py:20-23,50-56` |
| `doc/06:5` "HTTP 层按 OpenAPI 校验映射" | 无任何运行时 schema 校验；仅手工映射 + 测试用 jsonschema 校验响应 | `http_mapper.cpp:52-105`；`conftest.py:106-110` |
| `doc/06:187` 验收"OpenAPI 文档可生成 TypeScript 和 **C++** 客户端" | 仓库只有面板的 TS 生成/校验（`panel` check:contracts），无 C++ 生成器或校验入口 | `scripts/quality/check-openapi.sh:6`；无 C++ 生成脚本 |
| `doc/06:161` "终结结果保留 7 天"（Action 查询） | 控制面 Action 查询没有 7 天/过期判定；桥接侧 `ActionRouter::lookup` 才有 168h 丢弃 | `control_service.cpp:484-515` vs `action_router.cpp:174-178` |
| `doc/16:538` "配置更新仅持久化非秘密控制值，当前 SDK 不消费这些值" | 同文档 `doc/16:561-577` 记录配置下发闭环后 SDK 已消费并 ACK；`test_config_delivery.py` 验证真实 SDK 应用 | `doc/16-development-tasks.md:538,561-577`；`test_config_delivery.py:36-101`（台账内自述前后矛盾，属历史记录未加"已过时"标注） |
| `doc/08:189` "停止信号取消流、等待处理器退出后返回 0" | 控制监听 `stop()` 立即 `io.stop()`，不排空在途请求、不发 WS close，仅 join io 线程 | `http_server.cpp:317-322` |
| `README.md:48-53` 限制（64KiB/16KiB/128+128/1024 流会话/4096 事件/32 批） | 与代码一致（核对通过） | `http_server.cpp:168-169,205,277`；`event_stream.cpp:68-70,153-158,186-193` |
| `doc/16:508` "编译后的全部 23 个操作与 OpenAPI 权限/默认角色一致" | REST 侧一致（Ruby 对照测试）；**WS 频道权限映射不在对照范围**，`message.read` 漂移未被发现 | `proto/tests/api_contract_test.rb:100-130` |
| `doc/18:418` "控制 API：REST + WebSocket 已实现" | REST 成立；WS 7 频道中 messages 不可用、health 无事件，属部分实现 | 见 §4 #1/#2 |
| `doc/13` 整篇为历史快照（`doc/13:3-5` 自述） | 其中 P0 清单已由 `api/openapi.yaml`/`asyncapi.yaml` 与实现吸收；不影响当前实现判定 | `doc/13-control-api-contract-handoff.md:3-5` |

## 6. 跨模块影响（只陈述，不下结论）

- **契约模块（contracts）**：`messages` 频道与 `message.read` 的 asyncapi 语义、`close_codes`（缺 1013）、Action 查询保留期需要契约与实现二选一（改契约或改实现），待 contracts 分析员/人类裁定。
- **Core 模块**：`stream_snapshot` 的 health 常量、`control_permissions()` 不含 message.read、Action 查询的 owner 约定（`control/*` vs `plugin/*`）、`$control.manifests` 校验，都是 Core 侧行为；本报告只从控制面视角指出不一致，最终归属待 core 分析员确认。
- **控制面板（panel）**：默认同源 `/api/v1` 与 `{ws(s)}://location.host/ws/control`（`panel/src/main.tsx:9-11`），因此无 CORS/keep-alive 的现状与面板部署形态强耦合；面板当前未使用 Action 查询端点（panel 源码无 `actions/` 引用），与 §4 #3 的"死路径"互为印证；若面板要在面板页展示健康实时状态，需要先修 §4 #2。
- **基础设施（infrastructure）**：事件流每 100ms 的整库读取会与 SQLite 共享 RuntimeStore 互斥（`sqlite_runtime_store.cpp:229-238`），可能影响 Core 其他业务路径；`/logs` 数据源 = 存储装饰器（`core/src/runtime/runtime_log.cpp:34-56`），日志保留 10 分钟/4096 条。
- **运维（operations）**：TLS 终止、反向代理、优雅关闭、`/health/live|ready` 的部署探针依赖控制面提供回环监听与稳定错误码；`doc/16:597-601` 已承认部署骨架不可运行、T11b 未验收，因此本模块的生产可用性缺口与 operations 报告重叠。
- **CI/Toolchain**：控制面 acceptance 只在 `KIARABOT_ENABLE_INFRASTRUCTURE=ON` 且 `control/api/tests/.venv` 存在时注册（`control/api/CMakeLists.txt:13-18`），并由 `tests/acceptance/run.sh:17-18` 执行；CI 自动触发已暂停（`.github/workflows/ci.yml:3-17`），控制面测试当前不自动运行（与 ci-cd 报告同源）。

## 7. 覆盖结论

- 一句话结论：**control/api 是一层很薄、职责正确的传输层**——25 个 OpenAPI 操作全部有路由，认证/旋转/撤销/幂等/RBAC/审计/不可变分页/错误映射都在 Core 端口实现并有本机测试证据；但**事件流有 2 个声明频道不可用（messages 完全不可授权、health 永不产生事件）**，Action 查询 REST 无生产者，100ms 全量轮询存在可被认证用户放大的可用性风险，生产可用性（TLS/限流/CORS/优雅关闭/日志/追踪）几乎全靠外部或未实现。
- 覆盖度估计（口径：按契约与文档声明的行为点枚举）：
  - REST：25/25 有路由；**22/25 完整**、1 半实现（groups 恒空）、2 未接线（Action 查询）；
  - WS：7/7 握手可用；事件（类型×频道）**8/12 完整**（health 0/2、message 0/2）；
  - 认证/会话：11/11；幂等：6/6；分页：7/7；错误映射：映射表 10/10；
  - 生产可用性/可观测：**6/15**（回环监听、连接上限、body/header 上限、登录限流、慢消费者背压、容量上限为完整；其余 9 项缺或部分）；
  - 测试：5 个 pytest 文件 + 1 个 C++ 单测覆盖主流程与拒绝/依赖故障；**未覆盖全部容量/超时/事件类型/生产路径**（§4 #15）。

## 8. 未核实事项

1. **未构建、未运行任何测试**（本机无 cmake/uv/node/protoc）：所有行为判定来自静态阅读与仓库内注释/测试断言；`doc/16:546-551` 的历史通过数字未复跑，**不得据本报告宣称测试通过**。
2. **Beast 超时/保活的确切运行时行为**：`websocket::stream_base::timeout{5s, 30s, keep_alive_pings=true}`（`http_server.cpp:31-32`）在只读等待首帧之外的长期读上是否会让 30s 无数据连接被关闭（或靠 ping/pong 续命），未运行验证；若会关闭，长连空闲客户端会收到无 close 帧的断开（重连可 resume，但契约未定义 idle 关闭）。同理 `Expect: 100-continue` 由 Beast 返回何种错误、连接是否挂起未核实。
3. **远端 CI 状态**：无法访问 GitHub Actions；`doc/16:597-601` 自述历史上未通过、自动触发暂停，HEAD `8caecb1` 的修复是否绿未核实。控制面测试是否真正被 CI 执行取决于该次运行。
4. **`$control.manifests` 中插件 manifest 的完整性**：由 Core PluginHub 写入（`plugin_hub.cpp:195`），其注册校验是否保证 `name/api_version/runtime` 必填属 core 模块范围，未跨模块核实；若缺失会让 `/plugins` 违反 OpenAPI schema。
5. **`/logs` 的 operation/outcome 语义**（审计条目派生）是否满足 `doc/08:209` "结构化运行日志"的字段与用途预期，属 core/infrastructure 判定，未跨模块确认；`test_operations.py:73-84` 只断言了脱敏与少量字符串。
6. **性能量级**：§4 #4/#7 的放大倍数基于代码路径与容量上限的静态推断，无 benchmark；SQLite 实际读取代价、100ms tick 在 128/1024 会话下的退化曲线未测量。
7. **安全评估**：未做渗透/威胁建模；未核实 Origin 校验、CSRF（本项目用 bearer token 而非 cookie，风险低但无测试）、token 摘要比较非常量时间（理论上不可利用）、反向代理头部（代码不信任，已核）。
8. **测试环境依赖**：`test_tls.py` 依赖系统 `openssl` CLI（版本/开关未核实）；`test_config_delivery.py` 依赖 `sdk/python/.venv` 与生成代码（本机无 uv，未运行）；`control/api/tests/.venv` 当前不存在，故 `control_http_websocket_acceptance` CTest 目标在本机不会被注册。
