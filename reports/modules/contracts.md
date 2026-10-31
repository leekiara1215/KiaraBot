# 协议契约（kiarabot-contracts）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/node/uv/protoc/docker/redocly）。所有“测试会通过/会失败”均为读代码推断，见第 8 节。

## 1. 范围与规模

- 目录/文件：`proto/kiarabot/v1/`（`common/events/actions/adapters/plugins/services` 6 个 `.proto`）、`proto/tests/`（8 个文件）、`proto/README.md`、`api/openapi.yaml`、`api/asyncapi.yaml`。共 17 个文件。
- 规模：2885 行 = proto 572 + proto/tests 958（其中测试代码 932，README 26）+ api 1292 + proto/README 63。
- 契约面：65 个顶层 message、6 个 enum、2 个 gRPC streaming RPC；REST 25 个操作（23 业务 + 2 进程探针）；WebSocket 7 个频道 / 6 种事件。
- 关键入口与代码地图：
  - 跨语言契约：`services.proto:13-19`（AdapterBridge/PluginBridge）、`common.proto:10-23`（ErrorCode）、`events.proto:8-27`、`actions.proto:8-26`、`plugins.proto:54-79`、`adapters.proto:32-48`。
  - 服务端实现：`core/src/runtime/bridge_server.cpp:112`（AdapterService）、`:270`（PluginService）；`core/src/application/adapter_ingress.cpp`、`plugin_hub.cpp`、`action_router.cpp`、`host_dispatcher.cpp`、`task_scheduler.cpp`、`plugin_config_delivery.cpp`。
  - REST/WS 实现：`control/api/src/http_mapper.cpp:107-265`（路由）、`control/api/src/event_stream.cpp:45-195`（流）、`core/src/application/control_service.cpp`（业务）、`core/src/application/control_application.cpp:25-92`（权限策略）。
  - 门禁：`proto/tests/check_contracts.sh`、`check_enum_consistency.py`、`api_contract_test.rb`、`roundtrip.cc`、`descriptor_compatibility.cc`、`scripts/ci/check-descriptor.sh`、`scripts/ci/test-descriptor-gate.sh`、`.github/workflows/ci.yml:54-133`（contracts job）、`:198-224`（api-contracts job）。
- 对应设计文档：`proto/README.md`、`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`doc/06-control-api.md`、`doc/16-development-tasks.md`（T01b/T02a/T03a/T04a/T04b/T06a-c/T08a/T08b/T09a）、`doc/18-architecture-baseline.md`（只读）。

## 2. 模块职责（依据）

- 基线/doc 自述职责：`proto/` 只定义跨进程、跨语言契约，是唯一来源，其他客户端不得复制定义（`proto/README.md:9`、`doc/01-contracts.md:5-9`）；`api/` 是 REST（OpenAPI）和 WebSocket（AsyncAPI）的唯一契约（`doc/06-control-api.md:52-58`、`doc/01-contracts.md:29`）。契约层不实现 WebSocket 连接、平台解析、数据库、插件逻辑和面板（`doc/01-contracts.md:230-237`）。
- 契约来源：线协议 `proto/kiarabot/v1/*.proto`；HTTP `api/openapi.yaml`；WS `api/asyncapi.yaml`；运行语义 `doc/01-runtime-semantics.md`。
- 消费者（本报告只做证据核对，不替其下结论）：Core 桥接/`PluginHub`/`ActionRouter`/`HostDispatcher`/`ControlService`；`adapters/nonebot`；`sdk/python`；`control/api`；`panel`（从 `api/` 生成类型）。

## 3. 已实现能力（逐条带证据）

| # | 能力 | 证据（file:line） | 对应测试 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 6 个 proto 作为唯一跨语言契约，可生成 C++/Python 且编译为 descriptor | `CMakeLists.txt:31-121`；`sdk/python/tools/generate_proto.py:10-16`；`adapters/nonebot/tools/generate_proto.py:20-29` | CTest `proto_roundtrip`（`CMakeLists.txt:108-111`）、`check_contracts.sh:47-52` | 完整 |
| 2 | Event/Action 以 `oneof` 作为唯一类型判别，禁止独立 type 字符串 | `proto/tests/check_contracts.sh:10-15` | 静态门禁本身 | 完整 |
| 3 | `ErrorCode/HealthState/PluginState/ActionStatus` 在 proto、OpenAPI、AsyncAPI 三端同名同序 | `proto/tests/check_enum_consistency.py:9-50`；`api/openapi.yaml:633-690`；`api/asyncapi.yaml:325-346` | 同左（运行时执行） | 完整（仅 4 个枚举，见 4.5） |
| 4 | AdapterBridge：bearer 认证、注册、事件持久化 ACK（PERSISTED/DUPLICATE/REJECTED）、心跳 | `bridge_server.cpp:117-262`；`adapter_ingress.cpp:137-183,185-263`；`adapters.proto:9-67` | `core/tests/bridge_server_test.cpp`、`adapter_ingress_test.cpp`、`adapters/nonebot/tests/test_client.py` | 完整（背压语义除外，见 4.1-G2） |
| 5 | PluginBridge：注册/功能协商、订阅、逐插件投递与 ACK、心跳、终止 Error 帧 | `bridge_server.cpp:276-506`；`plugin_hub.cpp:100-222,227-328`；`plugins.proto:38-81` | `plugin_hub_test.cpp`、`plugin_delivery_test.cpp` | 完整 |
| 6 | Action：`action.result.v1` 协商、submit/query/cancel、request_id 关联、结果/错误互斥、调用方隔离幂等 | `bridge_server.cpp:305-312,454-470`；`action_router.cpp:193-479`；`plugins.proto:82-113` | `core/tests/action_router_test.cpp`、`sdk/python/tests/test_actions.py`、`adapters/nonebot/tests/test_actions.py` | 完整（仅 send_message 纯文本，见 4.2） |
| 7 | Host storage：get/set/delete、命名空间按身份绑定、TTL/限额、写幂等 | `host_dispatcher.cpp:104-241`；`host_validation.hpp:47-79`；`plugins.proto:179-189` | `host_dispatcher_test.cpp`、`sdk/python/tests/test_storage.py` | 完整 |
| 8 | Host model：非流式 model_chat、角色校验、token 回填、错误码映射 | `host_dispatcher.cpp:38-103`；`host_validation.hpp:54-64`；`infrastructure/src/http_model_provider.cpp:193-194` | `model_host_test.cpp`、`http_model_provider_test.cpp` | 完整 |
| 9 | Host tasks：create/get/cancel、run_at/local_time、ScheduledTaskState、TimerTriggered 入队 | `task_scheduler.cpp:121-311,313-424`；`plugins.proto:191-228` | `task_scheduler_test.cpp`、`sdk/python/tests/test_tasks.py` | 完整 |
| 10 | Plugin config 下发闭环（`plugin.config.v1`，PluginConfig/PluginConfigAck） | `plugins.proto:114-131`；`plugin_config_delivery.cpp:42-141`；`bridge_server.cpp:377-441` | `plugin_delivery_test.cpp`、`control/api/tests/test_config_delivery.py`、`sdk/python/tests/test_config.py` | 完整 |
| 11 | REST：23 个业务操作 + 2 个探针全部映射到 in-process 应用端口，含 RBAC、幂等、分页、错误映射 | `control/api/src/http_mapper.cpp:107-265`；`control_service.cpp:200-586`；`control_application.cpp:25-180` | `control/api/tests/`（16 个 pytest）、`control_service_test.cpp`、`control_application_test.cpp` | 完整（`/actions` 两路径见 4.3） |
| 12 | WS：6 个频道（control/health/logs/adapters/plugins/tasks）的 hello/resume、SESSION_READY、补放、RESYNC_REQUIRED、关闭码 | `control/api/src/event_stream.cpp:8-195`；`http_server.cpp:19-162` | `control/api/tests/test_stream.py` | 部分（messages 缺失、health 无动态事件，见 4.4） |
| 13 | Descriptor 语义兼容工具（文件/消息/字段/oneof/嵌套/extension/reserved/enum/service+method 签名与 options） | `proto/tests/descriptor_compatibility.cc:272-378` | `scripts/ci/test-descriptor-gate.sh`（2 例） | 完整（基线比较未生效，见 4.5） |
| 14 | C++ 跨语言 round-trip、字段编号断言、未知枚举保留 | `proto/tests/roundtrip.cc:11-189` | CTest `proto_roundtrip` | 完整（抽样覆盖） |
| 15 | OpenAPI lint / AsyncAPI parser 校验 | `.github/workflows/ci.yml:198-224`；`scripts/ci/validate-asyncapi.cjs` | 同左 | 完整（CI 仅手动，见 4.5） |

## 4. 缺口清单

### 4.1 缺口汇总

| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |
| G1 | 契约兼容门禁实际空转：CI 仅手动触发，且 descriptor 基线默认 `origin/main`＝当前 HEAD；上传的 descriptor 从未被下游消费 | 未验证 | **P0** | `.github/workflows/ci.yml:3-17,8-13,123,127-132`；`scripts/ci/check-descriptor.sh:5-27` | doc/01 验收“协议变更必须有兼容性测试”未真正生效，不兼容 proto 变更可合入而不被发现（跨模块，待 ci-cd 确认） |
| G2 | 背压 ACK 与契约相反：文档要求队列满返回**可重试** RATE_LIMITED 并关闭流，实现 `retryable=false`，适配器据此把事件标记 failed | 契约不一致 | **P1** | `doc/01-runtime-semantics.md:119`、`doc/01-contracts.md:263`；`adapter_ingress.cpp:26-31,242`；`bridge_server.cpp:253-255`；`client.py:110-120`；测试期望 `adapter_ingress_test.cpp:124,203-207` | 队列满时至少一次投递可能丢事件/误判失败；测试断言与实现静态矛盾（见 8） |
| G3 | AsyncAPI `messages` 频道仅契约：无投影、无 `message.updated` 事件、且没有角色能持有 `message.read` | 未实现 | **P1** | `api/asyncapi.yaml:126-139,401-410,446-452`；`event_stream.cpp:8-15,128-151`；`control_service.cpp:716-739`；`control_application.cpp:86-92`；`control_application_test.cpp:70`；`control/api/tests/test_stream.py:58-64` | 面板即便监听 `message.updated`（`panel/src/stream/control-stream.ts:64`）也永远收不到；契约承诺的频道无法使用 |
| G4 | Action 类型仅实现 1/7：6 个 payload 显式 CAPABILITY_NOT_SUPPORTED；`SendMessage.reply/reply_to_message_id/segments` 亦拒绝 | 半实现 | **P1** | `actions.proto:17-40`；`action_router.cpp:110-120`；`adapters/nonebot/src/kiarabot_adapter_nonebot/actions.py:44-49`；`doc/01-contracts.md:84-94` | 契约列为“第一批 Action”的 7 类中 6 类不可用；消息段能力名存实亡 |
| G5 | 7 种事件中 5 种无生产者（MessageSent/GroupMemberChanged/FriendRequest/AdapterStatusChanged/PluginStatusChanged） | 未实现 | **P1** | `events.proto:18-27`；唯一适配器仅注册 `event.message_received`（`client.py:49-57`）、仅映射 MessageReceived（`mapper.py:60`）；Core 仅有接收/校验与订阅映射（`adapter_ingress.cpp:63-107`、`plugin_hub.cpp:37-52`） | 事件契约 5/7 只有下行消费端，无产生端；插件订阅这些事件永远收不到数据 |
| G6 | REST Action 查询结构性不可用：只查 `owner=="control/"+user`，全仓库无代码创建该 owner；且无 7 天保留与 deadline→EXPIRED | 未接线 | **P1** | `control_service.cpp:484-514`（owner 匹配 489，无保留期/过期处理）；全仓 grep 仅此一处 `"control/"`；`api/openapi.yaml:449-482,691-707`；`doc/06-control-api.md:161` | `/actions/{actionId}` 与 `/actions/by-idempotency-key` 对所有真实 Action 恒 404；契约声明的终态保留语义未实现 |
| G7 | 契约门禁覆盖不足：enum 仅比 4/9 且只比名称；负向夹具仅 2 例（另一套 6 例脚本未接线）；Python 旧 reader 测试未接线 | 未验证/测试缺失 | **P1** | `check_enum_consistency.py:9-14`；`scripts/ci/test-descriptor-gate.sh:19-46`；`proto/tests/descriptor_compatibility_test.sh:29-68`（无调用方）；`proto/tests/action_contract_test.py`（仅 `proto/tests/README.md:1-3` 提及） | 枚举数值漂移、enum 重编号、oneof 改名、service 签名变化、Python 旧端行为无法被自动门禁覆盖 |
| G8 | `/groups` 永远返回空：`resources()` 无 Groups 分支 | 未实现 | P2 | `control_service.cpp:587-699`（无 Group 分支）；`doc/06-control-api.md:94`；`api/openapi.yaml:398-414,777-784` | 面板群组/角色/权限视图无数据源 |
| G9 | `health.updated` 永不产生：流内 health 投影固定 `HEALTHY` 单组件 | 半实现 | P2 | `control_service.cpp:716-722`；`event_stream.cpp:11-14,133-151`；`api/asyncapi.yaml:411-417` | `/ws/health` 只会停在 SESSION_READY，health 事件不可观测 |
| G10 | 两份契约互相矛盾：OpenAPI `admin:['*']` 暗示全权，AsyncAPI 明确“含 admin 也不默认 message.read”，代码对 admin 也拒绝该权限 | 契约不一致 | P2 | `api/openapi.yaml:13-19`（admin `'*'`）vs `api/asyncapi.yaml:21`；`control_application.cpp:85-92`；`api_contract_test.rb:116-121`（把 `'*'` 当作全权） | 客户端/门禁对“admin 是否有 message.read”得出相反结论 |
| G11 | OpenAPI PluginManifest 与实现不符：advertise `cpp-rpc/cpp-native/native/wasm`，实现仅接受 `python-rpc/process`；schema 缺 entrypoint/capabilities/dependencies/config_schema | 契约不一致/半实现 | P2 | `api/openapi.yaml:666-677`；`plugin_hub.cpp:117-127`；`control_service.cpp:432-442`；`plugins.proto:11-24` | `/plugins/validate` 对 schema 声明的合法值恒报 CAPABILITY_NOT_SUPPORTED；请求里带 openapi 未列字段会被 proto JSON 解析拒绝 |
| G12 | 死字段/死消息：proto `Health`、`PageRequest/PageInfo`、`Account.display_name`、`Error.details`、`ActionEnvelope.target.user_id/group_id`、`PluginStatusChanged.lifecycle_state`、`MessageSegment.AUDIO/VIDEO/FILE/LOCATION`、`ConversationRef.CHANNEL` 无生产/消费 | 未实现 | P2 | `common.proto:40,64,75-80,101-117`；`adapters.proto:20`；`actions.proto:15`；`events.proto:83`；`mapper.py:94-95`；`action_router.cpp:116-120`（全仓 grep 无使用点） | 契约面大于实现面；跨语言生成物包含无实际语义的字段，易误认为已支持 |
| G13 | `PluginState` STARTING/STOPPING 无运行时生产；DISABLED/STOPPED 由控制投影合成 | 半实现 | P2 | `plugins.proto:26-36`；`runtime_engine.hpp:538-610`（仅 ready/running/unavailable/failed）；`control_service.cpp:122-141` | 状态词表存在但状态机不产生；客户端状态展示依赖投影拼接 |
| G14 | doc 声明的 gRPC 错误映射未实现：CONFLICT→ALREADY_EXISTS/ABORTED 等状态在代码中不存在，业务错误走 in-band Error 帧 | 契约不一致 | P2 | `doc/01-contracts.md:188`；`bridge_server.cpp:19-24,86-106,135-140`（仅 UNAUTHENTICATED/PERMISSION_DENIED/INVALID_ARGUMENT/INTERNAL/CANCELLED） | gRPC 客户端按文档映射代码会拿不到预期状态；协议实际语义与文档不同 |
| G15 | AsyncAPI securityScheme 只定义未绑定；关闭码 1013 未进契约 | 契约不一致 | P2 | `api/asyncapi.yaml:141-145`（bearerAuth 无引用）、`:28`（close_codes 无 1013）；`http_server.cpp:94-95` | 契约无法表达首帧 token 认证；慢消费者关闭码为未记录行为 |
| G16 | doc 字段规则未实施：Action deadline 无 ≤5 分钟校验；ID 上限 128/256 分叉；Action ID 字符集与适配器 ID 字符集不一致 | 契约不一致 | P2 | `doc/01-contracts.md:219`；`action_router.cpp:26-36,104-109`；`adapter_ingress.cpp:18-25`；`host_validation.hpp:26-28`；`mapper.py:45`（`group:` 前缀） | 超长 deadline 的 Action 永不 EXPIRED；同一 ID 在不同边界被不同规则校验 |
| G17 | Host 未知 operation 错误码与文档不符：文档要求 CAPABILITY_NOT_SUPPORTED，实现先经 validate 返回 INVALID_ARGUMENT | 契约不一致 | P2 | `doc/01-contracts.md:155`；`host_validation.hpp:97-99`；`host_dispatcher.cpp:107-121` | 新版本客户端向旧 Core 发新 Host 操作得到 400 类错误而非“能力不支持” |
| G18 | `RegisterAdapterResponse.heartbeat_interval_seconds` 服务端已设置但唯一适配器客户端不读取 | 未接线 | P2 | `adapter_ingress.cpp:180`；`client.py:87-91`（只校验 protocol_version/instance） | 心跳间隔契约字段是单向摆设，服务端无法据此调节适配器 |
| G19 | 契约未定义权限/能力词表：manifest `capabilities`/`permissions` 为自由字符串，服务端硬编码映射 | 文档缺失/契约缺失 | P2 | `plugins.proto:18-19`；`plugin_hub.cpp:53-70`；`sdk/python/src/kiarabot_sdk/runtime.py:151-165` | `message.read/member.read/request.read` 等权限名无单一契约来源，拼写错误只会得到 PERMISSION_DENIED |
| G20 | 无优雅停机/drain 帧：`services.proto` 仅 Connect，Core 关闭即取消流 | 契约缺失 | P2 | `services.proto:13-19`；`bridge_server.cpp:551-558`；`doc/01-contracts.md:255`（关闭服务取消等待流） | 运维重启与故障不可区分，客户端只能按未知结果重连（跨模块，待 operations 确认） |
| G21 | proto 无任何 `reserved`/`deprecated` 声明，与 README 的“先 deprecated 再下主版本移除、编号永不复用”流程缺少可执行标记 | 文档缺失/契约缺失 | P2 | `proto/README.md:42-46`；`proto/kiarabot/v1/*.proto`（grep 0 处） | 未来 v2 删除字段时无显式保留位，复用编号只依赖人工纪律 |

### 4.2 重点 1：proto 已声明但下游无实现 / 半实现（逐条）

判定口径：在全仓库（含 Core、adapters、sdk、control、infrastructure、plugins，排除 build）grep 使用点；“仅契约”＝除 `proto/` 及测试语料外无生产代码引用。

| # | 声明项 | 声明位置 | 下游使用点（grep 结果） | 判定 |
| --- | --- | --- | --- | --- |
| 1 | `ActionEnvelope.delete_message/react_message/mute_member/kick_member/approve_request/schedule_task` | `actions.proto:19-24` | Core 对非 send_message 一律 `CAPABILITY_NOT_SUPPORTED`（`action_router.cpp:110-114`）；适配器同样拒绝（`actions.py:44-45`） | 仅契约、无实现 |
| 2 | `SendMessage.reply`、`SendMessage.reply_to_message_id`、`Message.message.segments`（非空） | `actions.proto:28-33` | 双端显式拒绝（`action_router.cpp:116-120`、`actions.py:47-49`） | 仅契约、无实现（显式拒绝） |
| 3 | `MessageSegment.Type.AUDIO/VIDEO/FILE/LOCATION` | `common.proto:75-80` | 适配方把非 text/image/at/reply 段统一映射 `UNKNOWN`（`mapper.py:94-95`），全仓无赋值 AUDIO/VIDEO/FILE/LOCATION | 仅契约、无实现 |
| 4 | `ConversationRef.Type.CHANNEL` | `common.proto:64` | Core/适配器仅允许 PRIVATE/GROUP（`action_router.cpp:117-118`、`actions.py:48`） | 仅契约、无实现（显式拒绝） |
| 5 | 事件 `MessageSent` | `events.proto:20,38-43` | Core 校验/订阅映射存在（`adapter_ingress.cpp:73-78`、`plugin_hub.cpp:41-42`），但全仓无生产者（适配器只产生 MessageReceived） | 仅消费端、无生产端 |
| 6 | 事件 `GroupMemberChanged` | `events.proto:21,45-56` | Core 校验/订阅存在（`plugin_hub.cpp:43-44`），无生产者 | 仅消费端、无生产端 |
| 7 | 事件 `FriendRequest` | `events.proto:22,58-61` | Core 校验/订阅存在（`plugin_hub.cpp:45-46`），无生产者 | 仅消费端、无生产端 |
| 8 | 事件 `AdapterStatusChanged` | `events.proto:24,70-74` | Core 校验/订阅存在（`plugin_hub.cpp:47-48`），无生产者 | 仅消费端、无生产端 |
| 9 | 事件 `PluginStatusChanged` 与 `lifecycle_state` | `events.proto:25,76-83` | 适配器被禁止发送（`adapter_ingress.cpp:102-104`）；Core 无产生点；插件无法订阅（`plugin_hub.cpp:53-70` 无该能力映射）；全仓无 `lifecycle_state` 引用 | 仅契约、无实现 |
| 10 | `MessageReceived` 之外的 EventEnvelope 受众 | `events.proto:18-26` | 仅 MessageReceived 有端到端闭环与测试（`sdk/python/tests/test_closed_loop.py:62-74`） | 5/7 事件未闭环 |
| 11 | `common.proto` `Health` | `common.proto:111-117` | 生产代码无 `v1::Health` 使用；REST 健康用 OpenAPI `Health` + `ControlHealthResult`（`control_service.cpp:443-483`） | 仅契约、无实现 |
| 12 | `common.proto` `PageRequest`/`PageInfo` | `common.proto:101-109` | 全仓无使用；REST 分页用自建 `ControlList`/`ControlPage`（`control_application.hpp:45-50,96-99`） | 仅契约、无实现 |
| 13 | `Account.display_name` | `adapters.proto:20` | 全仓无赋值/读取；适配器只给 account_id/enabled（`client.py:53`） | 仅契约、无实现 |
| 14 | `Error.details` | `common.proto:40` | 生产代码无 `mutable_details`/`set_details`；仅测试写入 | 仅契约、无实现 |
| 15 | `ActionEnvelope.target`（Subject 的 `user_id/group_id`） | `actions.proto:15` | 仅使用 `target().conversation_id()`（`action_router.cpp:123`） | 字段半实现 |
| 16 | `RegisterAdapterResponse.heartbeat_interval_seconds` | `adapters.proto:28` | 服务端设置 30（`adapter_ingress.cpp:180`）；客户端不读取（`client.py:87-91`） | 半实现（未接线） |
| 17 | `PluginManifest.config_schema`、`dependencies` | `plugins.proto:20-21` | 非空即 `CAPABILITY_NOT_SUPPORTED`（`plugin_hub.cpp:123-127`、`sdk/runtime.py:160-161`） | 显式不支持 |
| 18 | OpenAPI PluginManifest 的 `runtime` 枚举值 `cpp-rpc/cpp-native`、`isolation` 的 `native/wasm` | `api/openapi.yaml:674-675` | 实现仅接受 python-rpc/process（`plugin_hub.cpp:123-127`） | 契约声明但无实现 |
| 19 | `PluginState.STARTING/STOPPING`（及 STOPPED 的真实状态） | `plugins.proto:28-33` | 运行时只产生 ready/running/unavailable/failed（`runtime_engine.hpp:538-610`）；控制投影合成 DISABLED/STOPPED（`control_service.cpp:122-141`） | 枚举值无生产 |
| 20 | `ScheduledTaskState` | `plugins.proto:215-223` | 全部 1–6 与内部 TaskState 数值对齐（`runtime_types.hpp:155-162`、`task_scheduler.cpp:68-74`） | 已实现 |
| 21 | RPC `AdapterBridge.Connect` / `PluginBridge.Connect` | `services.proto:13-19` | 两个 Service 均已注册并实现（`bridge_server.cpp:112,270,539-545`） | 已实现 |

### 4.3 重点 2：OpenAPI 路径覆盖表（25 操作全部有路由）

路由判定位置：`control/api/src/http_mapper.cpp:107-265`；业务实现在 `core/src/application/control_service.cpp`；权限在 `control_application.cpp:25-78`。全部 25 个操作在编译后的 Core 策略中存在（`api_contract_test.rb:100-131` 会按 `--dump-policy` 计数核对，本机不能运行）。

| path | method | operationId | 实现位置 | 状态 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `/health/live` | GET | getProcessLiveness | `http_mapper.cpp:143-154` | 可用 | 探针；不访问存储 |
| `/health/ready` | GET | getProcessReadiness | `http_mapper.cpp:147-153`；`control_service.cpp:184-199` | 可用 | 写事务探针 |
| `/auth/me` | GET | getCurrentPrincipal | `http_mapper.cpp:171`；`control_service.cpp:395-396` | 可用 | |
| `/auth/sessions` | GET | listSessions | `http_mapper.cpp:213-237`；`control_service.cpp:593-607` | 可用 | |
| `/auth/sessions/{sessionId}/revoke` | POST | revokeSession | `http_mapper.cpp:185-187`；`control_service.cpp:397-402` | 可用 | |
| `/auth/login` | POST | login | `http_mapper.cpp:166-168`；`control_service.cpp:251-278,369-394` | 可用 | |
| `/auth/refresh` | POST | refreshToken | `http_mapper.cpp:169-170`；`control_service.cpp:279-296,369-394` | 可用 | |
| `/health` | GET | getHealth | `http_mapper.cpp:173-174,199-201`；`control_service.cpp:443-483` | 可用 | 组件=storage/adapter/plugins |
| `/adapters` | GET | listAdapters | `control_service.cpp:635-640` | 可用（依赖配置） | 未配置 adapter_id 时为空，不伪造 |
| `/plugins` | GET | listPlugins | `control_service.cpp:608-630` | 可用 | 只投影公开字段 |
| `/plugins/validate` | POST | validatePlugin | `http_mapper.cpp:175-180`；`control_service.cpp:432-442` | 部分 | OpenAPI schema 与实现枚举/字段不一致（G11） |
| `/plugins/{pluginId}/enable` | POST | enablePlugin | `http_mapper.cpp:188-195`；`control_service.cpp:532-534` | 可用 | 202 仅持久化接受 |
| `/plugins/{pluginId}/disable` | POST | disablePlugin | 同上 | 可用 | |
| `/plugins/{pluginId}/config` | GET | getPluginConfig | `control_service.cpp:536-555` | 可用 | delivery 为 GET 观察值 |
| `/plugins/{pluginId}/config` | PUT | updatePluginConfig | `control_service.cpp:538-549` | 可用 | expected_version 冲突 409 |
| `/plugins/{pluginId}/health` | GET | getPluginHealth | `control_service.cpp:466-476` | 可用 | |
| `/audit` | GET | listAuditEntries | `control_service.cpp:651-663` | 可用 | |
| `/bots` | GET | listBots | `control_service.cpp:631-634` | 可用（依赖配置） | 来自适配器账号配置 |
| `/models` | GET | listModels | `control_service.cpp:641-642` | 部分 | 仅单条 default；无多 provider 列表 |
| `/groups` | GET | listGroups | 无分支（`control_service.cpp:587-699`） | **未实现** | 恒空数组（G8） |
| `/users` | GET | listUsers | `control_service.cpp:643-650` | 可用 | 仅 admin 有 user.read |
| `/tasks` | GET | listTasks | `control_service.cpp:664-673` | 可用 | |
| `/actions/by-idempotency-key` | GET | getActionByIdempotencyKey | `http_mapper.cpp:181-182`；`control_service.cpp:484-514` | **未接线** | 只查 `control/<user>` owner，恒 404（G6） |
| `/actions/{actionId}` | GET | getActionResult | `http_mapper.cpp:210-212`；`control_service.cpp:484-514` | **未接线** | 同上；无 7 天保留/EXPIRED 迁移 |
| `/logs` | GET | listLogs | `control_service.cpp:651-663,675-684` | 可用（依赖配置） | 需 `options_.logs`（`core/app/main.cpp:229`，仅在 sqlite 组合下接线） |

### 4.4 重点 2：AsyncAPI 频道/事件覆盖表

| 频道 | 声明 | 服务端实现 | 状态 | 证据 |
| --- | --- | --- | --- | --- |
| `/ws/control` | 聚合全部事件 | 任意 `control` 会话按权限推送 kEvents 全部事件 | 可用 | `event_stream.cpp:19-29,128-151`；`test_stream.py:18-56` |
| `/ws/health` | `health.updated` | 频道与权限存在，但投影固定 `HEALTHY`，事件永不产生 | 部分 | `event_stream.cpp:11-14`；`control_service.cpp:716-722` |
| `/ws/logs` | `log.updated`（payload=OpenAPI LogEntry） | 已实现 | 可用（依赖日志源配置） | `event_stream.cpp:8-15`；`control_service.cpp:675-684`；`test_operations.py:73` |
| `/ws/adapters` | `adapter.updated` | 已实现（依赖 adapter 配置） | 可用 | `control_service.cpp:635-640`；`test_stream.py` |
| `/ws/plugins` | `plugin.updated` | 已实现 | 可用 | `test_stream.py:18-56` |
| `/ws/tasks` | `task.updated` | 已实现 | 可用 | `control_service.cpp:664-673` |
| `/ws/messages` | `message.updated` | 无投影、无事件、无角色可获权 | **未实现** | G3 证据行 |
| 事件 `message.updated` | `MessageEnvelope/MessagePayload` | 无生产 | 未实现 | `api/asyncapi.yaml:446-452`；`event_stream.cpp:11-15` |
| 事件 `plugin.updated` 的 `health_state` | payload 允许 state 或 health_state | 只推送 `state` | 部分 | `event_stream.cpp:140-143` |

补充：AsyncAPI 中 `Error`、`ActionStatus`、`LogEnvelope`、`LogPayload` 四个组件未被任何 message 引用（`api/asyncapi.yaml:293-300,361-372,418-424`），属死 schema；`test_stream.py:58-64` 甚至断言 admin 订阅 messages 返回 4403，固定了当前“频道不可用”的行为。

### 4.5 重点 3：契约门禁实际检查了哪些、漏了哪些

实际执行的检查（按调用链）：

| 门禁 | 位置 | 检查内容 | 是否在 CI 自动执行 |
| --- | --- | --- | --- |
| 静态结构检查 | `proto/tests/check_contracts.sh:10-40` | Event/Action 只用 oneof 判别；OpenAPI 含 security/bearerAuth/Idempotency-Key/trace_id/retryable；AsyncAPI 无根 security、hello/resume/resync 引用、messageId 唯一 | 是（经 `scripts/ci/check-descriptor.sh:26-27`；CI 本身仅手动） |
| 枚举一致性 | `check_enum_consistency.py:9-50` | 仅 `ErrorCode/HealthState/PluginState/ActionStatus` 4 个枚举，仅比较名称顺序 | 是 |
| proto 编译 | `check_contracts.sh:47-52` | 6 个 proto + imports 生成 descriptor（无 protoc 时 exit 2） | 是 |
| 基线 descriptor diff | `descriptor_compatibility.cc:272-378`；`check-descriptor.sh:5-27` | 文件/消息/字段全元数据/oneof/嵌套/extension/reserved/enum/service 方法签名与 options | 是（但见下方漏检 1） |
| 负向夹具 | `scripts/ci/test-descriptor-gate.sh:19-46` | 字段编号复用被拒、纯新增通过（2 例） | 是 |
| 更全负向夹具 | `proto/tests/descriptor_compatibility_test.sh:29-68` | additions/字段类型/字段删除/enum 重编号/oneof 改名/service 签名 6 例 | **否**（全仓无调用方） |
| C++ round-trip | `proto/tests/roundtrip.cc:11-189` | 抽样消息序列化往返、关键字段编号（event_ack=4/error=6/action=3/action_query=6/action_response=7）、PluginState 数值顺序、未知枚举保留 | 是（CTest `proto_roundtrip`） |
| Python 旧 reader 线兼容 | `proto/tests/action_contract_test.py:14-44` | 旧 descriptor 读新消息、未知字段保留、旧服务默认不启用 feature、selector/未知状态 | **否**（仅 `proto/tests/README.md:1-3` 文档提及） |
| REST/WS 与 Core 策略一致 | `proto/tests/api_contract_test.rb:88-131` | 编译后 Core 23 操作 × 权限 × 角色 × mutating 一致、响应码齐全、分页/幂等常量、ActionResult oneOf | 是（`tests/acceptance/run.sh:22`，依赖构建产物） |
| OpenAPI/AsyncAPI 解析与 lint | `.github/workflows/ci.yml:198-224` | redocly lint + AsyncAPI parser | 是（CI 仅手动） |
| 面板消费契约 | `panel/package.json:7-8`（`npm run check:contracts`） | 生成类型与契约一致 | 是（contracts job，`ci.yml:111-118`） |

漏检（与本次核对直接相关）：

1. **基线兼容空转**：`BASE_SHA` 默认 `origin/main`（`ci.yml:9-13`），在 main 上手动触发时基线＝HEAD，diff 恒等；上传的 `descriptor-${{ github.sha }}`（`ci.yml:127-132`）从未被后续 job 当基线消费；仓库内无发布版 descriptor。→ 跨版本兼容实际未验证（G1）。
2. **enum 覆盖 4/9**：缺 `ScheduledTaskState`、`EventDeliveryMode`、`ConversationRef.Type`、`MessageSegment.Type`、`GroupMemberChanged.ChangeType`、`AdapterEventAck.Status`；且只比名称、不比对数值（数值漂移只能被基线 diff 发现，而基线空转）。→ G7。
3. **“跨文件字段编号/保留值”**：protobuf 字段号是 message 内作用域，工具按“旧消息内旧字段号仍存在”比较，因此不会漏掉同消息复用编号；但 enum 重编号、enum 别名（allow_alias）重复数值、`reserved` 范围新增/移除只做单向检查，且 v1 全仓无 `reserved`/`deprecated` 声明（G21），未来 v2 删除字段后编号复用无机器保护。
4. **openapi/asyncapi 与 proto 的一致性**：只覆盖 4 个枚举；`PluginManifest` 字段集、`ActionResult` 约束、`Health`/`LogEntry` 结构、`ScheduledTaskState` 与 REST `TaskPage` 均无交叉校验。→ G11。
5. **契约 ↔ 实现覆盖无门禁**：没有检查“每个 OpenAPI path 有路由”“每个 AsyncAPI 频道/事件有投影与生产者”“每个 proto payload/字段有实现”。G3/G5/G6/G8/G9 都属该类，现有门禁全部放行。
6. **两套负向夹具未接线**：`descriptor_compatibility_test.sh` 与 `action_contract_test.py` 无任何脚本/CI/CMake 调用（grep 仅 README/台账）。→ G7。
7. **`check_contracts.sh` 不消费 `PROTOBUF_INCLUDE_DIR`**：`check-descriptor.sh:19-25` 为基线 protoc 传了 include，但随后调用的 `check_contracts.sh:47-51` 只传 `--proto_path=$proto_dir`，WKT（`google/protobuf/struct.proto` 等）解析依赖 vcpkg 工具行为；本机无 protoc 未验证（见第 8 节）。
8. **CI 自动触发被暂停**（`ci.yml:3-17`）：所有上述门禁只有人工 `workflow_dispatch` 才跑，“CI 绿”不是当前事实。

### 4.6 重点 4：与 `doc/01-contracts.md`、`doc/01-runtime-semantics.md` 不一致（错误码/状态/幂等语义）

| # | 语义点 | 文档/契约 | 实现 | 判定 |
| --- | --- | --- | --- | --- |
| 1 | 队列满背压 ACK | “满时回复**可重试** RATE_LIMITED 并关闭本次流”（`doc/01-runtime-semantics.md:119`）；“RATE_LIMITED…可重试，retry_after_ms=1000”（`doc/01-contracts.md:263`） | `error(RATE_LIMITED)` 默认 `retryable=false`（`adapter_ingress.cpp:26-31,242`），因此不回 `retry_after_ms`、bridge 不关流（`bridge_server.cpp:253-255`） | 不一致（且测试期望 true，见 G2） |
| 2 | 未知 Host operation 错误码 | 其余 Host operation 明确 `CAPABILITY_NOT_SUPPORTED`（`doc/01-contracts.md:155`） | `validate_host_request` 对 `OPERATION_NOT_SET` 返回 `INVALID_ARGUMENT`（`host_validation.hpp:97-99`），先于 dispatch 的能力分支 | 不一致 |
| 3 | gRPC 状态映射 | `CONFLICT→ALREADY_EXISTS/ABORTED`、`RATE_LIMITED→RESOURCE_EXHAUSTED`、`TIMEOUT→DEADLINE_EXCEEDED`、其他→INTERNAL（`doc/01-contracts.md:188`） | 生产代码只构造 UNAUTHENTICATED/PERMISSION_DENIED/INVALID_ARGUMENT/INTERNAL/CANCELLED（`bridge_server.cpp` 全文 grep 无 ALREADY_EXISTS/ABORTED/RESOURCE_EXHAUSTED/DEADLINE_EXCEEDED）；业务错误以 in-band `Error` 帧承载 | 不一致（实现语义已改为 in-band） |
| 4 | 事件生命周期字段 | `lifecycle_state` 缺失/0/未知数值的完整语义（`doc/01-contracts.md:199`） | 无生产者/消费者（G12） | 契约有、实现无 |
| 5 | ActionResult 终态保留 | 终结结果保留 7 天（`doc/01-runtime-semantics.md:74`、`doc/06-control-api.md:161`、`api/openapi.yaml:694`） | ActionRouter 按 168h 过滤（`action_router.cpp:175-178`）；REST 查询通道完全没有保留期与 EXPIRED 迁移（`control_service.cpp:484-514`），且 owner 不匹配恒 404 | REST 侧不一致/不可用 |
| 6 | Action deadline 规则 | “`deadline` 最长 5 分钟”（`doc/01-contracts.md:219`） | `action_router.cpp:33-36` 只校验时间戳正范围，无 5 分钟上限；适配器按 deadline 执行（`actions.py:86-88`） | 不一致（超长 deadline 不 EXPIRED） |
| 7 | ID 长度/字符集 | “所有 ID 非空且最长 256 字节”（`doc/01-contracts.md:219`）；Action ID 字符集为字母数字点下划线连字符（`:78`） | 适配器入口 128 字符且允许 `:/@`（`adapter_ingress.cpp:18-25`）；Host/SDK 用 256（`host_validation.hpp:26-28`）；Action 用 128 且不允许 `:`（`action_router.cpp:26-31`）；会话 ID 实为 `group:`/`private:` 前缀（`mapper.py:45`） | 不一致（无统一 ID 语法契约） |
| 8 | Plugin status 事件 | 首批事件含 `PluginStatusChanged`（`doc/01-contracts.md:46-56`）；插件状态事件中独立填充健康/生命周期（`doc/06-control-api.md:169`） | 无产生端、插件不可订阅（G5/G12） | 契约有、实现无 |
| 9 | 7 类 Action | 文档把 7 类列为 Action 协议首批（`doc/01-contracts.md:84-94`） | 仅 send_message 纯文本（G4）；`doc/16-development-tasks.md:449` 自述“当前只支持群/私聊 send_message 的 plain_text” | 文档内部已自相矛盾，`doc/01` 未标注实现子集 |
| 10 | 幂等语义（其余一致项） | 调用方+幂等键作用域、同键同内容回放、异内容 CONFLICT、查询/取消未来 deadline、ACCEPTED 非终态、EXPIRED 用 TIMEOUT、CANCELLED 不要求 error、未知枚举不得判 FAILED | `action_router.cpp:193-343,418-479` 逐条符合；`action_router_test.cpp` 有对应断言 | 一致 |

### 4.7 重点 5：契约缺少的内容（业务需要但未定义/不可执行）

| # | 缺失 | 依据 | 影响/建议 | 严重度 |
| --- | --- | --- | --- | --- |
| 1 | 权限/能力名契约源（`message.read/member.read/request.read/model.chat/storage.namespace/task.schedule/message.send/action.cancel`） | `plugins.proto:18-19` 为自由字符串；`plugin_hub.cpp:53-70` 硬编码 | 建议在 proto/文档冻结词表或 enum；否则跨进程授权只靠拼写约定 | P2 |
| 2 | 优雅停机/排空帧 | `services.proto` 仅 Connect；`bridge_server.cpp:551-558` 直接取消流 | 建议增加 drain/shutdown 帧或明确“关闭＝未知结果”的客户端流程（运维重启需要） | P2 |
| 3 | Action 投递尝试编号/适配器侧幂等上下文 | `doc/01-runtime-semantics.md:56` 自认“当前协议没有投递尝试编号” | 延迟 ACK 无法区分尝试，只能按至少一次；建议补充 attempt 或明确不可用语义 | P2 |
| 4 | `message.read` 的授予路径 | `api/asyncapi.yaml:21` 要求“显式 policy”，`control_application.cpp:86-92` 三角色均无该权限且无配置入口 | 要么补配置/角色模型，要么从契约删除该频道 | P1（与 G3 同源） |
| 5 | 结构化错误 details 的键约定 | `common.proto:40` 字段存在但无生产者（G12）；REST `details` 仅空对象（`http_mapper.cpp:25`） | 字段级校验/重试提示无法被客户端稳定消费；建议删除或定义键 | P2 |
| 6 | 适配器事件类型与真实 OneBot notice 的映射契约 | 首批事件缺 `message_sent` 生产、notice 类事件只有 GroupMemberChanged/FriendRequest | 群成员变动/好友请求的真实通知没有生产者契约落地路径（G5） | P1（与 G5 同源） |
| 7 | 发布版 descriptor 基线与保留位策略 | `proto/README.md:42-46` 要求 deprecated/reserved，v1 无任何声明（G21）；仓库无 release descriptor | 跨版本兼容缺少可执行基线（G1） | P1（与 G1 同源） |
| 8 | 面板/客户端未知枚举与未知 feature 的契约测试 | `sdk/runtime.py:383-388` 对未知 ActionStatus 抛 `UPSTREAM_UNAVAILABLE`，`doc/01-contracts.md:197` 要求“客户端必须保留未知值” | 跨模块需 sdk 模块确认；建议契约测试固化未知值保留 | P2 |

## 5. 与文档/台账声明的不一致

| # | 声明（文档/台账） | 实际（代码/仓库） | 证据 |
| --- | --- | --- | --- |
| 1 | “T04a…check_contracts.sh、api_contract_test.rb、descriptor_compatibility_test.sh 及相对本仓库 HEAD 的 descriptor 检查均通过”（`doc/16-development-tasks.md:447`） | `descriptor_compatibility_test.sh` 与 `action_contract_test.py` 无任何调用方；descriptor 基线默认＝HEAD，等于自比；“均通过”为人工自述，CI 仅手动且未恢复 push/PR | `proto/tests/descriptor_compatibility_test.sh`（grep 无调用）；`ci.yml:3-17,123`；`check-descriptor.sh:5-13` |
| 2 | T09a 勾选“频道权限过滤”完成；`doc/06-control-api.md:218` “同一监听提供 … messages 频道” | messages 无投影无事件；三角色均无 `message.read`，admin 订阅被测试断言 4403 | `event_stream.cpp:8-15`；`control_application.cpp:86-92`；`test_stream.py:58-64` |
| 3 | `doc/06-control-api.md:94` “GET /groups 返回群组标识、角色和有效权限标识” | `resources()` 无 Groups 分支，恒返回空数组 | `control_service.cpp:587-699` |
| 4 | `doc/06-control-api.md:161` “终结结果保留 7 天”“按 ID/幂等键查询” | REST Action 查询 owner 恒不匹配 → 恒 404；无保留期判断 | `control_service.cpp:484-514` |
| 5 | `doc/01-contracts.md:155` 其余 Host operation 返回 CAPABILITY_NOT_SUPPORTED | `OPERATION_NOT_SET` → INVALID_ARGUMENT | `host_validation.hpp:97-99` |
| 6 | `doc/01-runtime-semantics.md:119` 队列满“可重试 RATE_LIMITED 并关闭流” | 实现 retryable=false、不关流；测试却断言 true（静态矛盾） | `adapter_ingress.cpp:26-31,242`；`bridge_server.cpp:253-255`；`adapter_ingress_test.cpp:124,203-207` |
| 7 | `proto/README.md:26,41-46` “上一版本 descriptor / 已发布字段编号 / 每次协议变更必须运行 descriptor diff 和跨语言 round-trip” | 无发布基线可读；CI 基线=当前 ref；跨语言 round-trip 只有 C++；Python 旧 reader 测试未接线 | `check-descriptor.sh:5-27`；`components`；`action_contract_test.py` 无调用方 |
| 8 | `doc/16-development-tasks.md` T04b 自述“当前只支持群/私聊 send_message 的 plain_text” vs `doc/01-contracts.md:84-94` 把 7 类列为协议能力 | 两处文档口径不同，契约未标注实现子集；代码按 T04b 的子集行为 | `actions.proto:17-40`；`action_router.cpp:110-120` |
| 9 | `doc/18-architecture-baseline.md:415`（受保护基线附录）称“Python 适配器在独立检出中，尚未迁入本仓库” | 仓库已包含 `adapters/nonebot/` 并接入 CI 验收 | 基线为历史快照，仅报告差异，不修改（`doc/18:393` 已声明不构成契约变更） |

## 6. 跨模块影响（只陈述，不替其他模块下结论）

- Core 运行时（待确认）：Action 仅 send_message 纯文本；5 类事件无生产者；`PluginStatusChanged/lifecycle_state` 无生产；背压 RATE_LIMITED 非重试（G2/G4/G5）。这些不是契约缺陷，但会让契约矩阵呈现大面积“仅契约”。
- NoneBot 适配器（待确认）：只注册 `event.message_received` 与 `action.send_message.text.v1`（`client.py:49-57`）；AUDIO/VIDEO/FILE/LOCATION 段降级 UNKNOWN；`heartbeat_interval_seconds` 未使用（G5/G12/G18）。
- 插件 SDK（待确认）：未知 ActionStatus 被映射为 `UPSTREAM_UNAVAILABLE`（`sdk/runtime.py:383-388`），与“保留未知枚举”契约不一致；`docs/01-contracts.md:197` 的兼容承诺需要 sdk 侧确认。
- 控制 API / 面板（待确认）：messages 频道与 `message.updated` 不可用（面板 `panel/src/stream/control-stream.ts:64` 已监听）；`/groups` 恒空；`/actions` 两路径恒 404（G3/G6/G8）。
- Infrastructure（待确认）：`/logs` 依赖 `RuntimeLogSink` 配置接线（`core/app/main.cpp:229`）；未接线时 REST `/logs` 与 `/ws/logs` 返回空/无事件。
- CI/CD（待确认）：契约 job、api-contracts job 仅 `workflow_dispatch`；基线自比；两套夹具未接线（G1/G7）。“CI 绿”不能作为当前契约门禁通过的证据。

## 7. 覆盖结论

- 一句话结论：**契约文件本身质量高、三端枚举/错误模型一致性有门禁且语义文档基本被实现遵守；但“契约 → 实现”覆盖明显小于契约面——Action 7 类只落 1 类、事件 7 类有 5 类无生产者、AsyncAPI `messages` 频道整条不可用、REST Action 查询恒 404，同时宣称的兼容门禁在 CI 中实际空转，故本模块应判为“契约完整度高、实现覆盖度中低、验证门禁无效”。**
- 覆盖度估计：41/71 功能点完整覆盖（约 58%），5 项部分覆盖，25 项未覆盖。口径：
  - proto 契约功能点 32 项（7 类事件、7 类 Action、消息段、Action 查询/结果、适配器/插件桥、Host storage/model/tasks、config 下发、错误/健康/分页/生命周期/字段语义、2 个 RPC；以“生产端+消费端+测试”齐全为完整）：完整 12、未覆盖 20。
  - REST 25 个操作：完整 22、未覆盖 3（`/groups`、两个 `/actions` 查询）。
  - WS 7 个频道：完整 5、部分 1（health 无事件）、未覆盖 1（messages）。
  - 契约门禁 7 项（静态检查、枚举、proto 编译、基线 diff、负向夹具、round-trip、REST/WS 策略一致性、API lint、覆盖门禁）：完整 2、部分 4、未覆盖 1。

## 8. 未核实事项

1. **无法运行任何测试/构建/工具**：本机无 cmake/node/npm/uv/protoc/docker/redocly/asyncapi parser。所有“测试会失败/通过”的结论（尤其 G2 中 `adapter_ingress_test.cpp` 与实现的静态矛盾）未经实际执行验证。
2. **G2 的执行结论**：静态读码显示 `RATE_LIMITED` 以 `retryable=false` 返回，而 `adapter_ingress_test.cpp:124,203-207` 断言 `retryable=true` 且关流；未运行 CTest，无法确认该用例目前是红还是我遗漏了另一条赋值路径。
3. **`check_contracts.sh` 的 WKT 解析**：脚本只用 `--proto_path=proto`（`check_contracts.sh:47-51`），未消费 `PROTOBUF_INCLUDE_DIR`；protoc 是否会自动解析 `google/protobuf/*.proto` 取决于 vcpkg 工具行为，未在本机验证。
4. **CI 历史是否曾绿**：workflow 明确“Automatic CI triggers stay paused until a manual run is green”（`ci.yml:3-17`），无运行记录可查；不得假设当前 CI 通过。
5. **OpenAPI/AsyncAPI lint 是否零错误**：未运行 redocly/AsyncAPI parser；仅静态阅读（例如 asyncapi 组件死 schema、securityScheme 未绑定）未经验证器确认。
6. **真实运行态数据**：`/logs`、`/adapters`、`/bots`、`/models` 是否返回数据取决于 `KIARABOT_CONTROL_CONFIG`/sqlite/适配器配置组合（`core/app/main.cpp:120-260`），未实际启动 Core 验证；因此 4.3 中“依赖配置”的行只能按代码分支判定。
7. **面板 `check:contracts`**：CI 有该步骤，但未运行，无法确认面板生成类型与当前 openapi/asyncapi 完全匹配。
8. **发布版 descriptor 是否存在**：仓库内无 release artifact；是否在仓库外有“上一版本 descriptor”未知，G1 的“空转”结论基于仓库内证据。
9. **`doc/16` 台账勾选与实际测试执行的关系**：台账是自述，未逐项核对每个 `[x]` 对应的测试是否真实存在（例如 T04a 相关两项门禁未接线），只核对了与契约直接相关的部分。
