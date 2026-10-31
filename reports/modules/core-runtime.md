# Core 运行时（kiarabot-core-runtime）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/node/uv/protoc/docker）。
> 行号以当前 HEAD 工作区为准。只读仓库，唯一写入为本文件。

## 1. 范围与规模

- 目录/文件：`core/app/main.cpp`（组合根）、`core/include/kiarabot/core/{domain,application,ports,runtime}/`、`core/src/{application,runtime}/`、`core/tests/`、`core/cmake/`、`core/CMakeLists.txt`。
- 规模：**9789 行 / 46 个文件**（`.cpp/.hpp/.h` 9569 行 + 2 个 Python 测试 + CMake 106 行）；其中测试 **2597 行（15 个 C++ + 2 个 Python）**，`include/` 2700 行，`src/` 3980 行，`app/main.cpp` 406 行。测试/实现 ≈ 0.36。
- 关键入口与代码地图（file:line）：

| 层次 | 位置 | 说明 |
| --- | --- | --- |
| 进程入口 | `core/app/main.cpp:381-406` | `--check/--help/--grpc-listen` 参数解析，异常兜底 |
| 组合根 | `core/app/main.cpp:76-377` | 信号屏蔽 `:80-85`、受管插件 `:86-96,299-336`、持久化装配 `:121-183`、Action/Host/Scheduler `:184-222`、控制面 `:223-271`、桥启动 `:261-279`、调度线程 `:280-295`、停机 `:358-369` |
| 自检 | `core/app/main.cpp:63-74` | 用参考 Runtime + 内存存储做一次 ingest |
| 端口 | `core/include/kiarabot/core/ports/runtime_store.hpp:15-28`、`ports/ports.hpp:23-137` | `RuntimeStore/RuntimeTransaction` 与 Host 侧端口 |
| 域状态机 | `core/include/kiarabot/core/domain/runtime_engine.hpp:7-778`、`runtime_types.hpp:173-185`、`runtime_state_port.hpp:17-57` | 事件/投递/Action/插件/任务状态机，快照结构 |
| 事件入口 | `core/src/application/adapter_ingress.cpp:119-277` | 注册、校验、去重、会话顺序、持久化确认、背压 |
| 插件投递 | `core/src/application/plugin_hub.cpp:100-412` | 注册授权、逐插件投递、ACK、断线恢复、配置门控 |
| Action | `core/src/application/action_router.cpp:131-479` | 提交/查询/取消/派发/结果，`validate` 在 `:104-128` |
| 权限+审计 | `core/include/kiarabot/core/application/authorized_commands.hpp:23-119` | `PermissionGrant` 精确匹配、幂等、审计同事务 |
| Host | `core/src/application/host_dispatcher.cpp:32-241`、`host_validation.hpp:15-102` | storage.namespace / model.chat / 任务委派 |
| 任务调度 | `core/src/application/task_scheduler.cpp:114-488` | create/get/cancel/claim/finish/recover/tick |
| 控制应用 | `core/src/application/control_service.cpp:200-744`、`control_application.cpp:25-180`、`control_crypto.cpp:44-144` | 认证/会话/RBAC/配置版本/幂等/加密状态/健康 |
| 配置下发 | `core/src/application/plugin_config_delivery.cpp:24-141` | 每连接一条在途配置、10s 确认期限 |
| 传输 | `core/src/runtime/bridge_server.cpp:112-550` | 双流认证、发送线程、断线清理、背压 |
| 运行时支撑 | `core/src/runtime/{runtime_log.cpp,plugin_process.cpp,request_digest.cpp}`、`runtime/runtime_state.hpp`、`runtime/in_memory_runtime_store.hpp` | 日志装饰器、POSIX 子进程、SHA-256、快照适配、内存存储 |

- 对应设计文档：`doc/02-core.md`（主）、`doc/01-runtime-semantics.md`、`doc/16-development-tasks.md`（T01a/T01b/T02a/T03a/T03b/T04a/T04b/T05a/T05b/T06a/T06b/T06c/T08a/T08b/T10a/T10b）、`doc/17-account-ban-risk.md`、`doc/18-architecture-baseline.md`（只读）、`README.md`；跨进程契约以 `proto/kiarabot/v1/{events,actions,plugins,adapters,services}.proto` 为准，控制面资源以 `api/openapi.yaml` 为准。

## 2. 模块职责（依据）

- 基线自述：`doc/18-architecture-baseline.md:135-147` 把 C++ Core 定义为「事件接收和分发、命令路由、权限检查、插件调度、会话管理、AI 请求编排、任务调度、统一消息发送、配置管理、日志和审计、控制面板 API」；`doc/18:414` 认可当前实现为「运行时、适配器接入、插件调度、Action、Host（存储/模型/任务）、任务调度、控制应用端口和 gRPC 桥接」。
- 模块文档职责：`doc/02-core.md:29-31`「业务中枢，负责接收统一事件、执行领域规则、调度插件、管理权限和发出统一 Action；不负责登录 QQ，也不直接调用 AI/数据库/浏览器」；`doc/02:84-99` 声明 Core 拥有 BotInstance/AdapterInstance/PluginInstance/User/Group/Conversation/Permission/Role/Policy/Task/AuditLog 与事务边界；`doc/02:169-184` 声明内部接口 `EventRouter/PolicyService/ActionDispatcher`。
- 运行时语义唯一来源：`doc/01-runtime-semantics.md`（投递/顺序/重试/死信/Action/重连/任务）。
- 契约来源：`proto/kiarabot/v1/services.proto`（`AdapterBridge.Connect` / `PluginBridge.Connect` 双向流）、`events.proto`（`EventEnvelope`）、`actions.proto`（`ActionEnvelope` 的 7 种 payload）、`plugins.proto`（`HostRequest` 7 种操作、`PluginConfig/Ack`、feature 名 `host.storage.v1/host.model.v1/host.tasks.v1/action.result.v1/plugin.config.v1`）。
- 端口契约：`core/include/kiarabot/core/ports/` 由 Core 定义、基础设施实现（`AGENTS.md`「基础设施实现 Core 定义的端口，不能决定业务规则」）。

## 3. 已实现能力（逐条带证据）

> 「证据」列为实现位置；「对应测试」为目标测试内的用例函数/入口。所有测试仅静态读到，未运行。

| # | 能力 | 证据（file:line） | 对应测试 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 无参数常驻 + 主线程 sigwait 消费 SIGINT/SIGTERM，退出 0 | `core/app/main.cpp:80-85,341-357,370-371` | `core/tests/process_lifecycle_test.py`；`grpc_process_test.py` | 完整 |
| 2 | `--check` 一次退出、`--help` 0、参数错误 2、非 POSIX 1 | `core/app/main.cpp:63-74,390-402,372-376` | `process_lifecycle_test.py` | 完整 |
| 3 | gRPC 仅允许 `127.0.0.1:` 前缀、端口 0 自动分配、1 MiB 收包上限 | `core/src/runtime/bridge_server.cpp:527-550` | `bridge_server_test.cpp:34-44` | 完整 |
| 4 | 凭据从环境变量读取并校验（token 32–4096 可见 ASCII、不可重复、成对） | `core/app/main.cpp:111-120,299-305`；`bridge_server.cpp:57-77` | `bridge_server_test.cpp:37-40,69-81`；`grpc_process_test.py` | 完整 |
| 5 | 未认证 UNAUTHENTICATED / 错角色 PERMISSION_DENIED / 重复 authorization 头拒绝 | `bridge_server.cpp:38-55,84-90` | `bridge_server_test.cpp:69-78` | 完整 |
| 6 | 未装配持久化入口时显式拒绝业务（不静默丢弃、不发假确认） | `bridge_server.cpp:100-104`（`CAPABILITY_NOT_SUPPORTED`） | `bridge_server_test.cpp:79-95` | 完整 |
| 7 | 组合根装配 SQLite + 日志装饰器 + ingress/plugin/action/host/scheduler/control | `core/app/main.cpp:121-222,223-271` | `grpc_process_test.py`（`--sqlite`）；`adapter_ingress_test.cpp:149-212` | 完整（形态受限，见 §4） |
| 8 | 存储打开失败/非法 DB 路径/`file:` URI/`:memory:` 拒绝，坏 schema 拒绝启动 | `main.cpp:126-138` | `grpc_process_test.py`（断言 `user_version==5`、篡改后退 1） | 完整 |
| 9 | 受管插件 POSIX 子进程：独立进程组、清空信号屏蔽、TERM→宽限→KILL、退出保持 Core 存活并记录 UPSTREAM_UNAVAILABLE | `main.cpp:86-96,299-336,343-353`；`core/src/runtime/plugin_process.cpp:15-113` | 仅 `sdk/python/tests/test_process.py`（跨模块） | 完整 |
| 10 | 停机顺序：先停控制面，再停并 join 调度线程，再 Shutdown/Wait 桥 | `main.cpp:299-336,358-369` | `bridge_server_test.cpp:118-131`；`plugin_hub_test.cpp:293-296` | 完整 |
| 11 | 适配器注册：身份/协议/实现/账号白名单/能力上限校验，拒绝即 PERMISSION_DENIED | `adapter_ingress.cpp:137-183` | `adapter_ingress_test.cpp:72-90` | 完整 |
| 12 | 实例替换与 fencing（旧实例 ingest 返回 CONFLICT，旧 disconnect 不影响新实例） | `adapter_ingress.cpp:181,202-205,265-276` | `adapter_ingress_test.cpp:123-130` | 完整 |
| 13 | 事件校验：来源/账号/实例一致、schema_version、occurred_at、ByteSize≤1e6、payload 形状、禁止伪造 TimerTriggered/PluginStatusChanged | `adapter_ingress.cpp:45-116` | `adapter_ingress_test.cpp:91-103` | 完整 |
| 14 | 事件去重：`event_id` + 确定性 Protobuf SHA-256 fingerprint，同 id 不同内容 CONFLICT | `adapter_ingress.cpp:33-36,209-253`；`runtime/request_digest.cpp:36-50` | `adapter_ingress_test.cpp:113-120` | 完整 |
| 15 | 会话顺序：水位/degraded、跳号接受、回退拒绝，长度前缀三元组会话键 | `adapter_ingress.cpp:37-44,238-240`；`runtime_engine.hpp:49-74` | `runtime_test.cpp:25-62`（引擎级）；`adapter_ingress_test.cpp` | 完整 |
| 16 | 背压：队列上限 1024 → RATE_LIMITED retryable + 立即关闭本流 | `runtime_engine.hpp:32-35`；`bridge_server.cpp:253-255` | `adapter_ingress_test.cpp:121-122,203-207` | 完整 |
| 17 | 提交后才 ACK：PERSISTED / DUPLICATE，提交前断线重投得 DUPLICATE | `adapter_ingress.cpp:224-259` | `adapter_ingress_test.cpp:149-212`（含提交后未消费 ACK 的断线场景） | 完整 |
| 18 | 存储故障显式上抛（read/begin/commit），不伪装不存在/重复 | `authorized_commands.hpp:44-51,112-116,132-139` | `adapter_ingress_test.cpp:104-112`；`authorized_commands_test.cpp` | 完整 |
| 19 | 插件注册：清单/权限/能力/订阅授权，未授权限即 PERMISSION_DENIED，运行形态收紧 | `plugin_hub.cpp:100-222` | `plugin_hub_test.cpp:134-152` | 完整 |
| 20 | 逐插件投递 `(event_id, plugin_id)`：事务内按当时订阅快照建记录，重复事件不追加 | `plugin_hub.cpp:258-274`；`runtime_engine.hpp:637-656` | `plugin_hub_test.cpp:153-168` | 完整 |
| 21 | 每插件独立 ACK/重试/死信，退避 1/2/4/8/16s ±20%，5 次后 DEAD_LETTER | `runtime_engine.hpp:284-306,614-634` | `plugin_delivery_test.cpp:147-180`；`runtime_test.cpp:101-114` | 完整 |
| 22 | `ordered` 会话串行 + 每插件单在途 | `plugin_hub.cpp:297-302`；`runtime_engine.hpp:658-685` | `plugin_delivery_test.cpp:18-71`；`plugin_hub_test.cpp:153-196` | 完整 |
| 23 | 断线：先 join 旧发送线程→detach→unavailable+inflight 转 retrying，重连按新 trace 重投 | `bridge_server.cpp:344-373`；`plugin_hub.cpp:385-412`；`runtime_engine.hpp:336-348` | `plugin_hub_test.cpp:243-297`；`plugin_delivery_test.cpp:73-101` | 完整 |
| 24 | 进程重启恢复：attach 时 `recover_plugin_inflight`；跨会话/ACK 后指纹不变 | `plugin_hub.cpp:174-196` | `plugin_hub_test.cpp:188-195`；`plugin_delivery_test.cpp:85-101` | 完整 |
| 25 | 配置下发闭环：协商 `plugin.config.v1`、每连接在途一条、10s 超时 REJECTED、应用后才放行事件 | `plugin_hub.cpp:206-248,294-296`；`plugin_config_delivery.cpp:42-141` | `plugin_hub_test.cpp:98-132`；`control_service_test.cpp:57-165` | 完整 |
| 26 | Action 授权：`message.send` 精确授予，未授权/伪造 `requested_by` PERMISSION_DENIED | `action_router.cpp:100-103,207-223` | `action_router_test.cpp:79-86` | 完整 |
| 27 | 仅 `send_message` 群/私聊纯文本子集；其余 6 种 payload 明确 CAPABILITY_NOT_SUPPORTED | `action_router.cpp:104-128` | `action_router_test.cpp:87-89`（delete_message） | 完整（属声明子集） |
| 28 | Action 幂等：调用方隔离、内容冲突 CONFLICT、同键复用原结果、action_id 全局唯一 | `action_router.cpp:220-273`；`authorized_commands.hpp:58-82` | `action_router_test.cpp:100-107` | 完整 |
| 29 | 派发：先提交 `dispatched` 再写流，重连重发同一 Action，适配器实例写入 envelope | `action_router.cpp:344-417` | `action_router_test.cpp:108-116` | 完整 |
| 30 | 结果/查询/取消/EXPIRED 语义：派发后取消 CONFLICT、迟到结果不覆盖终态、查询超时不改状态 | `action_router.cpp:274-343,418-479` | `action_router_test.cpp:117-146` | 完整 |
| 31 | Host storage：`storage.namespace` 授权、插件命名空间隔离、TTL、幂等 receipt、限额 | `host_dispatcher.cpp:104-241` | `host_dispatcher_test.cpp:42-146` | 完整 |
| 32 | Host model：`model.chat` 授权、调用前 STARTED/结果审计分离、事务外 I/O、错误脱敏、deadline | `host_dispatcher.cpp:38-103` | `model_host_test.cpp`（含 provider 错误不泄密、审计故障不可重试） | 完整 |
| 33 | Host tasks：`task.schedule` + `event.timer.triggered` 订阅门控、任务幂等 receipt | `task_scheduler.cpp:205-311` | `task_scheduler_test.cpp:161-197` | 完整 |
| 34 | 任务调度：100ms tick、每次最多 32、claim token fencing、recover 把 RUNNING 转 RETRYING/FAILED、COMPLETED 前可靠入队 TimerTriggered | `task_scheduler.cpp:313-488`；`main.cpp:280-295` | `task_scheduler_test.cpp:76-160,199-223` | 完整 |
| 35 | AuthorizedCommands：默认拒绝、服务端 fingerprint、同事务写业务+幂等+审计、拒绝/回调失败回滚 | `authorized_commands.hpp:23-119` | `infrastructure/tests/authorized_commands_test.cpp`；各服务测试 | 完整 |
| 36 | 结构化日志装饰器：提交后按 audit/事件/投递/Action 状态差分输出，sink 故障 fail-open 计数 | `core/src/runtime/runtime_log.cpp:26-140` | `infrastructure/tests/runtime_log_test.cpp` | 完整（覆盖面见 §4） |
| 37 | 控制应用：登录（Argon2id、未知用户名同耗散列、30/min/桶）、刷新轮换、撤销、RBAC 三角色 | `control_service.cpp:208-399`；`control_application.cpp:5-92`；`control_crypto.cpp:44-136` | `control_service_test.cpp:167-252`；`control_application_test.cpp` | 完整 |
| 38 | 控制状态加密：`$control.state` secretbox、`$control.enabled` 投递开关、`$control.manifests` 公共清单；插件不能含 `$` | `control_service.cpp:577-585`；`plugin_hub.cpp:195`；`control_crypto.cpp:137-144` | `control_service_test.cpp:234-241`（无明文 token/密码） | 完整 |
| 39 | 控制幂等：scope=主体+操作+资源+键，同键同 fingerprint 重放、异内容 CONFLICT | `control_service.cpp:330-363,564-576` | `control_service_test.cpp:198-209` | 完整 |
| 40 | readiness（begin+commit 探测写）、stream_snapshot 脱敏投影 | `control_service.cpp:184-199,700-744` | 无 Core 单测；仅 `control/api/tests`（跨模块，待确认） | 部分 |
| 41 | 健康：storage 可写、adapter 连接、逐插件 state | `control_service.cpp:443-483` | `control_service_test.cpp`（健康未覆盖） | 部分（6 项中 3 项） |
| 42 | 任务/控制资源列表：Sessions/Plugins/Bots/Adapters/Models/Users/Audit/Logs/Tasks | `control_service.cpp:587-699` | `control_service_test.cpp:191-197` | 部分（Groups 缺失） |

## 4. 缺口清单

> 类型取自 `_brief.md` §3；严重度：P0 阻断主流程/已声明验收/安全底线，P1 功能域缺失或契约未实现但主流程可跑，P2 体验/文档/增强。

| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |
| G1 | **保留期清理全部未接线**：`purge_expired()` 无生产调用方；审计 `std::vector` 无任何删除/裁剪代码；命令幂等表终身保留 | 未接线 + 半实现 | **P0** | `runtime_engine.hpp:390-439`（purge 实现）仅 `core/tests/*` 调用（`runtime_test.cpp:56,154`、`plugin_delivery_test.cpp:68,177`、`runtime_store_test.cpp:77`、`core_smoke.cpp:65`）；全仓无 `audit.erase/clear/resize`；`authorized_commands.hpp:84-86` 审计达 100000 即 `UPSTREAM_UNAVAILABLE`；`sqlite_runtime_store.cpp:12,98-100` 快照 >64 MiB 直接 `Unavailable` | 长期运行后：审计满 → 事件接收（`adapter_ingress.cpp:225` 经 AuthorizedCommands）与所有受保护写永久返回 UPSTREAM_UNAVAILABLE；或快照超 64 MiB → 所有 commit 失败。两条都是不可自愈的写路径死亡。`doc/02:17,111,250`「保留期清理」「清理由运维任务补齐」在 doc/16 台账中无对应任务 |
| G2 | **共享快照并发写无全局串行化**：组合根把同一 `RuntimeStore` 交给 AdapterIngress/PluginHub/ActionRouter/HostDispatcher/TaskScheduler/ControlService，各服务只有私有 mutex；SQLite 以 revision 乐观校验，跨服务并发 commit 得到 `StorageError::Conflict` | 半实现 + 契约不一致 | **P1** | `main.cpp:175-178,219-222,259-261,172-173`（同一 `*persistent_store`）；`sqlite_runtime_store.cpp:93-121`（`UPDATE ... WHERE revision=?`）；冲突后果：`bridge_server.cpp:228-230`（adapter ActionResult）、`:486-498`（插件任意拒绝帧后 `break`）、`plugin_hub.cpp:330-383`（ACK 冲突无重试）。文档称「公共 Runtime 串行访问」（`doc/02:242`、`doc/01:109`），但生产装配没有该串行锁 | 事件接收 + 插件 ACK + 适配器 Action 结果 + 调度 tick 并发时按 revision 冲突互相拒绝：适配器/插件被断流、进度丢给上层重连；台账未声明该并发模型 |
| G3 | **无 ACK 超时**：插件收到投递后不 ACK（挂起/半死）时，该插件唯一的在途记录永远停在 `dispatched`，后续投递永久阻塞 | 未实现 | **P1** | `plugin_hub.cpp:297-302`（任一 `dispatched` 即返回空）；`runtime_engine.hpp:284-306` 只有显式 fail/ACK 才推进；无 deadline 字段/计时器；`doc/01:49-57` 未定义 ACK 期限 | 单插件挂起即该插件全量事件停摆，只能靠 TCP/流断开兜底；与「重试/死信」承诺不完整 |
| G4 | **可重试错误被当成断连**：插件流任一拒绝帧即 `break`，适配器流非可重试错误即 `TryCancel`；`acknowledge`/`complete`/`dispatch` 路径没有 `next` 那样的 20ms 重试 | 半实现 | **P1** | `bridge_server.cpp:486-498`、`:187-195`、`:228-230`；对比 `:188-191`（仅 next 冲突重试）、`:404-407`（仅 next_config/next） | 并发冲突/瞬时存储故障表现为断连+恢复风暴，而不是退避重试；`plugin_hub.cpp:398-401`、`detach` 失败同样被忽略 |
| G5 | **组合根只支持单适配器 + 单插件**：只有一组 `KIARABOT_ADAPTER_*` / `KIARABOT_PLUGIN_*` 环境变量，PluginHub 只构造一个 `PluginPolicy` | 半实现 | **P1** | `main.cpp:112-120,155-174`；`valid_credentials` 本身允许 N 条（`bridge_server.cpp:57-77`），但组合根只喂 1 条；测试用两个插件（`plugin_hub_test.cpp:137-150,243-263`） | T03a 验收「两个插件独立 ACK」只在测试可复现；真实部署无法装配第二个插件/适配器 |
| G6 | **业务入口强绑定一种存储形态**：只有 `KIARABOT_RUNTIME_DB`+`KIARABOT_ADAPTER_ACCOUNTS`+`KIARABOT_ADAPTER_ID` 同时存在才装配 ingress；只给 DB 直接退出 1；无法注入其他 `RuntimeStore` 实现，也无法不加适配器只跑控制面 | 半实现 | P1 | `main.cpp:123-183`（`:125-133` 拒绝条件） | `RuntimeStore` 端口实际只有一个生产实现；控制面必须依附适配器/插件装配才能启动 |
| G7 | **参考 Runtime 生命周期 API 仅测试接线**：`recover_inflight_events/dispatchable_events/due_retries/dead_letters/replay_dead_letter/expire_actions/purge_expired/update_plugin_recovery` 无生产调用方 | 未接线 | P1 | `core/include/kiarabot/core/application/runtime.h:57-140,164-171`；`main.cpp:64` 仅 `--check` 使用 `Runtime`；`doc/02:111`、`doc/01:57,71,81` 要求宿主调用 | 文档声明的「宿主启动恢复/定期 expiry/5 分钟插件 FAILED」在进程装配中不存在；插件 FAILED 只在重新 attach 时被动触发（`plugin_hub.cpp:179-184`），断线插件长期停留 UNAVAILABLE |
| G8 | **Action 域权限只有单一 `message.send`**：无资源范围（群/账号/目标）、无速率/内容/预算策略；`action.cancel/action.complete/action.dispatch/event.publish/plugin.connect` 由调用点硬编码恒真 grant | 半实现 | P1（安全/封禁风险） | `action_router.cpp:100-103,220-231,317-321,383-391,437-445`；`adapter_ingress.cpp:209-223`；`plugin_hub.cpp:135-139`；`doc/17:38,67,94` 明确「现有权限模型（仅 message.send）不足以约束危险 Action」「缺速率与内容策略」 | 一旦放开危险 Action，需要重建授权模型；当前无速率限制，`message.send` 可被插件无限刷（仅受 10000 条 Action 容量限） |
| G9 | **状态模型远小于文档声明**：Core 不拥有 User/Group/Conversation/BotInstance/AdapterInstance/Permission/Role/Policy 持久状态 | 未实现 | P1 | `doc/02:86-95` vs `runtime_types.hpp:173-185`（只有 events/sessions/actions/plugins/deliveries/audit/commands/namespaces/receipts/tasks）；权限是构造期静态 `PermissionGrant`/`PluginPolicy`（`authorized_commands.hpp:6-10`、`plugin_hub.hpp:10-13`） | 用户/群/会话/角色管理、按资源授权、权限变更审计均无实现基础；当前台账任务未宣称该项完成，属基线能力缺口 |
| G10 | **`GET /groups` 契约有、实现无**：`ControlResource::Groups` 有策略和传输映射，但 `resources()` 无分支 | 半实现 + 契约不一致 | P2 | `control_application.hpp:40`；`control_application.cpp:51-52`；`control/api/src/http_mapper.cpp:221`；`api/openapi.yaml:398-407`；`control_service.cpp:587-699` 无 Groups 分支 | 面板 `listGroups` 恒返回空页且无错误，无法区分「没有群」与「未实现」 |
| G11 | **`stream_snapshot` 可能解引用 end()**：`find_if` 结果未判空即 `found->role` | 未验证（健壮性） | P1 | `control_service.cpp:714-729` | 会话记录引用了配置中已删除的用户（重启换用户表）时越界访问 → 崩溃/未定义行为；该函数无 Core 单测 |
| G12 | **健康检查覆盖不全**：6 项承诺只实现 3 项，缺协议监听状态、待处理事件数、最近成功 Action 时间、数据库可写原因 | 半实现 | P2 | `doc/02:202-211` vs `control_service.cpp:443-483`（storage/adapter/plugins 三组件） | 故障定位能力不足；T11a 验收「迁移失败禁止 ready」由 control 侧保证，但 Core 健康详情不反映队列/DB 写故障 |
| G13 | **硬编码上限与默认不可配置**：Action 10000、审计 100000×4 处、任务 1024/插件、receipts 10000、namespace 1024 keys/1 MiB、会话 4096、分页 1024、登录 30/min/1024 桶；`RuntimeOptions`/`NamespacePolicy`/`ControlOptions.audit_limit` 可注入但组合根一律用默认 | 半实现 | P1 | `action_router.cpp:243,181`；`plugin_hub.cpp:363`；`task_scheduler.cpp:11-12,165-167,284,413`；`host_dispatcher.hpp:8-12`；`control_service.cpp:257-265,376,423,564`；`main.cpp:178,212,221,260`（无参数覆盖）；`control/api/src/configuration.cpp:43-51` 不读 `audit_limit` | 容量上限不可按部署调整，且 `audit_limit` 达到后是硬失败（见 G1）；运维无法用配置缓解 |
| G14 | **只支持一种插件运行时形态**：`runtime=="python-rpc"`、`isolation=="process"`、无 dependencies、config_schema 空/`{}`，其他一律 CAPABILITY_NOT_SUPPORTED；`manifest.entrypoint` 只校验不使用 | 半实现 | P2 | `plugin_hub.cpp:117-127`；启动实际来自 env（`main.cpp:313`） | C ABI/进程内插件、声明式依赖、schema 驱动配置在 Core 侧无路径（doc/18 已把 C ABI 列为未来，方向一致） |
| G15 | **模型/存储/日志各只有一种形态**：模型只有 HTTP `/chat/completions` 非流式；存储只有单行快照 SQLite；日志只有缓冲 sink；`IFileStorage` 端口无任何 Core 调用方 | 半实现 + 未接线 | P2 | `main.cpp:186-207`；`sqlite_runtime_store.cpp`；`ports/ports.hpp:131-137` + `infrastructure/{include/…,src}/file_storage.*` 仅 infra 测试使用 | 换供应商/换存储/文件能力都需改组合根与实现；文件存储能力虽声明却不可用 |
| G16 | **死端口**：`INamespaceStore`、`IIdempotencyStore`、`IUnitOfWork`、`IUnitOfWorkFactory`、`IAuditLogger` 全无实现、无调用方 | 未实现 | P2（文档未声明的扩展端口） | `ports/ports.hpp:23-32,40-47,52-69,125-129`；全仓 grep 无实现类；`doc/02:11` 只承认 `IUnitOfWork` 是「后续扩展端口」，未提其余四个；`doc/02:126` 称「审计端口」存在，实际审计是快照内 `vector<RuntimeAuditEntry>` | 端口清单误导读者以为有 Repository/UoW/审计端口；与 `doc/18:184-196` 列的 IMessageBus/IConversationStore/IUserRepository/IPermissionStore 一样属于未落地 |
| G17 | **事件字节「不透明」契约漂移**：Core 解析 envelope 做校验/fingerprint，再以 `SerializeAsString()` 重新存 | 契约不一致 | P2 | `doc/01:26`「inbox 保存传入的序列化 EventEnvelope 字节，Core 将其视为不透明数据」vs `adapter_ingress.cpp:33-36,237-240`；`runtime_engine.hpp:20-101` | 存的是 Core 重编码字节；「恢复/重放保留这些字节」只在 Core 内部自洽，不能保证与发送方字节逐字节相同（近似无害，但契约表述不实） |
| G18 | **重试由 20ms 轮询驱动，无到期任务定时器**：`due_retries`/`next_attempt` 语义正确，但投递延迟分辨率 20ms 且空转产生持续 DB 事务/快照解码 | 半实现（性能） | P2 | `bridge_server.cpp:205-207,424-426`（sleep 20ms）；`plugin_hub.cpp:276-328` 每次 next 都 `store_.begin()` 解全量快照 | 空载时每插件每秒 50 次全量快照解码；结合 G1 的快照增长会放大成本；无背压信号（纯轮询） |
| G19 | **审计/幂等无清理调度**（与 G1 同源但单独列）：`doc/01:117`「当前命令幂等元数据保留到显式清理，尚未实现清理调度」 | 未实现 | P1 | `doc/01:117`；`doc/02:250`；代码无清理入口 | 幂等键+审计使快照单调增长；这就是 G1 的触发源 |
| G20 | **危险 Action 只做了「拒绝」，没有后续门禁设计**（无 per-plugin/per-group/每日预算模型） | 未实现 | P2（当前安全，未来风险） | `doc/17:67,94`；`action_router.cpp:104-128` | 与 G8 配套；实现危险 Action 前必须补授权模型（doc/17 已明确） |
| G21 | **无插件自动重启/健康 API**（文档已声明留待后续） | 未实现（已声明） | P2 | `doc/02:19`；`main.cpp:343-353` 只记录退出 | 与文档一致，仅登记 |

### 4.1 关键测试缺口（正常/拒绝/超时/依赖故障四路径）

| 行为 | 正常 | 拒绝 | 超时 | 依赖故障 | 缺口说明 |
| --- | --- | --- | --- | --- | --- |
| 事件接收/去重/backpressure | ✅ `adapter_ingress_test.cpp:113-122` | ✅ `:91-108` | n/a（事件无 deadline） | ✅ `:104-109` | 缺「同 event_id 不同账号/跨适配器账号重叠」与 1024 队列真实容量边界（用 queue_limit=1 替代） |
| 插件投递/ACK/重试 | ✅ `plugin_hub_test.cpp:134-196` | ✅ `:140-148,184-185` | ❌ **无 ACK 超时测试（G3）** | ✅ `:158-162`；`plugin_delivery_test.cpp:117-126` | 缺「插件挂起不 ACK」、缺「控制禁用后投递行为」（仅测试了 re-register 前 `detach`） |
| 插件配置门控 | ✅ `control_service_test.cpp:57-165` | ✅ `:122-132` | ✅ `:135-138` | ✅ `:315-320`（read 故障） | 缺 `next_config` 权限/竞态（并发 attach）覆盖 |
| Action 全链路 | ✅ `action_router_test.cpp:100-116` | ✅ `:79-89,105` | ✅ `:134-146`（EXPIRED/query TIMEOUT） | ✅ `:90-99` | 缺 10000 容量 RATE_LIMITED、缺「适配器未声明 capability」、缺 100000 审计上限 |
| Host storage/model/tasks | ✅ `host_dispatcher_test.cpp:42-108`；`model_host_test.cpp:57-85`；`task_scheduler_test.cpp:161-197` | ✅ `host_dispatcher_test.cpp:50-53`；`model_host_test.cpp:55-58`；`task_scheduler_test.cpp:173-180` | ✅ `host_dispatcher_test.cpp:136-145`；`model_host_test.cpp:81-85`；`task_scheduler_test.cpp:195-196` | ✅ | 缺每插件 namespace/receipt 清理边界与真实 64 MiB 快照上限 |
| 任务调度 tick/recover | ✅ `task_scheduler_test.cpp:199-223` | ✅ `:102-103,114-116` | ✅ `:195-196` | ✅ `:104-111,181-184` | 缺「tick 中途 finish 失败后剩余任务不推进」（`:472-484` 行为无测试）与 32 条/ tick 上限 |
| 控制应用 | ✅ `control_service_test.cpp:167-252` | ✅ `:172-173,185-189` | ✅ `:243-251` | ✅ `:262-277` | `stream_snapshot`/`readiness` 无 Core 单测；`ControlResource::Groups` 全仓无测试 |
| 桥接传输 | ✅ `plugin_hub_test.cpp:243-297`；`adapter_ingress_test.cpp:149-212` | ✅ `bridge_server_test.cpp:69-81` | ✅ `:108-117` | ✅ `:118-131`（停机） | `bridge_server_test` 只覆盖「未接线拒绝路径」；已接线路径无「未协商 feature 即发 host/action 被断流」、heartbeat、并发流测试 |
| 组合根进程 | ✅ `grpc_process_test.py`、`process_lifecycle_test.py` | ✅ `grpc_process_test.py`（缺凭据/外部地址/非法 DB 均退出 1） | ✅（启动/停止超时） | ✅（DB 非法/schema 损坏） | 缺模型三环境变量、控制配置、`KIARABOT_PLUGIN_PERMISSIONS` 解析、多实例装配的进程级测试；受管插件仅 `sdk/python/tests/test_process.py` 覆盖 |
| 空闲清理/保留期 | ❌ | n/a | n/a | 仅引擎级 `runtime_test.cpp:56,154` | 生产装配下「7 天保留」无任何验证（G1/G7） |

### 4.2 端口 vs 实现对照（`ports.hpp` / `runtime_store.hpp` 全量）

| 端口（定义位置） | Core 侧实现 / 内存实现 | infrastructure 实现 | 生产调用方（接线点） | 状态 |
| --- | --- | --- | --- | --- |
| `RuntimeStore`（`ports/runtime_store.hpp:23-28`） | `InMemoryRuntimeStore`（`core/include/kiarabot/core/runtime/in_memory_runtime_store.hpp:6`，测试/本地用）、`LoggingRuntimeStore`（装饰器，`core/include/kiarabot/core/runtime/runtime_log.hpp:25`） | `SqliteRuntimeStore`（`infrastructure/include/kiarabot/infrastructure/sqlite_runtime_store.hpp:6`） | `main.cpp:134-142`（SQLite + 日志装饰器）再注入 ingress/plugin/action/host/scheduler/control | 已接线（生产仅一种实现） |
| `RuntimeTransaction`（`ports/runtime_store.hpp:15-21`） | 上述三者的内部 `Transaction` 类（`in_memory_runtime_store.hpp`、`runtime_log.cpp:47-109`） | `SqliteRuntimeStore::Transaction`（`sqlite_runtime_store.cpp:84-126`） | 各应用服务 `store_.begin()` | 已接线 |
| `detail::RuntimeStatePort` + `detail::UnitOfWork`（`domain/runtime_state_port.hpp:8-57`） | `RuntimeState`（`core/include/kiarabot/core/runtime/runtime_state.hpp:9-348`） | 无（域内端口） | `RuntimeEngine` 构造（`runtime_engine.hpp:9`）；各服务内 `detail::RuntimeState state(snapshot)` | 已接线（仅内存实现，用于事务私有快照） |
| `INamespaceStore`（`ports.hpp:23-32`） | 无 | 无 | 无 | **未实现/无调用方**；存储能力由 `HostDispatcher` 直接读写 `snapshot.namespaces`（`host_dispatcher.cpp:164-210`） |
| `IIdempotencyStore`（`ports.hpp:40-47`） | 无 | 无 | 无 | **未实现/无调用方**；幂等由 `snapshot.host_receipts` 字段实现（`host_dispatcher.cpp:147-220`） |
| `IUnitOfWork` / `IUnitOfWorkFactory`（`ports.hpp:52-69`） | 无（与 `detail::UnitOfWork` 同名不同物，易混淆） | 无 | 无 | **未实现**；`doc/02:11` 声明其为「后续扩展端口」 |
| `IAuditLogger` + `AuditRecord`（`ports.hpp:114-129`） | 无 | 无 | 无 | **未实现/无调用方**；实际审计是 `snapshot.audit`（`runtime_types.hpp:133-145`），与 `doc/02:126` 的「审计端口」说法不符 |
| `IModelProvider`（`ports.hpp:71-76`） | 无 Core 实现；测试 fake 在 `core/tests/model_host_test.cpp:17-39` | `HttpModelProvider`（`infrastructure/include/kiarabot/infrastructure/http_model_provider.hpp:14`） | `main.cpp:186-207` → `HostDispatcher` | 已接线（仅一种模型形态，见 G15） |
| `ITimeZoneResolver`（`ports.hpp:87-91`） | 无 Core 实现 | `TimeZoneResolver`（`infrastructure/include/kiarabot/infrastructure/time_zone_resolver.hpp:6`） | `main.cpp:211-212` → `TaskScheduler` | 已接线 |
| `IScheduler`（`ports.hpp:98-112`） | `TaskScheduler`（`core/include/kiarabot/core/application/task_scheduler.hpp:7`） | 无（Core 自实现；`doc/18` 说明基线的 CronSchedulerAdapter 未实现） | `HostDispatcher` 构造 + `main.cpp:280-295` tick 线程 | 已接线 |
| `IFileStorage`（`ports.hpp:131-137`） | 无 | `FileStorage` / `FakeFileStorage`（`infrastructure/include/kiarabot/infrastructure/file_storage.hpp:14,27`） | **无**（`core/` 全无引用） | **未接线** |
| `IControlApplication`（`application/control_application.hpp:123-142`） | `ControlService`（`control_service.hpp:32`） | 无（`control/api` 是消费者，不是实现） | `main.cpp:259-260,266` → `HttpServer` | 已接线 |
| `IRuntimeLogSink`（`runtime/runtime_log.hpp:18-22`） | 无 Core sink | `BufferedRuntimeLogSink`（`infrastructure/include/kiarabot/infrastructure/runtime_log_sink.hpp:10`） | `main.cpp:99,141-142` | 已接线 |

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/02:109`「Runtime 负责去重、会话顺序、背压、重试、死信查询和重放、Action 生命周期与插件重连状态转换」 | 生产进程不使用 `Runtime` facade；各应用服务各自内联调用 `detail::RuntimeEngine` 的子集，死信查询/重放/到期扫描/保留期清理无生产入口 | `main.cpp:64`（仅 `--check`）；`runtime.h:57-140` 仅测试引用 |
| `doc/02:111`「宿主在确认旧投递者已停止后调用 `recover_inflight_events()`…再从 `dispatchable_events()` 取得可投递事件」 | 组合根无此调用；插件路径改用 `PluginHub::attach` 内 `recover_plugin_inflight`（等价但非该 API）；适配器路径无对应恢复 | 全仓 `recover_inflight_events` 仅 `runtime.h`、`runtime_engine.hpp`、`runtime_test.cpp` |
| `doc/01:71`「宿主须定期调用 `expire_actions()`」、`doc/01:81`「宿主须定期调用 `update_plugin_recovery()`」 | 组合根无调用；Action 只在被 `next`/`query` 扫到时惰性过期；插件 FAILED 只在重新 attach 时触发 | `main.cpp:280-295` 只有 `TaskScheduler::tick()`；`plugin_hub.cpp:179-184` |
| `doc/02:242`/`doc/01:109`「公共 Runtime 串行访问；存储实现须保护不同事务之间的并发冲突」 | 生产装配有 5+ 独立写者共享一个 SQLite 快照，无全局串行；跨服务 commit 会 revision 冲突（见 G2） | `main.cpp` 各服务共享 `*persistent_store`；`sqlite_runtime_store.cpp:93-121` |
| `doc/02:202-211` 健康状态至少 6 项 | 只实现 storage/adapter/plugins 三项 | `control_service.cpp:443-483` |
| `doc/02:169-184` 内部接口 `EventRouter`/`PolicyService`/`ActionDispatcher`；`doc/02:86-95` 拥有 User/Group/Conversation/Permission/Role/Policy | 这些类/状态不存在；授权是静态 `PermissionGrant`+内联 `find` | `runtime_types.hpp:173-185`；`authorized_commands.hpp:6-10`；全仓无 `PolicyService` |
| `doc/02:250`「审计满（默认十万条）时返回 UPSTREAM_UNAVAILABLE」+`doc/02:17`「终态保留至少七天，清理由运维任务补齐」 | 十万条路径实现正确，但**没有任何清理实现或运维任务**；台账无对应工作项 | `authorized_commands.hpp:84-86`；doc/16 全文无清理任务；`purge_expired` 仅测试调用 |
| `doc/16:320-333` T10b 验收 `[x] 成功/拒绝/重试/死信/故障覆盖` | 日志只在 commit 后按快照差分产生：重复事件、背压 RATE_LIMITED、Action 查询超时、插件注册/断线本身、控制操作（无 audit 的读请求）均无日志；`doc/02:130` 自己写「不表示已逐路径验证」 | `runtime_log.cpp:47-124`（只遍历 audit 增量/新事件/投递状态/Action 状态）；`doc/02:130` vs `doc/16:326-330` 勾选 |
| `doc/16:130-143` T03a `[x] 两个插件独立 ACK` | 引擎与测试支持 N 插件，但组合根只能装配 1 个插件身份（G5） | `main.cpp:155-174` |
| `doc/16:175-188` T04b `[x]` 五项（权限拒绝不调平台/执行前 deadline/重复复用/断线查询/取消不覆盖到期） | 均有实现与测试；与声明一致（此条为一致项，列表用于对照） | `action_router.cpp:193-343`；`action_router_test.cpp:79-146` |
| `doc/16:83-96` T10a `[x]` 五项 | 授权/幂等/审计原子/失败不泄密均实现且有测试；但「审计满」后的可用性后果未在台账声明（见 G1） | `authorized_commands.hpp`；`infrastructure/tests/authorized_commands_test.cpp` |
| `doc/16:26-38` T01a `[x]` 四项、`[x]` 信号/参数语义 | 与代码一致（本报告 #1/#2）；`--check` 用的是参考 Runtime 而非真实服务，`doc/02:230` 已说明 | `main.cpp:63-74,381-406` |
| `doc/16:40-54` T01b `[x]` 五项 | 一致；但「不得接收业务后无声丢弃」只在未装配时显式拒绝，已装配路径无该测试 | `bridge_server.cpp:100-104`；`bridge_server_test.cpp` |
| `README.md:47`「已实现并纳入自动验收：…Action 纯文本闭环；Host storage/model/tasks 与任务调度；控制 API、认证/RBAC…」 | 代码存在且测试已注册（`CMakeLists.txt:91-146` 等），但 CI 自 `16191cb` 起暂停自动触发、历史 5 次未通过（`doc/16:597-601`），故「纳入自动验收」只能理解为「已注册」，不能理解为「已通过」 | `doc/16:597-601`；`.github/workflows/ci.yml:28-57,87-110`（contracts job 才带 `-DKIARABOT_ENABLE_INFRASTRUCTURE=ON` 并跑 `tests/acceptance/run.sh`） |
| `doc/17:38`「只接受 send_message 文本；MuteMember/KickMember/ApproveRequest 当前被拒绝（action_router.cpp#L110-L119）」 | 准确（引用的行号即 payload 判别；实际判据 `:110-128`） | `action_router.cpp:104-128` |
| `doc/17:67`「现有权限模型（仅 message.send）」 | 对 Action 域准确；但全系统另有控制面 admin/operator/viewer×16 权限矩阵与插件权限串（storage.namespace/model.chat/task.schedule…），表述过窄 | `control_application.cpp:5-92`；`host_dispatcher.cpp:43-44,134-135`；`task_scheduler.cpp:233-234` |
| `doc/02:126`「审计端口只接受明确的标识和状态字段」 | `IAuditLogger` 无实现无调用；实际审计是快照内结构体数组，无端口 | `ports.hpp:114-129`；`runtime_types.hpp:133-145` |

## 6. 跨模块影响（只陈述，不替其他模块下结论）

- **infrastructure（待确认）**：Core 正确性依赖 `SqliteRuntimeStore` 的 revision 语义与 64 MiB 快照上限；完整快照编解码（`runtime_snapshot_codec.hpp:240-245`）决定 G1/G2 的可缓解空间。`IFileStorage` 已在 infrastructure 实现（`file_storage.hpp`）但 Core 无消费方，属未接线端口。
- **control/api（待确认）**：`ControlService::stream_snapshot` 的 end() 解引用（G11）、`GET /groups` 空实现（G10）、`readiness()` 语义都由本模块提供实现，传输层仅映射；`control_service_test.cpp` 还链接了 `kiarabot/control/http_mapper.hpp` 做 503/504 映射断言，边界略耦合。
- **sdk/python + plugins（待确认）**：插件形态被 Core 收窄为 `python-rpc`+`process`（G14）；`action.result.v1`/`host.*.v1`/`plugin.config.v1` 门控在 `bridge_server.cpp:305-336` 与 `plugin_hub.cpp:206-212`，SDK 若未声明 feature 会被直接断流（G4）。
- **adapters/nonebot（待确认）**：必须声明 `action.send_message.text.v1`（`action_router.cpp:139-144`）；背压表现为 Core 关闭本次流（`bridge_server.cpp:253-255`），适配器需按 at-least-once 重投；Action 重连重发需适配器持久去重（`action_router.cpp:368-414`）。
- **doc/18 基线**：`doc/18:184-196` 第六层端口清单（IMessageBus/IConversationStore/IUserRepository/IPermissionStore/IFileStorage/IAuditLogger）中，Core 只落地 IModelProvider/IScheduler/IFileStorage（且 file 未接线），IAuditLogger 是死声明。属基线-实现差异，按 AGENTS.md 只报告不修改。
- **工具链/CI（待确认）**：core 的 `kiarabot_bridge` 目标在 configure 期下载 nlohmann/json（`core/cmake/bridge.cmake:1-19`，EXPECTED_HASH 固定），并强制 OpenSSL 3 + libsodium；离线/受限网络构建会失败。`core/CMakeLists.txt` 的 `kiarabot-core-smoke` 未设置 `cxx_std_20` 与告警选项，与其他目标不一致（P2 级构建一致性问题）。

## 7. 覆盖结论

- 一句话结论：**Core 的事件→插件→Action→Host→任务主链在单一装配形态下实现完整、引擎级测试扎实；但「长期运行可维护性」与「并发/容量边界」未闭环——保留期清理与过期扫描完全未接线，生产装配把 5+ 个并发写者压在同一全量快照上，且插件 ACK 无超时、拒绝即断连。**
- 覆盖度估计：**47/64 个功能点完整覆盖（73%），完整+部分 57/64（89%），未覆盖/未接线 7 个**。
  - 口径：按本报告 §3 的 42 条能力与 §4 的 21 条缺口归并/拆分为 64 个功能点；分组为「进程与组合根 10（完整 7/部分 2/未覆盖 1）」「事件接收 8（8/0/0）」「插件投递 9（7/1/1）」「Action 8（7/1/0）」「Host 与任务 9（8/1/0）」「权限/审计/日志 7（4/2/1）」「控制应用 7（4/2/1）」「端口与生命周期接线 6（2/1/3）」。权重为主观判断，主要用于横向比较，不代表验收结论。
  - 若按台账验收项计（T01a 4、T01b 5、T05a 4、T05b 4、T10a 5、T02a 5、T03a 4、T03b 6、T04a 5、T04b 5、T06a 5+、T06b/T06c/T10b/T08b 各若干），台账勾选项在代码中都能找到对应实现；**但「已实现」≠「可在生产装配中持续运行」**，G1/G2/G3/G7 均为装配层缺口。

## 8. 未核实事项

1. **未构建、未运行任何测试/CTest/CI**（本机缺 cmake/protoc/uv），§3 的「对应测试」只表示测试代码存在并注册（`CMakeLists.txt`、`core/CMakeLists.txt`），不代表通过；`README.md:47` 与 `doc/16` 中所有通过数字均无法在本机复现。
2. **CI 真实状态未核实**：`doc/16:597-601` 称自 `16191cb` 起暂停自动触发且历史 5 次未通过；本轮未联网核查 Actions 运行记录，也未核实手动 `workflow_dispatch` 现状。
3. **G1/G2 的实际触发阈值未量化**：需要运行/压测才能确定真实事件量级下 100000 审计或 64 MiB 快照先到，以及 revision 冲突的实际频率（取决于并发度与快照编码耗时）。
4. **跨模块验收未读**：`tests/acceptance/test_closed_loop.py`、`sdk/python/tests/test_process.py`、`control/api/tests/*` 只做了 grep 级确认（覆盖受管插件、配置下发、控制面流恢复），未逐行核对它们是否覆盖本报告的 G3/G8/G11；这些属 adapter/SDK/control-api 模块职责，待对应分析员确认。
5. **`tests/acceptance/run.sh` 的 Ruby 契约对照**（`proto/tests/api_contract_test.rb`）是否会拦截 G8/G10 这类权限/资源不一致，未核实。
6. **`control/api/src/configuration.cpp` 之外是否还有别处设置 `audit_limit`/日志容量**：目前只看到 `read_configuration` 不设置，`main.cpp:229` 只注入 `logs`；未排除其他装载路径。
7. **`libsodium`/`nlohmann` 下载与离线构建可行性**未验证（`core/cmake/bridge.cmake` 在 configure 期 `file(DOWNLOAD ... TLS_VERIFY ON)`）。
8. **POSIX-only 断言**：`plugin_process.cpp` 只在 `#ifdef __unix__` 下编译（`main.cpp:9-14,76-377`），非 POSIX 平台行为只按 `doc/02:228` 的声明核对，未做平台验证。
9. **§5 引用的 doc/16 行号**为当前 HEAD 台账文本位置，台账后续若被编辑需重新对齐。
