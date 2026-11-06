# Control API 模块设计（control/api）

> 目标实现为独立进程 `kiarabot-control`（组合根 `control/api/app/main.cpp`）。REST 唯一来源
> [`api/openapi.yaml`](../../api/openapi.yaml)，WS 唯一来源 [`api/asyncapi.yaml`](../../api/asyncapi.yaml)，
> owner RPC 以 `proto/kiarabot/v1/*.proto` 为准；架构总览见 [`docs/architecture.md`](../architecture.md)。
> `http_server.cpp`、`http_mapper.cpp`、`event_stream.cpp`、`configuration.cpp` 属 reference 构建
> （`control/api/reference/`），非目标行为。

## 1. 模块定位与职责

- 控制面唯一浏览器/客户端入口：HTTPS REST + JSON 与 WSS target scoped 事件流；页面在 `panel/`。
- 薄转发/投影层：接收 bearer 凭据与类型化请求，逐请求向 Identity 换一次性委托，调用
  Runtime/Scheduler/Model/Host owner RPC，把 typed 响应投影为 OpenAPI schema，不持有业务规则。
- 认证边界：`/api/v1/auth/*` 映射 Identity `Login`/`RefreshSession`/`ValidateSession`/
  `ListSessions`/`RevokeSession`；不保存用户、密码摘要或会话权威状态。
- 只读聚合：`/api/v1/logs`、`/api/v1/audit` 合并 Identity/Runtime 的 `ListLogs`/`ListAudit`；
  Control 只排序、标注 owner、透传 keyset 游标。
- 受控 bootstrap 交付：把 Runtime 一次性凭据明文写入固定目录 0600 sink（`SecretDeliveryApplication`
  /`SecretDeliveryPort`）并生成无秘密 receipt，REST/WS 不回显明文。
- 不属于本模块：业务数据库/事务、Action 执行、插件/进程启停、消息路由、模型调用与权限裁决。

## 2. 边界与依赖

- 契约：权限/分页/幂等/错误来自 `api/openapi.yaml`（`x-control-policy`、`x-permission`、
  `x-owner-rpc`）；WS 帧/close code/resync 来自 `api/asyncapi.yaml`（`x-target-transport`、
  `x-control-session-policy`、`x-target-contract`）。
- 下游 owner：`IdentityService`、`RuntimeService`、`SchedulerService`、`ModelService`、
  `HostManagementService`（`proto/kiarabot/v1/services.proto`、`host.proto`、`observations.proto`），
  一律 mTLS；`IssueDelegation` 是唯一授权载体，Control 不缓存会话/权限结论。
- 复用 Core 公共边界：`core/include/kiarabot/core/domain/target/foundation.hpp`、
  `core/src/runtime/platform/target_codec.cpp`、
  `core/include/kiarabot/core/runtime/platform/service_config.hpp`（配置/探针/启停）；不链接 reference Core。
- 上游消费者：`panel/` 及其反向代理（`deploy/docker/panel-nginx.conf.template`）；构建单元为
  `cmake/control-auth.cmake`、`cmake/control-ws.cmake`、`cmake/bootstrap-delivery.cmake`。

## 3. 进程/组件结构

- 进程：`run_service(Service::Control, compose)` 装配 `make_application(readiness)` 与
  `make_https_endpoint(config, readiness)`；匿名探针监听 `probe_listen` 与业务 HTTPS 分离。
- HTTPS（`control/api/src/target/https_endpoint.cpp`）：TLS≥1.2、单 io 线程 + 4 unary worker、
  ≤64 连接、body ≤1MiB、header ≤8KiB、TLS/读/写各 5s；入站不要求客户端证书（`verify_none`），
  出站 gRPC 才 mTLS；响应 `no-store`/`nosniff`，每连接一个请求后关闭。
- 认证与 owner 适配（`identity_auth_client.cpp`/`owner_clients.cpp`）：先交 `OwnerClients::handle`，
  否则处理 auth 族；统一 request/trace id、2s 上游预算、严格 JSON；access token 作 subject proof、
  出站证书 SHA-256 绑定，先 `IssueDelegation` 再调 owner RPC，gRPC 状态映射稳定码。
- Owner gateway（`owner_gateway.cpp` + `owner_gateway/`）：13 个 handler（health、principals、
  catalogs、operations、bootstrap、accounts、instances、tasks、audit_logs、lifecycle、models、
  actions、hosts），公共辅助在 `common.cpp`。
- WS（`websocket_endpoint.cpp`/`websocket_session.cpp`）：独立 4 worker、≤64 活跃 socket、Upgrade
  同源检查；`websocket_protocol.cpp` 路由/首帧/帧投影，`websocket_stream_store.cpp` 保留重放，
  `websocket_source_client.cpp` 负责 owner 查询；`secret_file_delivery.cpp` 实现固定目录 sink。

## 4. 数据与状态

- 权威业务数据在 owner PostgreSQL；Control 无数据库、不跨库读表、不保存成功结果缓存。
- 进程状态：`ControlReadiness` 三个原子标志（business/credentials/identity，
  `application/target/readiness.hpp`）供匿名 ready 使用；TLS 材料与 gRPC 通道仅驻留内存。
- WS 保留存储（`websocket_stream_store.hpp`）：≤1024 会话、每会话 ≤4096 事件且 ≤4MiB、全局
  ≤64MiB、保留 600s、每批重放 ≤32；淘汰产生明确 `RESYNC_REQUIRED`，进程重启丢弃会话
  （旧客户端收 `SERVER_RESTART`）。
- Bootstrap sink：固定目录 `<peer>.bootstrap`（0600、无符号链接、固定 owner）+ 无秘密
  intent/receipt；明文只在一次写入路径，不进入 REST/WS/日志/数据库。
- 游标：普通列表用 owner 的不透明 `page_token`（`page_size` 1..100，默认 20）；`/logs`、`/audit`
  用 Control 复合 keyset（`v1.`），分别携带 Identity/Runtime 的 `occurred_at:id`，不可混用。
- 幂等：写操作 `Idempotency-Key`（16..128 字节）原样写入 owner `RequestContext`；Control 不合成
  receipt、不缓存成功，同键重放与异载荷冲突由 owner 裁决。

## 5. 关键流程/状态机

- 认证：严格 JSON → `ValidateSession` →（会话类）`IssueDelegation`（`RESOURCE_PRINCIPAL`+
  `identity.session.read` 或 `RESOURCE_SESSION`+`identity.session.revoke`）→ Identity RPC → 投影；
  登录/刷新/撤销必带 `Idempotency-Key`，撤销成功 204。
- owner 请求：解析 method/path → 选资源族 → 规范化 scope（精确资源或 `RESOURCE_CATALOG/control`，
  禁止空 scope）→ 取 `x-permission` → 一次性委托 → owner RPC → typed 投影；`202` 仅表示持久受理，
  完成状态必须查 `GET /api/v1/operations/{owner}/{id}`。
- 探针：`/health/live` 恒 200；`/health/ready` 仅在 business、TLS 材料与 Control→Identity
  校验/撤销通道均就绪时 200，否则 503 且不暴露组件；平台 runner 每 1s 调 `endpoint->check()`。
- WS 建链：Upgrade 校验 GET/RFC6455/版本 13/无 body/唯一且命中 allowlist 的 Origin/socket 配额
  （超限 429）→ 5s 内收首帧（hello/resume + access_token + scope）→ `ValidateSession` 绑定
  principal/session/authorization_version → `SESSION_READY`/`OperationSessionReady` → 100ms
  轮询、≤32 条重放后实时。
- 授权刷新：每个空闲/重放/实时轮询都重新 `ValidateSession` + `IssueDelegation`；单次预算 500ms
  （快照频道 900ms），最近确认超 750ms 即停止发送并关闭，撤权/过期 1s 内关闭。
- channel：operations 用 owner `ListOperationChanges`；health/adapters/plugins 为快照频道（值签名
  忽略 observed_at、瞬时故障 2s 宽限）；logs/tasks/messages 为 v1.2 变化 feed；`/ws/control` 聚合与
  host operations 未接线，以 `CAPABILITY_NOT_SUPPORTED`→1011 失败关闭，不建立假空流。
- 恢复：`resume` 携带 session_id/last_sequence/owner_cursor；非法边界/保留丢失/进程纪元变化分别
  为 `INVALID_CURSOR`/`CURSOR_EXPIRED`/`SERVER_RESTART`，发 `RESYNC_REQUIRED` 后 close 1000。
- bootstrap：Runtime 明文由 Control 剥离并写 sink → 持久 intent → 原子安装 0600 文件 → 无秘密
  receipt → 回报 Runtime；中断后 query 只核对版本/摘要，无法确认返回 UNKNOWN，旧 worker 被 CAS 拒绝。

## 6. 接口与契约

- REST：`api/openapi.yaml` 当前 76 个 path、89 个 operation；67 个 `x-owner-rpc`，5 个 auth
  operation 为 `x-target-owner-rpc`；覆盖 auth/health/principals/accounts/bindings/installations/
  instances/tasks/occurrences/models/actions/logs/audit/operations/resources/hosts 资源族。
- 公开探针：`GET /health/live`、`GET /health/ready`（`servers: /`、`security: []`、
  `x-process-probe: true`）；其余在 `/api/v1` 下，默认 `bearerAuth`。
- 错误体：`{code, message, request_id, trace_id, retryable, details, failure_context?}`
  （`components.schemas.Error`/`ErrorCode`）；`failure_context` 保留 acceptance、operation、
  recovery、`retry_after`、`current_version`，自由文本不穿透。
- 权限：每 operation 的 `x-permission` 与 `x-control-policy.target_role_capabilities` 及 owner
  在线授权求交；`action.inspect`、`log.read`(48)、`audit.read`(49)、`health.read`(50)、
  `message.read` 必须显式授予，默认角色（含 admin）不派生。
- 分页/幂等：`page_size`/`page_token`、`Idempotency-Key`、`expected_version`（正十进制 uint64
  字符串）；target 响应为显式 typed schema，不允许原始 proto JSON 直出（`owner_gateway/` 投影）。
- WS：`api/asyncapi.yaml` 8 个 channel，首帧 hello/resume，事件 envelope 为
  `{schema_version,event_type,sequence,occurred_at,payload,owner_cursor?}`；close code 4400/4401/
  4403/1011/1013/1001，RESYNC 理由三值；target 状态以 `proto/tests/contract_status.json` 的
  `execution_scopes.target.overrides.*` 为准，`/ws/control` 仍 not_implemented。

## 7. 权限与安全

- 身份/授权：浏览器只提供 access token；Control 每次请求在线 `ValidateSession`，再委托 owner
  `CheckAuthorization`；固定角色能力与显式资源授予相交后才授权，客户端自报 actor/role/scope 拒绝。
- 委托绑定：`subject_proof`=原始 access token，`client_certificate_sha256`=Control 出站 leaf
  证书摘要，`audience`=具体 owner，`permissions` 单一权限，`scope`=精确资源（或
  `RESOURCE_CATALOG/control`）；不跨 owner 放大、不缓存决策。
- 传输：入站 TLS 终止在 Control（TLS≥1.2，无客户端证书）；出站 gRPC 强制 mTLS（CA+client
  cert/key，收发消息 1MiB 上限）；不信任任何代理头作为身份。
- WS：Origin 必须与 `websocket_origins`（HTTPS origin、去重、≤32 项）逐字匹配，缺 Origin 仅
  `websocket_allow_missing_origin=true` 时放行；token 只放首帧，禁止 query/cookie；catalog 只是
  选择器，不派生 child 授权；每资源每次委托，会话绑定 authorization_version。
- 秘密：bootstrap 明文只进 sink 文件；模型/Provider 密钥只写不读；日志/审计投影不含 token、
  proof、cursor 原文、凭据、消息正文或模型正文；错误只回稳定码。
- 边界：64 连接、1MiB body、8KiB header、≤64 请求头、64 WS socket、5s 首帧、64KiB 首帧/单帧、
  JSON 深度 8、WS 队列 128 帧/1MiB；无通用 REST 令牌桶，登录限流由 Identity 执行（30 次/用户名/
  分钟）；目标传输不实现 CORS，生产要求同源反代。

## 8. 错误/失败/恢复语义

- owner gRPC→稳定码（`owner_clients.cpp`）：`UNAUTHENTICATED`→401、`PERMISSION_DENIED`→403、
  `INVALID_ARGUMENT`→400、`NOT_FOUND`→404、`ABORTED`→`CONFLICT`→409、`RESOURCE_EXHAUSTED`→
  `RATE_LIMITED`→429、`DEADLINE_EXCEEDED`→`TIMEOUT`→504、`UNIMPLEMENTED`/`UNAVAILABLE`→503、
  `INTERNAL`→500；未知稳定码/畸形 details 一律 500，不伪造 acceptance。
- 参数/协议：重复或未知 JSON 键、非法 scope/分页/时间/游标、path 与 body ID 不一致→400；未知
  路由→404；已知路径错误方法→405（不保证 `Allow`）；`HEAD` 在 handler 层按 GET 处理。
- 缺失依赖：owner endpoint 未配置/能力未实现→503，不得返回成功空页；查询超时→504，且不得推断
  Action/模型调用失败或写操作回滚；owner 不可用不得回退缓存/旧快照。
- 429：auth 边界补 `Retry-After`；owner gateway 直接映射的 429 仅带稳定 body（`retry_after` 进
  `failure_context`，不保证响应头）。
- WS：首帧/scope/owner 选择器畸形 4400；会话无效/过期 4401；授权拒绝或 principal/channel/
  owner/scope 绑定不一致 4403；Identity 无法在新鲜度预算内确认、owner TIMEOUT/UNAVAILABLE、
  畸形上游或未实现源 1011；慢消费者/限流 1013；停机 1001；正常 resync 关闭 1000。
- 进程 lifecycle：启动参数/配置非法 exit 2，依赖初始化/监听失败 exit 1，运行时 endpoint lost 亦
  exit 1；SIGTERM/SIGINT 停止 accept、取消在途上游、向每个 WS 发 1001、≤2s 排空后 join，exit 0。
- bootstrap：sink 已写但 report/ACK 丢失时 PENDING/UNKNOWN，查询与同键重放可恢复；显式 reissue
  推进版本撤销旧凭据，迟到旧 worker 被 `expected_version` CAS 拒绝；冻结契约无 delivery cancel 入口。

## 9. 可观测性与运维

- 启动日志：公共 runner 输出 `service=control probe_port=<n> grpc_port=0 ... ready=<bool>` 与
  `control_https_port=<n>`；失败输出 `operation=start outcome=...` 与稳定 `reason=`，不含秘密。
- 健康：部署探针走独立 `probe_listen` 的 `/health/live`、`/health/ready`；
  `deploy/compose.core.yml` 的 healthcheck 为 `curl http://127.0.0.1:9080/health/ready`，业务端口
  8080 仅发布到 127.0.0.1；panel nginx 代理 `/api/`、`/ws/`、`/health/ready` 到 Control。
- 配置：`kiarabot-control --config <json>`；`control_api` 必填 `listen`、`identity_endpoint`，可选
  runtime/scheduler/model/host endpoint 或互斥 `host_endpoints`（1..1024 项离线注册表）、
  `websocket_origins`/`websocket_allow_missing_origin`/`secret_delivery_recipients`；未知键/坏端点/
  同时给单 host 与注册表均启动失败（`core/src/runtime/platform/service_config.cpp`）。
- 部署：control 以非 root（10001:10001）、只读根文件系统、`cap_drop: ALL`、`no-new-privileges`、
  1 CPU/512M 上限、`stop_grace_period: 20s` 运行；TLS/配置只读挂载；bootstrap 目录仅 Control 可写。
- 可观测缺口：无每请求访问日志、指标或 trace 头回传；request_id/trace_id 仅在错误体出现
  （`x-request-id` 长度 8..128 时采用，`traceparent` 合法时取其中 trace-id），WS 无事件级日志。

## 10. 测试与验收证据

- 单元：`p12_control_owner_gateway`（`control/api/tests/owner_gateway_test.cpp`：13 族路由/HEAD/
  405/委托/分页/错误映射/bootstrap 脱敏）、`p12_target_websocket_unit`/`_store`/`_schema`
  （`target_websocket_protocol_test.cpp`/`target_websocket_stream_store_test.cpp`/
  `target_websocket_schema.cjs` 真实 OpenAPI+AsyncAPI Ajv）、`p12_secret_delivery`
  （`secret_delivery_test.cpp` fork 崩溃 phase）。
- 真实 Control + TLS 夹具（`control/api/tests/architecture/`）：`p04_control_auth_https`、
  `p11_control_model_rest`、`p12_audit_logs_rest`、`p10_scheduler_rest`、`p12_host_control_rest`、
  `p12_target_websocket_https`、`p12_target_websocket_producers`（脚本为同目录同名 `.py`）。
- 真实多进程 e2e（`tests/architecture/`）：`p12_host_control_chain_e2e`、`p12_host_real_chain_e2e`、
  `p12_host_dual_host_chain_e2e`、`p12_bootstrap_delivery_e2e`、`p12_ws_owner_directory_e2e`、
  `p12_ws_owner_directory_scale_e2e`、`p04_identity_log_feed_e2e`、`p05v12_owner_feed_e2e`、
  `p12j_panel_live`。
- 门禁与边界：`proto/tests/target_api_test.rb`、`check_implementation_status`、
  `proto/tests/evidence_scopes_test.py`、OpenAPI/AsyncAPI Ajv 漂移检查（`scripts/quality/`、
  `panel/scripts/check-contracts.mjs`）；fixture owner 测试只证明 Control 传输/委托/投影，全栈证据
  以上列 e2e 与 `proto/tests/contract_status.json` 的 evidence 为准。

## 11. 已知限制与后续

- `/ws/control` 聚合与 `/ws/operations?owner_service=host` 仍 not_implemented/未接线，producer 缺失
  时认证后 1011 关闭；客户端应以 REST 权威快照恢复，不能把缺失流当空数据。
- `/ws/health` 的 producer 只校验 Identity 会话与 owner gRPC channel READY，未见对 `health.read`
  的逐次委托校验；契约声明需要该权限，建议后续补齐或澄清。
- 目标 REST 无通用限流/每主体配额、无 CORS/OPTIONS 支持，owner 路由 405 不带 `Allow`（仅 auth
  路径带）；仅有连接数、body/header/WS 帧边界与 Identity 登录限流，生产依赖同源反代与外部限流。
- 无请求级访问日志/指标/trace 头回传；`x-request-id`（8..128）与 `traceparent` 只用于错误体
  request_id/trace_id，`X-Trace-ID` 未采用。
- `GET /health/ready` 的 OpenAPI 描述仍保留 reference“自身存储写探针”措辞，目标实现以 readiness
  端口（business/credentials/identity）为准，契约描述待对齐。
- 冻结 bootstrap 契约无 delivery cancel 入口，只能靠显式 reissue、同键重放与状态查询收敛；
  reference 路径（同进程 `IControlApplication`/SQLite/reference WS）保留但带 `reference` label，
  不能作为目标进程验收证据。
