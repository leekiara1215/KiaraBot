# 目标 Control WebSocket 可实施计划（2026-11-04）

## 0. 范围、状态与文档影响判断

本报告为用户批准纳入实施计划的 Control WS 工作流设计，不是实现完成声明，也不是现行契约。主代理统筹契约冻结、分派和验收；本轮只新增本报告，不修改产品源码、共享协议或既有任务差异。不得据此继续沿用“目标 WS 已延期”作为交付目标；最终交付必须有 target 服务端、权威事件源、Panel 恢复和故障验收。

目标模块为 `control/api/`、`panel/`，直接前置涉及 owner Core 应用/端口、PostgreSQL 实现与 `proto/`。Control 不拥有业务库，Panel 不裁决业务状态。契约唯一来源分别为 `api/asyncapi.yaml`（WS）、`api/openapi.yaml`（REST 快照和复用类型）、`proto/kiarabot/v1/`（owner RPC）；`proto/tests/contract_status.json` 记录执行作用域和证据，不能替代线协议。

实施文档必须同步：`doc/06-control-api.md`、`doc/07-control-panel.md`、`doc/01-runtime-semantics.md`、`doc/01-contracts.md`、`proto/README.md`；新增 owner 用例/持久日志涉及 `doc/02-core.md`、`doc/05-infrastructure.md`、`doc/22-transactions-and-migration-design.md`；连接额度、Origin、ready/撤销时限、关闭/重启恢复涉及 `doc/08-operations.md`；新增测试/构建目标涉及 `doc/11-toolchain.md`；实测写 `doc/27-phase-exit-evidence.md` 第 0 节。只更新直接描述新增行为的章节，不重写历史证据。

`doc/00-boundaries.md` 现有依赖边界适用，无需改；`doc/03-plugin-sdk.md`、`doc/04-nonebot-adapter.md` 无 WS 客户端或插件/适配器命令变化，无需改（后续消息事件源如果扩公共接口需重新判断）。`doc/12-ci-cd.md` 仅在实际新增 CI gate 时更新。`doc/18-architecture-baseline.md` 及 AGENTS 保护清单只读，任何差异只报告；不以“同步文档”改写。此轮没有修改上述文档。

## 1. 已核实的当前实现与缺口

- 已读 `AGENTS.md`、C++ 主规范、边界、Control/Panel/工具链、AsyncAPI、`doc/27` 当前入口；核对当前未提交重构，不 reset/revert/stash，不生成混合提交。
- 图项目 `Users-betta-Workspace-KiaraBot`，generation `2026-11-04T05:19:50Z`，ready，37 partial/55 excluded。精确符号查询返回 reference EventStream；coverage 确认 `control/api/src/target/` 和 `core/src/application/target/` 整子树 excluded/freshness=not_tracked，不能据图空结果宣称无实现。本报告对 target 结论来自源码回读；Panel/AsyncAPI 覆盖 metadata_match 仍仅为 best effort。
- `control/api/src/target/https_endpoint.cpp` 的 Session 只 TLS handshake → HTTP read → worker `IdentityAuthClient::handle` → HTTP write → shutdown。没有 WebSocket Upgrade。单 HTTP body 1 MiB、header 8192 字节、64 连接、4 worker 的现值不等于长期 WS 的安全并发预算。
- `control/api/src/target/identity_auth_client.cpp` 有真实 mTLS Identity 调用、两秒总 REST 预算和取消管理；其 HTTP API 未提供适合 WS 的当前会话/精确 scope 授权接口。`owner_clients.cpp` 有 ValidateSession、IssueDelegation 和 owner 调用，不能用 display_permissions 替代逐 scope 在线授权。
- `owner_gateway/operations.cpp` 只接受 runtime/model 的 `resource.read`、scheduler 的 `task.read`，逐请求精确委托后 GetOperation。当前没有 List/WatchOperations。`runtime.proto`、`scheduler.proto`、`model.proto` 的相关 RPC 与 Core Repository 目前提供按 ID Get/find，不能由 Control 临时收集本进程写返回的 operation ref 冒充 scope 的完整事件源。
- `owner_gateway/health.cpp` 和 `owner_clients.cpp::observe` 已有四依赖健康聚合，当前来源是 gRPC channel readiness。Identity 不可确认时不向未授权连接发送组件详情；Runtime/Scheduler/Model 单点故障按现有聚合为 DEGRADED。AsyncAPI 中“固定 HEALTHY”描述已落后，实施时应同步修正，不能把 WS 另造为深度业务健康探针。
- `panel/src/App.tsx` 登录后建立全局 `/ws/control`；首个 SESSION_READY 前阻挡 REST，失败后才能进入 REST fallback。`control-stream.ts` 已有 hello/resume、十六秒上限退避、4401/4403/4400/1013 终止处理、RESYNC_REQUIRED；缺 operations scope/owner_cursor，token 轮换会销毁流对象丢失会话。`p12-live.tsx::OperationTracker` 每两秒单独 REST 轮询，错误后不再安排下一次；恢复必须接入 tracker，而不能只刷新全局 revision。
- target adapter 是 `AdapterRecord`（online/lifecycle/fence/last_registration），target instance 是 `InstanceRecord.state` 的 Runtime 期望、Host 观察、Plugin 配置应用三层事实。AsyncAPI 的旧 AdapterPayload 健康枚举、PluginPayload 单一 PluginState 不能无损表达它们；不能把 online/desired 简化为 HEALTHY/RUNNING。
- reference `control/api/src/event_stream.cpp` 依赖旧统一快照，并不属于 target 接线。仅可复用协议测试场景，不能把其构建或旧 `control_http_websocket_acceptance` 通过作为 target 验收。

## 2. 推荐冻结的接口设计

以下是交给主代理统一登记的推荐方案，冻结前保留“拟”字样；不把报告中的名称当作已经存在的 API。

### 2.1 频道、scope 与 owner

- 保留 `/ws/control`、`/ws/health`、`/ws/adapters`、`/ws/plugins` 及已声明事件名。operations 用 `/ws/operations?owner_service=runtime|scheduler|model`；host 在 HOST 工作流完成后加入。URL 只放非秘密 owner 选择，access token 只在首帧，拒绝 query token/cookie 自动认证。一个 operations 会话只绑定一个 owner，因此复用现有单值 `owner_cursor`，不引入伪全局 cursor 或跨 owner 原子快照。
- operations 保留 OperationHello 的 scope；OperationResume 的 owner_cursor 必须同 owner/scope。`OperationReady` 拟改为独立 `OperationSessionReady`：保留 SESSION_READY 三字段，额外必带初始 `owner_cursor`。这样 hello 后尚无事件就断线仍有可恢复游标。现有 SessionReady schema 不增加必填项，其他频道帧不受影响。
- scope 是明确资源集合；首版每条 operations/adapter/plugin scoped 会话上限 127 资源，规范化排序/拒绝重复，不接受空 scope、通配符或以 catalog grant 推导 child grant。adapter/plugin 的 Hello/Resume 拟采用额外 scope 的专用 schema，scope 在首帧传输，避免 URL 长度与访问日志暴露资源选择。超过上限返回稳定限流语义，不截断。
- `/ws/control` 是当前合法频道的并集，不暗含每个 owner/scope 的所有资源。首版用它承载已授权健康和显式 target 状态订阅；精确订阅通过专用频道。其是否携带 target scoped 子订阅需主代理在 WS-01 冻结，推荐首版不把 operations 隐式并入 control，以免改动既有 Hello。未完成频道仍登记 target not_implemented，明确返回受控拒绝，不能空流假成功。
- 权限跟 owner REST 一致：runtime/model operation=`resource.read`，scheduler operation=`task.read`；host operation 由 HOST 契约给出，不能先定成 resource.read。health=`health.read`、adapter=`adapter.read`、plugin=`plugin.read`、task=`task.read`、log=`log.read`、message=`message.read`，均重新核验精确 scope。当前 AsyncAPI operations 全部 resource.read 的单值声明须在契约中修正，不能削弱 scheduler 权限。

### 2.2 权威 owner 增量前置

拟新增三个 RPC：`RuntimeService.ListOperationChanges`、`SchedulerService.ListOperationChanges`、`ModelService.ListOperationChanges`，分别写入 `runtime.proto`、`scheduler.proto`、`model.proto`；共用请求/响应值类型放 `platform.proto` 或独立 `observations.proto`（推荐独立文件以控制共享冲突）。Host 对应 RPC 仅在独立 HOST 契约冻结后新增，服务 owner 不移交 Control。

拟请求：`RequestContext`、明确 `ResourceScope`、可选 `StreamCursor`、有界 `page_size`（默认 32，最大 100）；上下文的委托为 Control 平台 mTLS 身份携带当前人类会话 proof 签发，每请求重新授权。响应包含已脱敏 Operation 列表、对应 cursor、next_cursor、has_more、checked_at。每项必须带自身 owner sequence，而不是整批统一 cursor，避免 Panel 仅处理部分批次后错过事件。没有 cursor 的请求只捕获并返回当前 head checkpoint，不把所有历史进度当“新事件”；Control 先建立流再让 Panel 拉 REST 快照。

Owner 本地持久 change journal 与 operation/participants 变更在同一 UoW 提交：业务 rollback 不产生通知，提交未知按既有 operation 身份恢复；不能在事务后尽力 append。枚举所有写入口，包括 common_work、record_codec、lifecycle participants、Model 管理及新 streaming 更新，统一经新端口 append，防止只改主要 save 路径漏事件。持久记录仅保存 owner/operation ID、资源、版本、owner sequence、提交时间及最小安全状态投影；不保存配置/凭据/模型输出。通知表达当前进度，可合并中间状态，不是审计，不保证逐个过渡都可见；participants-only 变化也必须推进可观测修订。

排序不能直接用 PostgreSQL nextval 当作已提交高水位（并发事务会先取得序号后提交）。推荐每 owner 独立短事务计数行/有序 append，行锁在同一 UoW 随提交释放，保证可见序号按提交推进；批内有稳定顺序。评估锁顺序并提供两事务屏障测试，禁止内层直接 SQL。Journal 保留至少 600 秒，容量设硬上限，删除旧数据同时持久推进 low-water mark；容量提前压缩可触发明确 resync，不能静默跨 gap。

owner epoch 表示持久数据集 incarnation，普通进程重启继续同 epoch/sequence；恢复旧备份或重建日志必须更换 epoch，旧 cursor 失效。Control 自身 stream epoch 每次启动变化，内存 session 不跨进程恢复。scope 的规范形式、owner、subject/session 与 authorization_version 绑定 cursor；任一变化必须重授权且不复用旧 cursor。

新增纯 Core 查询用例/端口不依赖 wire/JSON/SQL；PostgreSQL 实现持有权威表和时钟。Control 使用有界 unary 增量拉取（目标 100ms 检查、慢源不重叠调用、每 source 一个 in-flight），避免引入无法在一秒内撤权的长 gRPC stream。空页只有 owner 成功确认才能作为合法“无变化”，依赖故障不能伪造空页。

### 2.3 target 状态 payload 与其它频道分批交付

首批必须交付 operation.updated、health.updated、adapter.updated、plugin.updated。保留 envelope/schema_version/事件名称；对 adapter/plugin 的 target payload 以显式 `target` 子对象引用新的安全 target 状态 schema，并扩展 payload 分支，保留 reference 旧 schema 的作用域。目标通知只表达资源 ID、版本、online/lifecycle/注册元数据或实例三层状态，绝不伪造旧健康/PluginState 必填值。契约改变须在 WS-01 显式冻结、做旧消费者忽略新字段测试；若不能做到兼容，明确版本化 target payload，而不是在代码中偷偷改旧字段含义。`plugin.updated.health_state` 继续独立待实现，直到 owner 真正提供该健康事实；配置 ACK 不等于插件健康。

第二批 WS-07：task.updated 由 Scheduler 当前授权任务状态增量产生；log.updated 由 Identity/Runtime 已脱敏日志增量产生，使用与 REST 相同 LogEntry，复合 owner cursor 内部独立维护，不把一个 owner cursor 传给两个源。不新增 audit 频道；日志和审计权限互不蕴含。当前只有倒序分页 ListLogs，必须先补安全的“从 checkpoint 向前变化”查询/端口或等价权威头水位机制，不能只看第一页就假定没有遗漏。

第三批 WS-08：message.updated 从 Runtime 已持久接受消息的 metadata 投影产生，仅 message_id/conversation_id/direction=received，无正文、平台原始包、sent 推断。需要显式 message.read 精确资源授权；不沿用 reference 私有配置赋权。没有 REST 消息历史投影，RESYNC_REQUIRED 后只显示通知可能有缺口，不能“重建历史”或假全量恢复。此批验收完成前 target messages 保持未实现；其余频道已上线的事实不自动翻转它。第二/三批都是本计划代码任务，不因首版可用而悄悄删除。

## 3. 会话、恢复、错误与资源约束

### 3.1 TLS、认证与撤销

生产仅 WSS，沿用真实 target TLS/外呼 mTLS，不链接 reference runtime。Upgrade 只接受已登记 GET 路由、合法 WS 版本、受限 headers 和配置允许的 Origin；默认同源，非浏览器无 Origin 的策略由部署配置明确，不引入公网通配。HTTP 路由非法返回既有结构化 400/404/405；Upgrade 后首帧 5 秒截止、64 KiB 限制，只接受 text hello/resume，拒绝重复认证和 binary 数据帧（关闭 4400）。

握手时在线 ValidateSession，并用 owner 的既有精确授权过程确认频道/scope；不能仅看角色/展示权限。每 250ms 启动或共享当前 Identity 检查，单检查 500ms 总预算；距离最近成功确认超过 750ms 就撤销发送资格，安排关闭，撤权/到期后一秒内关闭。每条待发送通知（包括 replay 和已排队消息）都在发送前确认授权新鲜性；scope/权限版本改变先作废旧 replay/queue，再关闭或 resync，不能泄漏旧权限数据。

失效/到期 4401；当前授权拒绝 4403；Identity 无法确认、malformed upstream、内部故障 1011，并降低 readiness；不要把依赖故障误报“密码错误”。撤销发生在 idle socket、replay、慢写、worker 返回前同样受限时规则。不要把最慢两秒 REST 任务排在同一无界队列阻塞检查；WS auth 与数据 RPC 使用独立有界执行预算、共享同一会话检查，额度按可满足时限的实际容量接纳。连接总额、WS 额和 REST 预留额分别限流，真实压力测试证明 1 秒边界，不能仅靠定时器名声称达标。

### 3.2 sequence、cursor 与 replay

- `sequence` 是当前 Control 授权会话内单调序号，限定 JS safe integer；`owner_cursor.sequence` 是十进制 uint64 字符串，Panel 用 bigint/字符串比较。两者用途不同，不能互换。session_id 由 CSPRNG 产生至少 32 字符能力串，绑定 principal、Identity session、channel、owner、规范 scope 和权限版本。
- 保持 600 秒 replay、每 stream 4096 retained notifications、最多 1024 retained sessions；同会话只能一个 active socket。服务端先 SESSION_READY（接受的客户端 sequence），再 32 条一批 replay，最后 live；replay 期间新增事件继续入队且保持序。
- resume 重验当前会话/scope；换用户/Identity session/channel/owner 或 scope 以 4403 拒绝。未知/未来 sequence、owner cursor 与本会话已确认边界不对应，为 `RESYNC_REQUIRED: INVALID_CURSOR`；超出保留窗口/low-water 为 CURSOR_EXPIRED；Control 重启、owner epoch 变更为 SERVER_RESTART。发帧后 1000 正常关闭。把 reason 限成上述稳定枚举，不依赖自由文本。
- Owner 增量错误沿用完整 RpcFailure：INVALID_ARGUMENT→无效输入，CONFLICT + Recovery.ReadCurrent→cursor gap/失效并映射上述 RESYNC_REQUIRED；UNAUTHENTICATED/PERMISSION_DENIED→4401/4403；TIMEOUT/UPSTREAM_UNAVAILABLE→1011（客户端有界退避、REST 显示真实失败），RATE_LIMITED→1013；不为 cursor 新发明不一致 ErrorCode。unknown/malformed failure 1011，保守不发送数据。
- Control 重启丢内存 stream，客户端旧 resume 得 SERVER_RESTART；owner 重启但持久数据完整不伪造新 journal epoch。进程停止取消 RPC/定时器，限时 drain/关闭，所有回调明确所有权和 strand 线程归属。

### 3.3 慢消费者、日志与秘密

Replay 存储与网络待写队列分别有额度，推荐每 socket 待写至多 128 帧或 1 MiB（先到为准），每帧至多 64 KiB，并设有限写截止；超额 close 1013，不静默丢帧、不无限累积。会话总保留另设字节上限（4096 只是事件数量上限）；触发淘汰的 session 下次 resume 得 CURSOR_EXPIRED。单一 writer，读写分别 strand 串行，绝不持锁做网络/RPC/回调；新事件不能越过 replay。1013 是现有 Panel 终止状态，保留显式手动恢复，不自动快速循环。

只记录 request_id/trace_id、非秘密连接关联 ID、channel、owner、关闭码/稳定原因、数量/延迟；token、refresh token、原始首帧、session_id、委托 proof、bootstrap、游标原文、配置、消息正文、Provider 文本均不写日志/指标标签/错误文本。Operation reason 使用稳定 code 与批准的闭集消息，不复制 owner 自由错误字符串。日志通知只用 REST 已脱敏字段，禁止通过健康 reason/operation details 绕开日志秘密限制。Token 和流能力只在进程/浏览器内存，销毁会话时释放；不加入 localStorage、URL 或持久 Control DB。

### 3.4 Panel 恢复

按当前页面/operation ref 和 scope 建立必要频道，流状态与 REST 可用性分开。首帧未就绪、4403/1013 或流不可达都允许按 REST 自身权限展示快照，并标注“实时不可用/最后确认时间”，不再因 WS 权限阻止全部 REST 页面。4401 清空会话回登录。

成功 SESSION_READY 后先记 cursor/sequence，再拉权威 REST；读期间事件置 dirty，读完成后刷新受影响资源，使用请求 generation 防止晚到旧快照覆盖新视图。普通断线保留已处理 cursor，resume 完成再刷新；RESYNC_REQUIRED 清除旧流能力，hello 新会话并全量刷新当前可见资源和正在跟踪的 operation。operation.updated 用来触发当前 owner GetOperation 验证，HTTP 200 不是完成；BLOCKED/UNKNOWN 保持未知/阻塞，不变成失败或自动重做写请求。其它 owner 不受单一源故障伪全局清空。

Token 轮换更新 token provider，保留同 Identity session 的流 cursor；通过重新认证的新 socket resume，不继续用旧 access token。停止与重建清理 timer/handler，旧 socket 消息不能推进新 scope/新身份的 cursor。未知 event_type/字段允许前向兼容，但 malformed envelope、乱序/重复/跳序要明确处理：重复去重，跳序进入恢复，不把 session-ready 的 server head 当作客户端已处理 sequence。

## 4. 可实施任务与依赖

每项遵守上节公共输入输出、稳定失败与线程语义；禁止修改受保护文档、reference 实现及其它工作流内部代码。下面路径是允许范围，不授权同时覆盖既有未提交修改。新增文件优先独立组件；共享文件由主代理单人集成。

### WS-01 — 契约与接口冻结（主代理/契约 owner）

依赖：无；必须先与 MODEL streaming、HOST 独立 operation/bootstrap 工作流对齐。允许 `api/asyncapi.yaml`、必要的 `api/openapi.yaml` 复用状态类型、`proto/kiarabot/v1/{runtime,scheduler,model,platform}.proto` 或新 observations.proto、`proto/tests/{target_spec,contract_status}.json` 和精确契约测试；相关文档见第 0 节。输入本报告未定点；输出明确 schema、RPC/权限元数据、owner cursor 保留/错误及 target/ref 状态分离。尚无实测时不翻 implemented。验收为 descriptor/目标 schema/AsyncAPI/类型生成通过且无权限降级。

### WS-02 — owner 变更日志与纯查询用例

依赖 WS-01。允许新 `core/include/kiarabot/core/{domain,application,ports}/target/operation_observation*.hpp`、对应 `core/src/application/target/operation_observation*.cpp`、新 `infrastructure/postgres/src/operation_observations.*`、`schema/operation_observations.hpp` 及所属单测；共享 `unit_of_work.hpp`、`common_work.cpp`、`record_codec.cpp`、schema manifest/services 由主代理合并。输入 operation/participants 的同 UoW 变更和已授权 scope/cursor；输出持久有界增量/高低水位。验收真实 PG 原子性、并发提交顺序、rollback、不重复、不漏记录、scope/过期/容量/数据集重建和重启，失败不能伪空页。

### WS-03 — Runtime/Scheduler/Model owner RPC 适配

依赖 WS-01/02。按服务拆子任务 WS-03R/S/M；允许对应纯应用头/用例、新 RPC adapter、`core/src/runtime/platform/{runtime_rpc,scheduler_rpc,model_rpc}.cpp` 必要接线及各自测试、组合根；构建 CMake 由主代理串行集成。输入当前人类 proof/一次性委托、明确 scope/cursor；输出安全 operation progress。RPC 每次按目标 descriptor 权限在线核验、资源从真实 OperationRef 验证，不信请求自报资源。Scheduler 用 task.read；Model 与 MODEL 工作流共同确认新增 streaming 状态提交都会产出通知。验收真实 Identity+owner mTLS/PG 的允许、拒绝、撤权、超时、未知/未来游标及一秒关闭所需截止预算。

### WS-04 — target WSS 传输与认证会话

依赖 WS-01；可与 WS-02/03 并行用契约 test double。独占 `control/api/src/target/https_endpoint.cpp`、新增 `websocket_{endpoint,session,auth_client,stream_store}.*` 及对应 `control/api/include/.../target/`、WS 专属测试。允许 `identity_auth_client`/owner_clients 的最小抽出/注入接口，避免同一份鉴权实现复制；必要改动先向主代理登记。输入 Upgrade/hello/resume 与 auth/source port；输出 SESSION_READY/replay/live/resync/close。验收真实 WSS Upgrade、Origin、首帧期限、frame/队列/容量限制、idle/replay/slow-write 撤权、独立 REST 预算、shutdown；测试名称必须 target 专属。

### WS-05 — 首批 producer 接线

依赖 WS-02/03/04。允许新增 `control/api/src/target/{operation,health,runtime_status}_source.*` 及 target source port 测试；只通过 owner RPC/既有健康 port，不直接 include Infrastructure/PG。Runtime adapter/instance 状态先按精确授权 REST 等价 owner snapshot 周期比较，100ms 可合并状态，错误保持 unknown/stale 并断流恢复；涉及完整目录扫描必须用 immutable page 并在线逐页授权，不能靠一个 page 漏掉其余资源。operation 使用持久 owner 增量。输出首批四事件与严格 projection。验收 begin stream→REST 读取期间变化不丢、Host UNKNOWN/配置 pending 不变 running/healthy、channel 故障隔离、未授权无事件。

### WS-06 — Panel scoped 订阅与恢复

依赖 WS-01/04，可先 mock；真实验收依赖 WS-05。WS 工作流独占 `panel/src/stream/`、`App.tsx`、`views/p12-live.tsx`、必要 `api/client.ts`、`mock/server.ts`、`scripts/{generate-asyncapi,check-contracts}.mjs`、对应 panel tests；MODEL/HOST UI 需求通过接口协作，不能并发改这些文件。输入 token provider、channel/owner/scope、stream messages；输出可见实时状态、owner REST 刷新、可靠恢复。验收轮换保 cursor、首次事件前断线、scope 切换、replay/重复/跳序、重启/过期 resync、snapshot race、4401/4403/1013、REST fallback、operation tracker 恢复后继续查询且不重发管理写。

WS-06恢复验收还包括真实离线数据集恢复入口：普通PG/SQLite进程重启保留epoch；恢复旧备份、
rebuild journal或reset sequence时必须在受控restore工具/hook实际rotate dataset epoch，不能只提供
局部rotate port或文档约定。由主代理后续分配`scripts/ops`锁，owned fixture执行backup→restore→旧
cursor确定typed gap，保留备份manifest的实际schema版本；SQLite同型验收由Host owner执行。

### WS-07 — tasks/logs 频道完整纳入

依赖 WS-01~06。允许 Scheduler/Identity/Runtime 专属新增 observation 用例/端口、proto owner RPC、日志安全投影与 WS source；共享契约/日志持久化路径主代理串行分配。输入当前授权 scope 与 source checkpoint；输出 task.updated/log.updated。验收分页跨头部写入、两日志源推进不互相跳过、partial source 故障明确失败、已脱敏日志全链路、audit.read 不派生 log.read。不能仅更新 KNOWN_EVENTS 就翻频道可用。

### WS-08 — messages metadata 频道纳入

依赖 WS-01~06，可在 WS-07 后或有独立事件源文件时并行。允许 Runtime message metadata observation 新端口/投影、必要 proto、WS source 和专属测试。输入真实已接受 message event、当前 exact message.read；输出 received metadata only。验收未接受/重复事件无伪新通知、消息正文和秘密不进入存储/通知/日志、恢复缺口不伪历史、sent 仍不生成。明确 owner 源完成后才能翻 target messages。

### WS-09 — Host owner 集成

依赖 HOST 工作流的独立 operation 契约/持久查询完成，及 WS-01/04/06；不阻塞 runtime/scheduler/model 首批开发。允许新增 host operation source adapter/测试；`owner_gateway/operations.cpp`、owner 枚举、codec 的 host 分支由 HOST owner 负责，WS 不并发编辑。输入 HOST 真实 ref/scope/authorization/cursor；输出同名 operation.updated 的 host 分支；禁止 Runtime/Control 假造 host operation，bootstrap 明文永不进事件。验收独立 Host 重启/SQLite journal 恢复、跨 owner ID 错用拒绝、运行未知不成功。

### WS-10 — 集成、部署与证据收口

依赖 WS-01~09 的各批实际范围；每批可单独实测，但全部任务未完成不能称已声明频道齐备。允许 WS 专属 `control/api/tests/architecture/target_websocket_test.py`、`tests/architecture/p12_target_websocket*.py`、新增 panel target WSS 测试、`cmake/control-auth.cmake` 与必要 CMake/CI 注册（主代理）、`doc/27` 与模块文档。输入新二进制、真实 PG/Identity/Runtime/Scheduler/Model（Host 随 HOST 交付）、真实浏览器 TLS；输出可复现命令/退出码/日志和 scope 明确的证据。先本机真实进程，再目标 Compose；真实付费 Provider/QQ 不是 WS 传输验收依赖，不用 fixture 成功替代真实 owner 撤权与持久性。

## 5. 并行文件归属与主代理集成顺序

契约/文档 owner 先串行 WS-01，保管全部 api/proto registry/target_spec、共享 CMake 和 doc/27。WS C++ transport worker 独占 WS-04/05 的新 Control WS 文件及 https_endpoint，Panel worker 独占 WS-06；owner feed worker 执行 WS-02 后按服务推进 WS-03。可用槽位有限时先 transport 与 owner feed 并行，Panel 在 schema 冻结后启动；不要为“并行”同时编辑公共文件。

与其余四工作流的冲突点：MODEL owns model_gateway/model_rpc/Model state 和 provider；WS 只要求在每次持久 operation 修订中产生 feed，统一端口由 WS-02 owner提供，Model owner接入。HOST owns host operation/codec/REST owner枚举；WS-09 消费其结果。Host 请求错误语义影响既有 RpcFailure，WS 复用最终稳定映射不另创错误；bootstrap owns create/bind response、凭据传递，不允许其秘密进入事件。`common_work.cpp`/`record_codec.cpp`/`unit_of_work.hpp`/Core CMake 为共享瓶颈，由主代理合并最小 hooks，新实现独立文件。禁止并发 configure/build `build/full-locked`，重型 CTest 必须主代理发放串行窗口。

## 6. 构建、校验、测试与验收命令

使用 `doc/11` 已锁定依赖：C++20、PG16.4、OpenSSL3.5、gRPC1.71/Protobuf5.29.3、Boost1.87、Node22.14、Python3.11.18；不新增第三方 WS/JWT 库或升级锁。下面新增测试/目标名为计划登记名，实施后先检查它们实际存在且未禁用；show-only 本身不算通过。

契约阶段（修改 proto 时必须跑完整 descriptor/目标镜像检查）：

```bash
./scripts/quality/check-asyncapi.sh
./scripts/quality/check-openapi.sh
./scripts/quality/check-target-api.sh
./proto/tests/check_contracts.sh
python3 proto/tests/check_implementation_status.py
npm --prefix panel run generate
npm --prefix panel run check:contracts
```

由主代理在独占构建窗口执行：

```bash
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset full-locked --target kiarabot-control kiarabot-runtime kiarabot-scheduler kiarabot-model --parallel 4
ctest --preset full-locked -R '^(p12_target_websocket_unit|p12_target_websocket_https|p12_owner_operation_changes|p12_target_websocket_recovery)$' --no-tests=error --output-on-failure
ctest --preset full-locked -R '^(p04_control_auth_https|p12_control_owner_gateway|p10_scheduler_rest|p11_control_model_rest|p12_audit_logs_rest|p06_postgres_runtime_events)$' --no-tests=error --output-on-failure
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/check-architecture.py --inner
python3 scripts/quality/check-architecture.py --links build/full-locked/architecture-targets.json
python3 scripts/quality/check-architecture.py --database build/full-locked/compile_commands.json
./scripts/quality/check-format.sh
KIARABOT_TIDY_BUILD_DIR=build/full-locked ./scripts/quality/check-tidy.sh
git diff --check
```

实际新增单测可编译为独立 `kiarabot_control_target_websocket_test`、`kiarabot_owner_operation_changes_test`；WS-10 必须在 CMake 显式注册以上 CTest，不能拿 0 tests 掩盖缺少目标。改动文件先用固定 clang-format --dry-run --Werror 与对应编译数据库的 clang-tidy 单 TU 验证；全仓格式/tidy 的既有失败逐项和 `doc/27` 基线比较，不删除断言/放宽规则。最终重建后由主代理运行一次完整 `ctest --preset full-locked --no-tests=error --output-on-failure`；不能复用本轮之前 167/167 声称新增 WS 验收。

Panel（固定依赖已存在时不重复安装；缺失才 npm ci）：

```bash
npm --prefix panel run lint
npm --prefix panel test
npm --prefix panel run build
```

增加真正 target WSS 的 live 测试，不允许仅运行历史 reference fixture 的 `panel/tests/live.test.ts`。目标 Compose 验收需在运行配置冻结后给出确切入口/参数并归档（拟 `tests/architecture/p12_target_websocket_smoke.py --base-url ... --ca-file ... --token-file ... --scope-file ...`）；上述文件目前未创建，不虚构执行记录。输入秘密通过权限受限文件，不在 shell 参数暴露 token，日志只输出脱敏结果。

验收必须逐项覆盖：正常首帧/增量；非法输入/跨用户/跨scope；replay无重复乱序；600秒过期与容量驱逐；初始事件前断线；Control/各owner重启；owner恢复旧数据epoch；快照加载并发变化；participant-only修订；Identity撤销/到期/不可达的一秒边界（idle、replay、slow-write）；慢消费者/内存额度/REST仍可用；Panel token轮换、终止关闭和REST恢复；日志/秘密泄露负例；真实Compose连接。快时钟测试用注入时钟，真实撤销期限单独实际测时，避免用睡600秒替代可控测试。

## 7. 未冻结决策与推荐可自决方案

1. 单 owner 地址选择：推荐 query `owner_service`，首帧结构保持既有 scope；同时定义 OperationReady 的 owner_cursor 扩展。现有 schema 不足以在首个事件前恢复，须 WS-01 显式变更，不能隐式猜游标。
2. Adapter/plugin payload：推荐保留原 envelope、使用明确 target 安全状态分支，实际 schema 在主代理统一冻结；不把 target 三层状态压成旧枚举。这是兼容性设计点，不是当前已实现能力。
3. scoped `/ws/control` 聚合：推荐首版 control 不暗含 operations；健康和专用 scoped频道并行，随后按显式 contract扩聚合。若用户要求单 socket全部频道，必须另加有界subscriptions语义并做单流多个owner cursor设计，不能复用单owner cursor伪全局顺序。
4. Journal 持久化：推荐 owner同UoW、commit顺序计数、600秒有界保留；epoch持久且数据集重建变化。纯进程内 polling cache不能满足断线/重启的真实owner cursor要求，故不采用。
5. 状态源首版用snapshot比较是否足够：adapter/plugin允许可合并快照通知，故推荐先复用已授权owner读取；operation不允许临时ref集合。若要求逐条状态过渡，另以owner journal事件扩任务，不冒充现有100ms coalescing保证。
6. 阈值/Origin：推荐64KiB帧、128帧或1MiB待写、32帧replay batch、默认同源，具体全局字节/连接/独立auth worker额度由压测确定；所有最终值写配置定义和运维文档，不用计划数字直接宣称安全容量。
7. 交付次序：首批四事件是最小有用 target增量；tasks/logs/messages与Host扩展已保留明确任务和依赖。只完成首批应报告部分完成，不能把全范围替换成文档或隐含延期。

这些方案在用户已授权的WS计划范围内可由主代理按契约兼容性定案；涉及新增业务权限、降低当前撤销门限、扩大秘密/正文可见性、改变保护基线时不能借本报告自行授权。

## 8. 此轮实际验证与限制

- `./scripts/quality/check-asyncapi.sh` exit 0；AsyncAPI parser通过，5条severity=2提示（旧版本建议、未使用安全/组件），不是target实现证据。
- `./scripts/quality/check-openapi.sh` exit 0；Redocly通过，13条既有warning（oneOf重叠、not/required、未用组件），未修改契约或顺手消警告。
- 源码/协议/Panel静态核对及图coverage已完成；一次包含不存在shell glob的辅助查询失败后改为rg目录查找，未将其当缺失实现证据。
- 本轮未编译/运行产品测试、Panel测试、格式/tidy或Compose：本轮仅规划，无需运行重型构建；没有新产品代码或新二进制，不应重复重型历史验收或把未执行检查标绿。`git diff --no-index --check /dev/null doc/plans/2026-11-04-control-ws-plan.md` 无空白诊断（exit 1 表示与空文件有差异）；Python报告检查exit 0，验证UTF-8、末换行、无尾空白及WS-01..WS-10存在。生成物未提交。
- 发现 `reports/analysis/` 被忽略后，已归档到可追踪的 `doc/plans/2026-11-04-control-ws-plan.md`；未修改忽略规则、强制暂存或创建提交。

本轮唯一新增文档为 `doc/plans/2026-11-04-control-ws-plan.md`；其他应同步文档是实施任务的必交付项，当前无产品行为变化，无需提前改写当前规范或doc/27事实。
