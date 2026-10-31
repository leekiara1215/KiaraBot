# 插件 SDK（kiarabot-plugin-sdk）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/node/uv/protoc/docker）。

## 1. 范围与规模

- 目录/文件：`sdk/python/`（SDK 实现、测试、打包配置、proto 生成脚本）、`sdk/PLUGIN_SDK_DEPENDENCIES.md`（模块依赖/边界声明）、`plugins/event-observer/`、`plugins/ai-reply/`；契约只看 `proto/kiarabot/v1/{plugins,services,events}.proto`，宿主接线只读 `core/`，不替 Core 模块下结论。
- 规模：22 个文件 / 1799 行（另 `sdk/python/uv.lock` 203 行，含 lock 共 2002 行）；测试 8 个文件 / 1051 行，静态枚举 **66 个 pytest 用例**（参数化展开，与台账 T08b「SDK 66/66」一致：`test_runtime` 13、`test_actions` 6、`test_storage` 7、`test_tasks` 2、`test_process` 9、`test_model` 19、`test_closed_loop` 4、`test_config` 6）。
- 关键入口与代码地图（file:line）：
  - 公共 API 出口：`sdk/python/src/kiarabot_sdk/__init__.py:2-5` 只导出 `Context/Options/Plugin/PluginError/Runner/load_manifest/EventEnvelope`。
  - 进程入口：`sdk/python/src/kiarabot_sdk/__main__.py:12-40`（`python -m kiarabot_sdk --manifest <path>`），环境变量只读 `PLUGIN_CORE_ENDPOINT`、`PLUGIN_AUTH_TOKEN`（`__main__.py:19`）。
  - 运行时：`runtime.py:31-134`（`Context`）、`137-148`（`Plugin` 钩子）、`151-164`（`validate_manifest`）、`167-176`（`load_manifest`）、`179-193`（`Options`）、`196-216`（`invoke` 协作超时）、`230-417`（`Runner._session` 注册/读循环/重连清理）、`419-458`（`Runner.run` 生命周期）。
  - 测试夹具：`testing.py:9-76`（`FakeHost`，真实 gRPC + 生成类型）。
  - 示例插件：`plugins/event-observer/observer.py:4-15`、`plugins/ai-reply/ai_reply.py:9-39`。
  - 宿主接线（只读参照）：`core/app/main.cpp:87-89,155-175,223-260,296-315,345-357`；`core/src/runtime/bridge_server.cpp:290-340`；`core/src/application/plugin_hub.cpp:100-230`；`core/src/runtime/plugin_process.cpp:12-107`。
- 对应设计文档：`doc/03-plugin-sdk.md`、`sdk/PLUGIN_SDK_DEPENDENCIES.md`、`sdk/python/README.md`、`proto/kiarabot/v1/plugins.proto`、`doc/18-architecture-baseline.md:318-324,403,406,416`（受保护基线，仅引用）、`doc/16-development-tasks.md` T03a/T03b/T04a/T04b/T06a/T06b/T06c/T07a/T08b、`doc/08-operations.md:5-19`。

## 2. 模块职责（依据）

- 基线/doc 自述职责：PluginSDK 是插件与 Core 的稳定边界，负责清单/版本协商、生命周期、事件订阅、消息与 Action、模型/存储/权限/任务服务、隔离/心跳/日志/错误处理（`doc/03-plugin-sdk.md:43-61`）；不负责 QQ 登录、OneBot 解析、具体 AI SDK、Core 数据库、面板页面（同文件 54-60）。基线要求「插件只能通过 SDK 访问 Core 能力」（`AGENTS.md` 架构边界；`doc/18-architecture-baseline.md:416` 称 Python 进程外 SDK 与 PluginBridge 已落地）。
- 契约来源：`proto/kiarabot/v1/plugins.proto`（清单 11-24、帧 54-131、Host 请求/响应 133-228）与 `services.proto:17-19`（`PluginBridge.Connect` 双向流）。权限/能力词表不在 proto 中枚举，实际以 Core 实现为准（`core/src/application/plugin_hub.cpp:52-68` 的 `required_permission`）。
- 依赖声明：`sdk/PLUGIN_SDK_DEPENDENCIES.md:5-23` 把本 SDK 定位为「只依赖生成协议、不导入 Core/NoneBot/数据库/模型 SDK」；该文件同时是模块缺口台账（声明未实现项在 21 行）。

## 3. 已实现能力

### 3.1 PluginBridge / Host 能力逐项映射（对应任务要求 1）

`PluginBridge` 只有一条 `Connect` 双向流（`services.proto:17-19`），能力全部由帧类型承载。

| # | proto 能力（位置） | SDK 封装/消费点 | 语义与拒绝方式 | 测试 | 状态 |
| --- | --- | --- | --- | --- | --- |
| 1 | `Connect` + `authorization: Bearer`（services.proto:17-19；plugins.proto 注释 8-9） | `runtime.py:240-245` 用 token 建流；`Options` 校验 endpoint/token（179-193） | 单流、单身份；channel 消息上限 1 MiB（240-243） | `test_runtime.py:53-75`、`test_process.py:86-155` | 有 |
| 2 | `PluginToCore.register=1`（plugins.proto:38-43,54-66） | `runtime.py:324-325`，固定上报 5 个 `supported_features` | instance_id 每次连接新 uuid（233）；manifest 原样发送 | 同上 + `test_config.py:36` | 有 |
| 3 | `CoreToPlugin.register_result=1`（plugins.proto:45-52） | `runtime.py:326-339` 校验 `required_api_version`、授权权限集合逐项相等、`enabled_features` 是本端已知子集 | 不一致 → `PERMISSION_DENIED` 并退出；拒绝 → 沿用宿主错误码（330-331） | `test_runtime.py:106-131` | 有 |
| 4 | `PluginToCore.event_ack=2` / `CoreToPlugin.error=6`（81；75-78） | `runtime.py:308-321`、`357-358` | 成功 ACK 无 error；`PluginError` 保留稳定码，其他异常脱敏 `INTERNAL`；`UncooperativeHook` → 进程硬退出 | `test_runtime.py:78-103` | 有 |
| 5 | `CoreToPlugin.event=2`（plugins.proto:71） | `runtime.py:274-321` 串行调用 `on_event(event, context)`；队列深度 2，满 → `RATE_LIMITED`（237,400-403） | 逐条 ACK；断线时未 ACK 事件由 Core 按至少一次重投（`doc/03:39-41`） | `test_runtime.py:53-75`、`test_process.py:113-138` | 有 |
| 6 | `PluginToCore.action=3`（84-87） | `Context.submit_action`（`runtime.py:59-61`） | 未协商 `action.result.v1` → 本地 `CAPABILITY_NOT_SUPPORTED`（`_call` 50-57） | `test_actions.py:16-46`、`test_closed_loop.py:53-159` | 有 |
| 7 | `CoreToPlugin.action_response=7`（104-113） | `runtime.py:372-389` | 结果/错误 oneof 与 `expected` 交叉校验；未知/畸形 → `UPSTREAM_UNAVAILABLE`；未知状态不伪造 FAILED | `test_actions.py:49-78` | 有 |
| 8 | `PluginToCore.action_query=6`（88-97） | `Context.query_action(action_id=…)` 或 `idempotency_key=`（`runtime.py:63-72`） | 二者恰好一个 + timeout>0，否则 `INVALID_ARGUMENT`；带 deadline | `test_actions.py:34-41`、`test_closed_loop.py:151-156` | 有 |
| 9 | `PluginToCore.action_cancel=7`（98-103） | `Context.cancel_action`（`runtime.py:74-79`） | 同 Action 通道；无 SDK 侧测试 | 无 | 有封装，**未验证** |
| 10 | `PluginToCore.heartbeat=4` / `CoreToPlugin.heartbeat_ack=4`（114-115） | `runtime.py:349` 定时发心跳；`355-356` 只忽略 ack | `sent_at_unix_ms` 未填、ack 时间戳不消费；仅靠 `io_timeout` 判连接 | `test_process`/`test_model` 隐式 | 有（保活，无存活判定） |
| 11 | `PluginToCore.config_ack=8` / `CoreToPlugin.config=3`（116-131,72） | `runtime.py:274-307`：校验 version/delivery_id/apply_timeout，调 `on_config`，成功才存快照并 ACK | 未协商 `plugin.config.v1` 收到 config → `CAPABILITY_NOT_SUPPORTED`（392-393）；非法版本/超时 → `INVALID_ARGUMENT`；回调失败保留旧快照 | `test_config.py:16-118` | 有 |
| 12 | `HostRequest.model_chat=10`（167-178） | `Context.model_chat`（`runtime.py:99-100`） | 需 `host.model.v1`；非 429 错误在 SDK 层强制 retryable=False（90-93），不自动重放 | `test_model.py:73-120,140-177` | 有 |
| 13 | `HostRequest.storage_get=11`（179-180） | `Context.storage_get`（`runtime.py:96-97`） | 需 `host.storage.v1`；`NOT_FOUND` 透传 | `test_storage.py:49-94`、`test_config.py:66-94` | 有 |
| 14 | `HostRequest.storage_set=12`（181-187） | `Context.storage_set`（`runtime.py:124-131`） | 幂等键由插件提供；`expires_at` 可选；SDK 不做 TTL/未来时间校验 | 同上（无 `expires_at` 用例） | 有（部分未验证） |
| 15 | `HostRequest.storage_delete=13`（188-189） | `Context.storage_delete`（`runtime.py:133-134`） | 幂等键必填 | `test_storage.py:82` | 有 |
| 16 | `HostRequest.task_create=14`（202-212） | `Context.task_create`（`runtime.py:102-116`） | `run_at`（必须 UTC aware）与 `local_time` 字典二选一，否则 `INVALID_ARGUMENT`；需 `host.tasks.v1` | `test_tasks.py:38-52,82-99`（仅 `run_at` 路径） | 有（`local_time` 未验证） |
| 17 | `HostRequest.task_cancel=15`（213） | `Context.task_cancel`（`runtime.py:121-122`） | 需幂等键 | `test_tasks.py:51,67-69` | 有 |
| 18 | `HostRequest.task_get=16`（214） | `Context.task_get`（`runtime.py:118-119`） | 无幂等键 | `test_tasks.py:52,65-66` | 有 |
| 19 | 请求关联：`request_id/trace_id/deadline`（133-143）、`HostResponse.request_id`（151-165） | `rpc()` 维护 `pending/expected`（`runtime.py:251-272`），读循环按 oneof 交叉校验（359-389） | 迟到/未知响应丢弃；超时/断线恢复为稳定错误；并发上限 64 → `RATE_LIMITED`（252-253） | `test_actions.py:34-46`、`test_storage.py:17-46` | 有 |
| 20 | 功能协商 `supported_features/enabled_features`（41-42,50-51） | 固定上报 `action.result.v1/host.storage.v1/host.model.v1/host.tasks.v1/plugin.config.v1`（`runtime.py:325`）；读循环只认已知帧 | Core 侧按能力交集启用（`core/src/runtime/bridge_server.cpp:305-337`）；未协商能力本地直接拒绝 | `test_model.py:68-120`、`test_storage.py:15-46`、`test_tasks.py:82-99`、`test_actions.py:49-78` | 有 |
| 21 | `PluginManifest` 12 字段（plugins.proto:11-24） | `validate_manifest`（`runtime.py:151-164`）+ `load_manifest`（167-176，≤64 KiB、严格 JSON） | 见 3.2 与 4 节缺口 G13 | `test_runtime.py:26-50` | 部分 |
| 22 | PluginBridge **没有** health/stop/log/动态订阅 RPC | 无封装、无协议 | — | — | **无** |
| 23 | Host **没有** conversation / permission-check / task-list 操作（`HostRequest` oneof 仅 7 项，140-149） | 无封装、无协议 | — | — | **无** |
| 24 | `PluginHeartbeat.sent_at_unix_ms`（114） | 未填（`runtime.py:349` 只设 instance_id） | Core 只校验 instance_id 后回 ack（`bridge_server.cpp:471-477`） | 无 | 字段存在但未使用 |

结论：**proto 现有的 7 个 Host 操作和 8 类插件→Core 帧全部有 SDK 封装**；缺口不在已有帧的封装，而在「协议层根本没有入口」的能力（会话、权限查询、模型流、动态订阅、健康/日志）。

### 3.2 实现细节与校验边界

- 运行时/隔离：只接受 `runtime=python-rpc` 且 `isolation=process`（`runtime.py:159-160`，Core 同样校验 `plugin_hub.cpp:123-125`）；`dependencies` 非空、`config_schema` 非空且非 `{}` → `CAPABILITY_NOT_SUPPORTED`（`runtime.py:160-161`）。
- 依赖：Python `>=3.11,<3.13`，`.python-version=3.11.18`；运行依赖 `grpcio==1.85.0`、`protobuf==6.33.6`；开发依赖 `grpcio-tools==1.81.1`、`pytest==9.2.0`、`pytest-asyncio==1.4.1`（`pyproject.toml:3-16`、`uv.lock:15-16,46-47,105-114`）。SDK 源码只 import `asyncio/logging/re/uuid/contextlib/dataclasses/datetime/pathlib/grpc/google.protobuf.json_format/kiarabot.v1`（`runtime.py:1-14`、`testing.py:2-6`、`__main__.py:1-9`），未导入 Core、NoneBot、数据库或模型 SDK——与依赖声明一致，但**没有自动检查脚本保护**（见 G19）。
- 清单校验覆盖面（12 字段）：id/version 正则 `[A-Za-z0-9_.-]{1,128}`、name 1–128、api_version 必须 `kiarabot.plugin.v1`、entrypoint 正则 `[A-Za-z_][\w.]*:[A-Za-z_]\w*` 且 ≤256、permissions/capabilities 各 ≤128 且无重复（`runtime.py:152-164`）。**未校验**：`ordered`（任意 bool 均接受，语义完全交给 Core，`plugin_hub.cpp:169`）、capabilities 是否属于已知事件词表（Core 注册时拒绝，`plugin_hub.cpp:152-158`）、version 是否语义化版本、permission 词表/作用域、entrypoint 模块是否可导入（运行时才失败）。Core 侧对 entrypoint 只查非空/≤256（`plugin_hub.cpp:120-122`），**SDK 比 Core 更严**，两侧校验强度不一致。
- 契约/错误映射：`Error` 稳定码经 `common.ErrorCode` 校验（`runtime.py:19-23`）；回调异常不含自由文本（316-320，`test_runtime.py:97` 断言私有文本不外泄）；配置回调 `min(hook_timeout, apply_timeout_ms/1000)`（290-291）。
- 生命周期：`on_start` → 连接/注册 → 事件/配置回调 → 取消流 → `on_stop('shutdown')`（`runtime.py:419-458`）；重连重新注册、重新校验权限、不缓存授权（231,338,405-408）；不可协作钩子触发 `UncooperativeHook`，`__main__.py:26-29` 用 `os._exit(1)` 硬退出，不假装 asyncio 能中断同步阻塞代码（`deps:10`）。
- 进程退出/重连：POSIX 启动器 `core/src/runtime/plugin_process.cpp:12-107` 用 `posix_spawn` + 独立进程组 + 最小环境，stdout/stderr 指向 `/dev/null`（63-65），停止 TERM→宽限→KILL+waitpid（83-107）；SDK 侧可重试错误（`UPSTREAM_UNAVAILABLE/TIMEOUT/RATE_LIMITED/CONFLICT` 等）退避后重连（`runtime.py:434-452`），不可重试错误直接退出（435-436）；Core 不重启崩溃插件（`main.cpp:345-357` 只记录 `plugin_exit`）。

### 3.3 示例插件

| 插件 | 文件/行数 | 能力 | 测试 | 完整度 |
| --- | --- | --- | --- | --- |
| event-observer | `observer.py` 15 行 / `plugin.json` 12 行 | 订阅 `event.message.received`（需 message.read）、`ordered=true`；`on_event` 只把 event_id 追加到内存列表 | `test_runtime.py:53-75` 通过真实 FakeHost 校验收发/ACK/重连；`test_process.py:53-66` 用作受管进程模板（会替换 entrypoint） | **部分**：仅最小收事件示例，无 on_start/on_stop/config/业务/插件内测试 |
| ai-reply | `ai_reply.py` 40 行 / `plugin.json` 12 行 | `/ai ` 前缀纯文本触发；storage 记录调用意图再调 `model_chat('default', …)`；成功存答案后 `submit_action` 发纯文本；重试复用 idempotency key | `test_model.py:140-177`（重放不重复调用模型，正常+超时），`test_model.py:73-120` 用真实 Core + fake HTTP Provider 校验权限/限流/畸形响应等 | **部分**：单轮、硬编码 alias `default`、硬编码 2s 超时、无会话上下文/群私聊区分/提及处理/on_config/on_start/on_stop；未纳入受管启动验收 |

## 4. 缺口清单

> 类型按简报七类；「显式拒绝」指有线路入口且返回 `CAPABILITY_NOT_SUPPORTED`（可接受），「文档承诺、无线路拒绝」指插件连请求都无法发出。

| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |
| G1 | 无插件发现/注册表：Core 只通过 `KIARABOT_PLUGIN_ID/TOKEN/PERMISSIONS/EXECUTABLE/MANIFEST` 启动**单个**插件，`plugins/` 不被扫描、无安装/启用/升级/禁用流程 | 半实现 / 未接线 | **P1** | `core/app/main.cpp:87-89,155-175`；`doc/08-operations.md:19` 自述「CLI 当前只配置一个插件」；`plugins/` 无任何运行时索引 | 基线三插件只能逐个手工切环境变量；平台「可扩展插件」主流程受限 |
| G2 | 会话管理（ConversationService get/append/clear）既无 proto 操作也无 SDK 封装，插件无法通过 Host 读写会话上下文 | 未实现（文档承诺、无线路拒绝） | **P1** | `doc/03-plugin-sdk.md:186-194`（列为 Host API）；`plugins.proto:140-149`（HostRequest 只有 7 操作）；`sdk/PLUGIN_SDK_DEPENDENCIES.md:21` 承认未实现 | 插件只能退化到 storage 自建会话状态；ai_chat 类多轮场景无标准能力 |
| G3 | 基线插件 `group_admin`、`reminder` 完全缺失；AI 插件只有一个 40 行最小示例 | 未实现 | **P1** | `plugins/` 仅 4 个文件；`doc/18-architecture-baseline.md:403` 明确「group_admin、reminder 尚未实现」 | 群管理、提醒两个功能域为 0；`task_create` 能力无任何示例使用 |
| G4 | 插件日志不可观测：Core 把子进程 stdout/stderr 重定向 `/dev/null`，SDK 也没有日志帧/上报通道 | 半实现 | **P1** | `core/src/runtime/plugin_process.cpp:63-65`；`sdk/python/README.md:25`；`doc/08-operations.md:17`；`doc/03-plugin-sdk.md:52`（责任含日志） | 插件崩溃/异常只剩 Core 一行 `plugin_exit`，排障困难；设计与实现不一致 |
| G5 | 无自动重启、无插件健康上报：插件进程退出后 Core 只记录日志继续存活，SDK 无 health/readiness 通道；控制面板无法看到插件级健康 | 未实现 | **P1**（跨模块） | `core/app/main.cpp:345-357`；`doc/02-core.md:19`（自述后续）；`doc/03-plugin-sdk.md:145-153`（目标接口含 health()）；`doc/08-operations.md:80`（「插件通过 PluginSDK 上报健康状态」） | 插件故障后业务长期不可用，无自愈；健康文档承诺未兑现 |
| G6 | CI 未自动运行 SDK 测试：workflow 仅 `workflow_dispatch`，历史 5 次均未通过；SDK 66 用例只在手动/本机 `tests/acceptance/run.sh` 中跑 | 未验证 / 未接线 | **P1**（跨模块） | `.github/workflows/ci.yml:1-19,99-113`；`tests/acceptance/run.sh:24`；`doc/16-development-tasks.md:583,588,599` | 无持续门禁；台账的 66/66 是历史本机记录，待 CI 复跑校准 |
| G7 | 权限查询未实现：没有 `PermissionService.check(subject,permission,resource)->Decision` 的协议与 SDK；只有 `Context.granted_permissions`（注册时静态自省） | 未实现（依赖文档已承认；无线路拒绝） | P2 | `doc/03-plugin-sdk.md:206-212`；`runtime.py:46-48`；`sdk/PLUGIN_SDK_DEPENDENCIES.md:21` | 插件无法对具体资源做权限决策，只能等 Core 在操作时拒绝；体验为「试错」 |
| G8 | 模型流 `stream_chat` 未实现：v1 模型契约只有非流式 | 未实现（文档显式声明） | P2 | `doc/03-plugin-sdk.md:181,236`；`plugins.proto:167-169` 注释；`sdk/PLUGIN_SDK_DEPENDENCIES.md:21` | AI 回复无法增量输出；基线 AI 体验受限 |
| G9 | 动态事件订阅未实现：订阅只能在注册时由 `capabilities` 声明，无 subscribe/unsubscribe 帧 | 未实现（deps 文档显式声明） | P2 | `doc/03-plugin-sdk.md:37`；`plugins.proto:38-43`；`sdk/PLUGIN_SDK_DEPENDENCIES.md:21` | 运行期改订阅需重连；无法按会话动态开关 |
| G10 | 依赖解析未实现：`dependencies` 非空直接 `CAPABILITY_NOT_SUPPORTED`，没有版本约束/依赖顺序/冲突检查 | 显式拒绝（可接受） | P2 | `runtime.py:159-161`；`core/src/application/plugin_hub.cpp:123-125`；但 `doc/03-plugin-sdk.md:121-128` 声称 Core「必须检查依赖是否满足」 | 依赖型插件无法安装；主设计文档给了与实现相反的承诺 |
| G11 | native/wasm 隔离未实现：`isolation` 只接受 `process`，且**省略字段即拒绝**（没有默认值） | 显式拒绝（可接受）/ 契约不一致 | P2 | `runtime.py:159-160`；`plugin_hub.cpp:123`；`doc/03-plugin-sdk.md:130`「只能是 process、native 或 wasm，默认使用 process」 | C ABI/进程内插件无路径；清单作者若信「默认 process」会踩坑 |
| G12 | 非空配置 schema 未实现：无 JSON Schema 解释器，非空 schema → `CAPABILITY_NOT_SUPPORTED`（仅空串/`{}` 可用） | 显式拒绝（可接受） | P2 | `runtime.py:160`；`plugin_hub.cpp:125`；`doc/03-plugin-sdk.md:226` | 插件配置只能无 schema 裸值；但 `doc/03-plugin-sdk.md:114` 的清单示例反而写了 `"config_schema": "config.schema.json"` |
| G13 | 清单校验不完整/两端不一致：`ordered`、capabilities 词表在 SDK 不校验；entrypoint SDK 严于 Core；version 接受任意字符组合 | 半实现 | P2 | `runtime.py:151-164`；`plugin_hub.cpp:120-125,152-158`；`plugins.proto:11-24` | 错误要到注册（甚至 Core 侧）才暴露；本地 `load_manifest` 不能作为权威校验 |
| G14 | 插件工具链缺失：`sdk/cpp/`、`sdk/typescript/`、`tools/kiara-plugin/`（创建/校验/测试/打包）不存在 | 未实现 / 文档缺失 | P2 | `doc/03-plugin-sdk.md:77-79`；`sdk/` 仅 `python`；`doc/18-architecture-baseline.md:406` 承认 | 无脚手架/打包规范；C++/TS 插件无 SDK |
| G15 | 测试夹具承诺未落地：`FakeEvent/FakeMessageService/FakeModelService/FakeStorage/FakeScheduler` 全部没有，只有一个通用 `FakeHost` | 未实现 | P2 | `doc/03-plugin-sdk.md:320-330`；`testing.py:9-76`；`sdk/python/README.md:29` | 插件作者需自行拼装假对象；文档目标 API 与实现不同 |
| G16 | 运行时参数不可配置：`python -m kiarabot_sdk` 只读 endpoint/token，`hook_timeout/io_timeout/heartbeat_interval/retry_delay` 固定默认 5/5/1/1，Core 也不透传 | 半实现 | P2 | `__main__.py:19`；`runtime.py:179-193`；`sdk/python/README.md:27`「事件回调超时应配置为大于模型调用时限」 | 无法按插件调大回调超时以容纳长模型调用；该 README 建议不可执行 |
| G17 | 心跳仅写不判：`sent_at_unix_ms` 不填，`heartbeat_ack` 只忽略，SDK 无应用层超时检测 | 半实现 | P2 | `runtime.py:349,355-356`；`plugins.proto:114-115`；`bridge_server.cpp:471-477` | 半开 TCP 场景只能等 `io_timeout` 读超时；心跳字段语义空转 |
| G18 | 插件配置下发依赖控制面：只有设置 `KIARABOT_CONTROL_CONFIG` 时才装配 `PluginConfigDelivery` 并协商 `plugin.config.v1` | 未接线（条件性） | P2 | `core/app/main.cpp:105,223-260`；`plugin_hub.cpp:200-212` | 纯 CLI 模式下 `on_config` 永远不会被调用；SDK 能力存在但实际不可用 |
| G19 | 无 SDK 导入边界自动检查：`check-boundaries.py` 只检查 C++ Core include，SDK 未导入 Core/NoneBot/DB 全靠约定 | 未接线 | P2 | `scripts/quality/check-boundaries.py:1-45`；`sdk/python/src/kiarabot_sdk/runtime.py:1-14` | 「SDK 只依赖生成协议」可被无意破坏而无门禁 |
| G20 | 测试缺口：`cancel_action`、`storage_set(expires_at)`、`task_create(local_time)`/DST、配置非法 version/delivery_id 长度/重复帧不同指纹、心跳超时/退避、队列满 `RATE_LIMITED`、`Options` 非法参数、manifest >64 KiB、`dependencies` 拒绝、`query_action` 双选/均缺 均无 SDK 用例 | 测试缺失 | P2 | `sdk/python/tests/*`（grep 无 `cancel_action/expires_at/local_time`）；`runtime.py:124-131,74-79,102-116,394-397` | 行为存在但未验证；重构回归风险 |
| G21 | ai-reply 功能单薄：仅 `/ai` 前缀、单轮、硬编码 alias `default` 与 2s 超时，无 on_start/on_stop/on_config、无群/私聊/提及处理、无会话上下文 | 半实现 | P2 | `plugins/ai-reply/ai_reply.py:9-39`；`plugins/ai-reply/plugin.json` | 只演示最短链路，不构成 `ai_chat` 业务 |
| G22 | event-observer 仅内存记录 event_id，无生命周期/配置/业务/插件内测试 | 半实现 | P2 | `plugins/event-observer/observer.py:4-15` | 最小示例成立，但无法作为插件开发模板的完整参照 |
| G23 | 示例插件无独立测试与配置目录：`doc/03-plugin-sdk.md:80` 声称 `plugins/<name>/` 含「清单、配置、实现和测试」，实际每个插件只有 2 个文件 | 文档缺失 / 测试缺失 | P2 | `plugins/event-observer/`、`plugins/ai-reply/` 文件列表；`doc/03-plugin-sdk.md:80` | 插件级回归完全依赖 `sdk/python/tests`；无配置样例 |
| G24 | ai-reply 未纳入受管进程验收：受管启动示例/env 模板只有 event-observer；ai-reply 只以内联 `Runner` 测试 | 未验证 | P2 | `tests/acceptance/core.env.example:6-11`；`test_model.py:73-120`（内联 Runner） | 实际部署 ai-reply 缺参考配置；启动/停止/环境隔离路径未被覆盖 |
| G25 | `on_stop` 只有固定 `reason='shutdown'`，无停用/失败/升级等原因，Core 也无停止原因帧 | 半实现 | P2 | `runtime.py:458`；`doc/03-plugin-sdk.md:147-153` | 插件无法按停用原因做收尾；文档的 stop(reason) 语义未展开 |
| G26 | 文档的 Host API 形态未按字面实现：没有 MessageService `send/reply`、没有 `context.models.default()`/`messages.reply()`/`on_message` | 半实现 / 文档承诺 | P2 | `doc/03-plugin-sdk.md:168-176,296-318`；`runtime.py:59-61,137-148` | 按文档目标形态写出的插件无法运行；作者需读实现才知道用 ActionEnvelope |

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/03-plugin-sdk.md:90-119` 的清单示例含 `"config_schema": "config.schema.json"` 与 capabilities `action.message.send/model.chat/conversation.read/conversation.write` | SDK 对非空 config_schema 抛 `CAPABILITY_NOT_SUPPORTED`；Core 对未知 capability 抛 `CAPABILITY_NOT_SUPPORTED` → **示例清单无法注册** | `runtime.py:160`；`plugin_hub.cpp:152-158` |
| `doc/03:130`：`isolation` 只能是 process/native/wasm，「默认使用 process」 | 只接受显式 `"process"`；省略字段即 `CAPABILITY_NOT_SUPPORTED`，无默认值 | `runtime.py:159-160`；`plugin_hub.cpp:123` |
| `doc/03:241-255` 权限词表含 `scheduler.create`、`conversation.read/write`，不含 `task.schedule/member.read/request.read/adapter.read` | Core 实际用 `task.schedule`、`member.read`、`request.read`、`adapter.read`；`scheduler.create` 全仓无实现 | `plugin_hub.cpp:52-68`；`task_scheduler.cpp:234`；`plugins.proto:203`；`plugins/ai-reply/plugin.json:9` |
| `doc/03:77-80`「SDK 组成」列 `sdk/cpp/`、`sdk/typescript/`、`tools/kiara-plugin/` | 三者都不存在（只有 `sdk/python/`） | `ls sdk`、仓库根无 `tools/`；`doc/18:406` 已承认 |
| `doc/03:320-330` 承诺 `FakeEvent/FakeMessageService/FakeModelService/FakeStorage/FakeScheduler` | 只有 `FakeHost`（真实 gRPC 通用假宿主），没有上述专用夹具 | `testing.py:9-76`；`sdk/python/README.md:29` |
| `doc/03:27`「异常转换为不含自由文本的 INTERNAL」 | 回调抛 `PluginError` 时保留稳定码与 retryable，只有非 PluginError 才是 INTERNAL；同页 342 行在配置回调处又写「PluginError 保留稳定码」——**文档内部口径不一致** | `runtime.py:316-320`；`doc/03:342`；`sdk/python/README.md:17` 同样笼统 |
| `doc/03:186-212,214-220` 把 ConversationService、PermissionService、SchedulerService.list 作为 Host API 描述，未标「未实现」 | 协议层完全没有这些消息；`sdk/PLUGIN_SDK_DEPENDENCIES.md:21` 却明确列为「尚未实现」——**两份文档口径不同** | `plugins.proto:140-149`；`sdk/PLUGIN_SDK_DEPENDENCIES.md:21` |
| `doc/03:121-128`「Core 在启用前必须检查：…依赖是否满足；配置 Schema 是否有效」 | 依赖非空/配置 schema 非空一律 `CAPABILITY_NOT_SUPPORTED`；没有依赖解析与 schema 解释器 | `runtime.py:159-161`；`plugin_hub.cpp:123-125`；`doc/03:226`（同页又承认只支持空 schema） |
| `sdk/PLUGIN_SDK_DEPENDENCIES.md:18-19`「T06b：…待受控 Provider 和 SDK 接线；T06c：…待持久化调度执行和 SDK 接线」 | T06b/T06c 已实现：`Context.model_chat`、`Context.task_create/get/cancel` 存在并有测试；`doc/03:5-11` 与 `doc/16:205-235` 均记「已完成并验收」 | `runtime.py:99-134`；`test_model.py`、`test_tasks.py`；**该依赖文件已过期** |
| `sdk/PLUGIN_SDK_DEPENDENCIES.md:11`「FakeHost 仅模拟注册/事件/心跳」 | `FakeHost` 还支持 `action/action_query/action_cancel`、`host_request`、`config_ack` 队列与转发 | `testing.py:12-19,56-61`；`sdk/python/README.md:29` |
| `sdk/python/README.md:27`「事件回调超时应配置为大于模型调用时限」 | `python -m kiarabot_sdk` 不读任何 hook_timeout 环境变量，固定 5.0s；Core 也不透传 | `__main__.py:19`；`runtime.py:183-193` |
| `doc/16:583,588,599` 台账自述「SDK 66/66 … 以上为本机历史记录；远端 CI 尚未通过」「自 16191cb 起暂停自动触发」 | 静态枚举恰为 66 个用例（一致）；但 CI 确为手动触发，无法据台账宣称持续通过 | `.github/workflows/ci.yml:1-19`；`tests/acceptance/run.sh:24`；`doc/16:599` |
| `doc/16:148` T03b 允许修改「tools/ 插件工具」且状态「已实现」 | 没有任何 `tools/` 目录或插件工具实现/验收项；T03b 验收清单也未包含工具链 | `ls tools` 失败；`doc/16:144-158`；`doc/03:79` |
| `doc/18:416` 称「清单权限、事件 ACK 和 Host 能力协商已接线」 | 与代码总体一致（本报告验证）；但未覆盖单插件 CLI、配置下发依赖 control config、无重启/健康等限制 | `main.cpp:105,155-175,223-260,345-357` |

## 6. 跨模块影响（只陈述，不替其他模块下结论）

- **Core**（待 Core 模块确认）：5 个 feature 的协商/启用由 `bridge_server.cpp:305-337` 与 `plugin_hub.cpp:200-212` 实现；插件策略只能由 `KIARABOT_PLUGIN_PERMISSIONS` 构造单个 `PluginPolicy`（`main.cpp:155-175`）；配置下发仅在 `KIARABOT_CONTROL_CONFIG` 存在时装配（`main.cpp:105,223-260`）；插件崩溃不重启（`main.cpp:345-357`）；插件 stdout/stderr 丢弃（`plugin_process.cpp:63-65`）。
- **契约（proto/）**（待 contracts 模块确认）：`PluginManifest.config_schema` 为字符串但无非空语义；缺少 conversation/permission-check/streaming/订阅管理消息；`PluginHeartbeat.sent_at_unix_ms` 全链路未使用。能力/权限词表不在 proto 枚举，实际由 Core 硬编码，存在漂移面（G13、第 5 节第 3 行）。
- **Action**（待 core-runtime 确认）：Core 只实现 send_message 纯文本子集，影响 `ai-reply` 与后续插件可发送的内容；SDK `submit_action` 本身是通用 ActionEnvelope 提交。
- **控制 API / 面板**（待 control-api/panel 确认）：插件管理目前只消费注册后的 `$control.manifests` 快照（`plugin_hub.cpp:195`）与配置下发 REST（`doc/08-operations.md:220`）；无安装/启用/升级、无插件健康聚合，与 G1/G5 相关。
- **基础设施**（待 infrastructure 确认）：模型 Provider（HTTPS/回环、30s 上限）、存储 TTL/配额、调度时区由基础设施/Core 提供；SDK 的 Host deadline 上限 300s 与 `core/.../host_validation.hpp` 的 300s 校验一致。
- **CI/CD**（待 ci-cd 模块确认）：SDK 测试已写入 `tests/acceptance/run.sh:24`，但 workflow 自动触发暂停（`.github/workflows/ci.yml:1-19`），本模块的 66 用例当前不构成持续门禁。

## 7. 覆盖结论

- 一句话结论：**PluginBridge 现有帧与 7 个 Host 操作的 Python 封装、进程外生命周期、配置闭环和真实 Core 验收测试都已完成；缺口集中在「协议没有入口」的会话/权限查询/模型流/动态订阅、插件发现与多插件运行时、日志/健康/重启、以及文档与清单校验的漂移。**
- 覆盖度估计：按 **34 个功能点**枚举（桥接与生命周期 10：Connect、注册协商、事件订阅+ACK、心跳、重连、生命周期钩子、配置闭环、进程启动/停止/硬超时、插件发现/多插件、日志/健康；Host 能力 8：Action 提交/查询/取消、存储、模型非流式、任务 create/get/cancel、会话、权限查询；未实现能力 7：动态订阅、依赖解析、模型流、native/wasm、非空配置 schema、插件工具链、C++/TS SDK；清单/边界 3：字段校验、路由/依赖/配置语义、导入边界自动检查；示例/基线 4：event-observer、ai-reply、group_admin、reminder；测试/文档 2）。**完整 11/34（约 32%），含部分覆盖 20/34（约 59%），未覆盖 14/34（约 41%）**。其中「显式拒绝 CAPABILITY_NOT_SUPPORTED」的 4 项（依赖、native/wasm、非空 schema、以及能力词表外的 capability）属于**可接受的已声明限制**；真正的问题集中在 G1-G6 六个 P1 与文档漂移。**本模块未发现由代码证据支持的 P0**：插件主流程（单进程外插件收事件/Action/Host/配置）可跑通，安全边界由 Core 认证与权限策略兜底；P1 集中在多插件运行时、会话能力、基线插件缺失、可观测性、自愈与持续验证。

## 8. 未核实事项

- 未运行 `uv sync`、`pytest`、CMake 或 `tests/acceptance/run.sh`（本机无 cmake/uv/protoc/node）；「66 个用例」是按 `pytest.mark.parametrize` 静态展开计数，未验证实际通过率；台账中的 66/66 是历史本机记录。
- 未生成/读取 `sdk/python/build/generated/**`（目录不存在，属忽略产物），无法核对生成代码与 proto 的逐字段差异；`grpcio-tools==1.81.1` 与运行时 `grpcio==1.85.0` 的兼容性未验证。
- 未动态验证 `FakeHost` 与真实 Core 的时序细节（心跳超时、队列满、重连退避、config 并发门控），只按代码与测试源码推断。
- 未验证 POSIX 启动器在 macOS 上的行为（`POSIX_SPAWN_CLOEXEC_DEFAULT` 等宏差异）及非 POSIX 平台（SDK `loop.add_signal_handler`、`posix_spawn` 均限 POSIX）。
- 未核实 `doc/16` 中 CI 历史失败的具体 job/原因，也未核实远端 Actions 当前状态（台账自述 5 次未通过、自动触发已暂停，按历史记录引用）。
- 未核实插件清单 snake_case JSON 能否被 `json_format.Parse` 接受（测试源码依赖这一点，但本机无环境执行）；未核实 `uv.lock` 中各包版本在 PyPI 的实际可用性。
- 未核实 `plugins/` 是否原本计划包含配置/测试目录，仅有 `doc/03:80` 一处文字依据；未核实作者对 `group_admin/reminder` 的功能定义细节（基线只给目录名）。
- 未核实 `Context.granted_permissions` 之外是否还有未公开的 Core→Plugin 权限查询路径（协议中未见，Core 侧未全量审计）。
