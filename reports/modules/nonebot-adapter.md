# NoneBot 适配器（kiarabot-nonebot-adapter）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/node/uv/protoc/docker；适配器要求 Python ≥3.11，本机仅 python3.9，未创建 venv）。

## 1. 范围与规模

- 目录/文件：
  - 实现：`adapters/nonebot/src/kiarabot_adapter_nonebot/`（`__init__.py`、`__main__.py`、`client.py`、`mapper.py`、`actions.py`、`outbox.py`）、`adapters/nonebot/pyproject.toml`、`adapters/nonebot/README.md`、`adapters/nonebot/tools/generate_proto.py`、`uv.lock`。
  - 测试：`adapters/nonebot/tests/`（6 个 pytest 文件）。
  - 验收层：`tests/acceptance/`（`run.sh`、`README.md`、`real-environment.md`、`adapter.env.example`、`core.env.example`）。
- 规模：适配器源码 6 个文件 **558 行**（`__init__.py` 1 + `__main__.py` 98 + `actions.py` 112 + `client.py` 158 + `mapper.py` 98 + `outbox.py` 91）；测试 **572 行 / 6 文件**，收集 34 个用例（含参数化；见第 3 节）；`tools/generate_proto.py` 23 行；`tests/acceptance/` 5 文件 **141 行**。
- 关键入口与代码地图：
  - 进程入口/装配：`adapters/nonebot/src/kiarabot_adapter_nonebot/__main__.py:20-94`（`configure()` 读环境变量 21-22、`nonebot.init` 31-32、注册 OneBot V11 Adapter 44、outbox 45、ActionExecutor 56、消息 matcher 60-69、startup/shutdown 71-85）。
  - 事件映射：`mapper.py:29-98`（`map_message` / `_map_message`）。
  - Action 执行：`actions.py:20-112`（`ActionExecutor.validate:38-60`、`execute:62-109`）。
  - Core 长连接客户端：`client.py:18-158`（`ClientConfig:18-35`、`registration:49-57`、`connection:74-125`、`run:127-151`）。
  - 本地持久化 outbox：`outbox.py:13-91`（`enqueue:34-66`、`next_pending:68-73`、`finish:75-84`）。
  - 测试入口：`adapters/nonebot/tests/`；跨模块验收入口 `tests/acceptance/run.sh`。
  - 协议生成：`adapters/nonebot/tools/generate_proto.py:1-23`（从本仓库 `proto/` 生成到忽略目录，不落库）。
- 对应设计文档：`doc/04-nonebot-adapter.md`（主）；`doc/18-architecture-baseline.md`（第二层/第三层/第七层，只读基线）；`doc/01-contracts.md`、`doc/01-runtime-semantics.md`（确认/背压语义）；`doc/08-operations.md`（健康检查）；`doc/16-development-tasks.md` T02b/T04b/T07a/T07b；`doc/17-account-ban-risk.md`（适配器层约束）。

## 2. 模块职责（依据）

- 基线/doc 自述职责：
  - `doc/18-architecture-baseline.md:68-79`：渠道适配器实现统一 `ChannelAdapter` 接口 `connect()/disconnect()/publish_event()/send_action()/get_status()/get_capabilities()`；`doc/18:81-87` 允许内部使用 NoneBot2 / onebot-adapter-onebot / OneBot V11，C++ Core 不应感知。
  - `doc/04-nonebot-adapter.md:11-21`：本模块只做「启动 NoneBot2、注册 OneBot V11、连接 NapCat、转换事件和消息段、执行 Core 下发 Action、报告连接和平台能力」，不做业务/权限/数据库/调度/面板；`:47-67` 进程边界为 `NapCat --OneBot V11/WS--> adapter --gRPC--> Core`，适配器不共享 Core 数据库；`:126-145` 替换标准与验收标准（接收文本/图片/@、把 Action 发回群聊和私聊、断线重连、不导入业务插件、无 Core DB 访问、可用模拟 Core 单测）。
  - `doc/04:5-9`（T04b）与 `doc/04:157-161`（T02b）是当前实现范围的自我限定：Action 仅 `action.send_message.text.v1` 纯文本子集，事件仅群/私聊 + text/image/at/reply，未知段降级；outbox 10000 条/64 MiB/单条 1 MB，已确认保留 7 天，水位不回退。
  - `adapters/nonebot/README.md` 声明：正向 WebSocket、能力声明、有界 outbox、确认语义、退避、脱敏日志、Action 执行日志（`OUTBOX_PATH.actions`）。
- 契约来源：`proto/kiarabot/v1/adapters.proto`（`AdapterToCore`/`CoreToAdapter`/`AdapterEventAck`/`Heartbeat`）、`events.proto`（7 种事件、`MessageSegment` 9 型）、`actions.proto`（7 种 Action kind、`ActionResult`）、`services.proto:13-15`（`AdapterBridge.Connect`，Bearer 元数据、错误详情）；`common.proto`（错误码/健康/会话/段类型）。适配器不拥有契约，唯一来源在 `proto/`。

## 3. 已实现能力（逐条带证据）

| # | 能力 | 证据（file:line） | 对应测试 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 群/私聊消息接收 → `MessageReceived`，会话 ID `group:<gid>`/`private:<uid>`，`ConversationRef.GROUP/PRIVATE` | `mapper.py:39-46,60-65` | `test_mapping_outbox.py:15-25` | 完整 |
| 2 | 账号校验：`self_id` 必须等于 `ACCOUNT_ID`，否则 `PERMISSION_DENIED` | `mapper.py:41-42` | `test_mapping_outbox.py:44-58` | 完整 |
| 3 | 稳定 `event_id`（uuid5 over adapter/account/conversation/message_id/time），`trace_id=event_id`，`schema_version=kiarabot.v1`，`AT_LEAST_ONCE` | `mapper.py:48-53` | `test_mapping_outbox.py:19-25,61-86` | 完整 |
| 4 | 消息段：text / image（仅 `http(s)://` URL）/ at / reply；其余段 → `UNKNOWN`，不携带原始平台数据 | `mapper.py:66-95`；单事件 ≤1 MB `:96-97` | `test_mapping_outbox.py:28-41` | 部分（image 仅 URL；AUDIO/VIDEO/FILE/LOCATION 从不产生） |
| 5 | 原始 OneBot JSON 不越过协议边界（只保留稳定 ID/文本/URI） | `mapper.py:60-95` | `test_mapping_outbox.py:28-41`（断言私有字段不出现） | 完整 |
| 6 | 本地有界持久化 outbox：按 `(adapter, account, conversation)` 原子分配单调 `sequence`，SQLite `BEGIN IMMEDIATE` 落盘 | `outbox.py:34-66` | `test_mapping_outbox.py:61-86,89-100` | 完整 |
| 7 | 幂等：同 `event_id` 同内容返回原记录；同 ID 不同内容 `CONFLICT`；重启后仍可识别已确认记录 | `outbox.py:44-48` | `test_mapping_outbox.py:61-86` | 完整 |
| 8 | 确认语义：仅 `PERSISTED`/`DUPLICATE` 标 acked；非可重试 `REJECTED` 保留 failed 记录；可重试 `REJECTED` 保留 pending 并按 `retry_after_ms` 退避 | `client.py:106-122`；`outbox.py:75-84` | `test_client.py:70-110` | 部分（可重试标记从未由 Core 置位，见缺口 #2） |
| 9 | 断线/重连重投：重连重新注册、重绑新 `adapter_instance_id`，其余字段不变 | `client.py:82-104,123-125` | `test_client.py:40-41,95-100` | 完整 |
| 10 | 退避：1s 起指数增长、上限 30s、0.8–1.0 抖动；区分 gRPC 错误/`TimeoutError`/outbox 错误 | `client.py:127-151` | `test_client.py:70-110` | 完整 |
| 11 | Bearer 认证（loopback-only endpoint，token 32–4096 可打印 ASCII，不输出 repr） | `client.py:28-35,74-80` | `test_client.py:20-24,113-117` | 完整 |
| 12 | 注册：`adapter_id`/implementation/版本/`kiarabot.v1`/账号/能力；校验 accepted、协议版本、`assigned_instance_id` | `client.py:49-57,82-91` | `test_client.py:20-28,94` | 完整 |
| 13 | 能力声明：`event.message_received` v1，及（有 executor 时）`action.send_message.text.v1` v1 | `client.py:54-56` | `test_client.py:94` | 完整（仅 2 项，无状态/健康能力） |
| 14 | Action 纯文本发送（群 `send_group_msg` / 私聊 `send_private_msg`），message 为 OneBot text segment，不解释 CQ 码 | `actions.py:56-60`；`__main__.py:46-56` | `test_actions.py:22-39`（群）；`test_nonebot_process.py:38-41`（群） | 部分（私聊未测；真实会话 ID 不匹配，见缺口 #1） |
| 15 | Action 校验：action_id/idempotency_key、归属 adapter/account/instance、仅 `send_message`、拒绝 reply/segments、会话可数字化、deadline 合法 | `actions.py:38-60` | `test_actions.py:69-92` | 完整（对 T04b 子集而言） |
| 16 | 不支持 Action/消息段 → `ActionResult{status=FAILED, error.code=CAPABILITY_NOT_SUPPORTED}`，无平台副作用 | `actions.py:44-49,64-66` | `test_actions.py:69-92`（仅 delete_message/segment 两例） | 完整（实现层面；其余 kind 未单独测） |
| 17 | 执行前 deadline：已过期 → `EXPIRED`+`TIMEOUT`；平台调用 window = `min(remaining, 5s)` | `actions.py:86-97` | `test_actions.py:69-92`（deadline 分支） | 完整 |
| 18 | 执行前落盘占用（ACCEPTED），终态才回传；重启后同 `action_id` 返回原终态，不重复调用平台 | `actions.py:71-85,103-107` | `test_actions.py:22-39` | 完整 |
| 19 | 未知结果（超时/断线/取消/无效 platform id）保留 ACCEPTED，不伪造 FAILED、不重发 | `actions.py:95-102`；`client.py:68-69` | `test_actions.py:42-66` | 完整（策略层面） |
| 20 | Action 日志容量/保留：10000 条、64 MiB，终态 7 天后清理，未终态不自动删除 | `actions.py:79-82` | `test_actions.py:95-107` | 完整（容量交互缺陷见缺口 #13） |
| 21 | SQLite 故障映射 `UPSTREAM_UNAVAILABLE`；Action 日志与事件 outbox 分库（`OUTBOX_PATH` + `.actions`） | `actions.py:25-36,108-109`；`__main__.py:45,56` | `test_actions.py:95-107` | 完整 |
| 22 | 日志脱敏：只输出稳定错误码；NoneBot/Loguru 共享 sink 清空 message/exception | `__main__.py:30-42`；`client.py:137-145` | `test_client.py:101`；`test_nonebot_process.py:67` | 完整（代价见缺口 #18） |
| 23 | 关闭流程：stop 事件 + 取消连接任务 + 关闭两个 SQLite | `__main__.py:76-85` | `test_client.py:120-137`；`test_nonebot_process.py:64-66` | 完整 |
| 24 | 不导入业务插件、不访问 Core 数据库（仅本地 SQLite），协议代码由 `proto/` 生成 | 源码 import 面（`__main__.py:10-17`）；`tools/generate_proto.py:1-23` | 无专项测试（静态可证） | 完整 |
| 25 | 测试覆盖：映射/降级/稳定 ID/拒绝码、outbox 重启/序号/容量、fake gRPC 注册与确认/断线/重投/拒绝、Action 执行/幂等/未知/取消、fake OneBot WebSocket 实际发送、真实 C++ Core 集成（skipif `CORE_TEST_BINARY`） | 全部 6 个测试文件 | 34 用例（12+7+12+1+1+1，参数化计入） | 存在但本机未运行（见第 8 节） |

## 4. 缺口清单

| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |
| 1 | **Action 目标会话 ID 契约自相矛盾**：接收侧生成 `group:<gid>` / `private:<uid>`，发送侧却要求 `conversation.id` 纯数字（≤20 位）且 `target.conversation_id` 与之相等。SDK 示例插件直接把事件 `conversation` 复制进 Action | 契约不一致/半实现 | **P0** | `mapper.py:44-45,63-65`；`actions.py:50-52`；`plugins/ai-reply/ai_reply.py:30-36`；对照 `core/src/application/action_router.cpp:104-126` 仅做字符集校验 | 按文档/SDK 示例构造的真实回复 Action 会被适配器判 `INVALID_ARGUMENT`（FAILED），"把统一 Action 发送回群聊和私聊"（`doc/04:140-141`）在真实会话 ID 下不成立；唯一通过测试的是伪造数字 ID（`test_actions.py:12-19`、`test_action_pipeline.py:33`） |
| 2 | **背压/拒绝一律被视为永久失败，事件被丢弃**：Core 对适配器的拒绝错误全部 `retryable=false`（无任何置 true 的路径），含队列满 `RATE_LIMITED`；适配器仅在 `retryable=true` 时保留 pending，否则 `finish(code)` 标 failed 并前进 | 契约不一致/半实现 | **P0** | `core/src/application/adapter_ingress.cpp:26-31,195,206-213,242-245`（错误工厂默认 false）；`client.py:115-120`；`outbox.py:75-84`；对照 `doc/01-runtime-semantics.md:119`、`doc/01-contracts.md:263`、`doc/04:153` | 与 T02a 勾选项"背压不丢事件"（`doc/16:111`）冲突：队列满时消息被标 failed 且不再重投；Core 端的 retryable 背压关闭流路径（`bridge_server.cpp:253`）成为死代码 |
| 3 | **无健康/状态接口**：既不实现 compose 要求的 8081 `/health/ready`，也不发送 `AdapterStatusChanged`、不实现基线 `get_status()`；注册能力也不含健康相关项 | 未实现 | **P0** | 全 `src/` 无 health/status 上报（`grep` 仅 ack/action/outbox 状态）；`deploy/compose.staging.yml:57-69`（`CORE_ENDPOINT`/`HEALTH_PORT=8081`/curl `/health/ready`，`deploy/compose.prod.yml:51-65` 同）；`doc/18:68-79`；`doc/04:120`；`doc/08:61-80,134,138`；`doc/17:103` | compose 的 adapter 永远 unhealthy → 依赖 `adapter: service_healthy` 的 panel 永不启动（`deploy/compose.staging.yml:88-90`）；登录态失效/被踢下线/平台不可用时系统无信号；Core `/health` 与 `/adapters` 状态无数据来源 |
| 4 | **无适配器镜像/Dockerfile/console script**：仓库 `Dockerfile` 仅构建 Core；compose 默认命令 `kiarabot-adapter-nonebot` 在 `pyproject.toml` 中无 `[project.scripts]`；compose 注入的 `CORE_ENDPOINT=http://core:8080` 不是适配器读取的 `CORE_GRPC_ENDPOINT`，且适配器强制 `127.0.0.1:port` | 未实现/契约不一致 | **P1** | `Dockerfile`（仅复制 `kiarabot-core`）；`adapters/nonebot/pyproject.toml:1-24`（无 scripts）；`deploy/compose.staging.yml:57-60`；`__main__.py:20-22`；`client.py:29`；`doc/08:138,182` | 即使补齐健康接口，容器内也无法启动/连接 Core；`doc/08:138` 已自认部署骨架不可运行（T11b 未验收） |
| 5 | **事件类型覆盖窄**：仅 `post_type=message` 且 `message_type∈{group,private}`；无 notice/request/meta 处理，因此 `GroupMemberChanged`、`FriendRequest`、`AdapterStatusChanged` 永不产生（`MessageSent` 也不发）；无 `on_notice`/`on_request` 处理器，通知/请求事件静默丢弃 | 半实现 | P1 | 映射拒绝：`mapper.py:39-40`；仅注册 `on_message`：`__main__.py:60`；契约事件：`events.proto:18-26`；基线事件清单：`doc/18:95-103` | 群成员变动、好友/加群请求无法进入 Core；`ApproveRequest` 失去 request_id 来源；`doc/04:120` 的断线上报无实现 |
| 6 | **消息段覆盖窄**：仅 text/image(URL)/at/reply；file/voice(video/record)/face/forward/node/json/xml/location 等一律 UNKNOWN 且不保留内容；image 在无 http(s) URL（file/base64/CQ 字符串）时也降为 UNKNOWN；AUDIO/VIDEO/FILE/LOCATION 这些契约类型从不产生 | 半实现 | P1 | `mapper.py:74-95`；`common.proto:70-88`；`doc/04:79-86`（声称"文本、图片、回复、@、文件等"） | 图片依赖 NapCat 提供 http URL 才可用；文件/语音/视频/AI 输入类消息丢失；与 doc 声明不符 |
| 7 | **`at` 未区分 @全体**：`qq='all'` 被当作普通 mention id `'all'` 接受 | 半实现 | P2 | `mapper.py:88-90`；测试固化该行为 `test_mapping_outbox.py:30-40`；`doc/17:94`（建议 @全体默认不开放） | @全体被静默放行，插件/审计无法区分；封号风险控制缺一环 |
| 8 | **私聊临时会话（`sub_type=group`）不可回复**：私聊分支丢弃 `group_id`/`sub_type`，发送侧只传 `user_id`；OneBot 私聊临时会话需带 `group_id` | 半实现 | P2 | `mapper.py:44-46`；`actions.py:56-60` | 群临时会话私聊收到的消息无法正常回复，会被平台拒绝或错发 |
| 9 | **Action kind 覆盖 1/7**：`delete_message`/`react_message`/`mute_member`/`kick_member`/`approve_request`/`schedule_task` 均以 `CAPABILITY_NOT_SUPPORTED` 拒绝；只在测试中覆盖 `delete_message` 一例 | 半实现 | P1 | `actions.py:44-45`；`actions.proto:17-25`；`doc/04:90-101`（文档声称转换 4 类）；T04b 子集声明 `doc/04:5-9` | 契约已定义的 6 类 Action 无实现；`ApproveRequest` 还缺事件源；管理类能力为 0 |
| 10 | **平台错误不分类**：`ActionFailed` 一律映射 `UPSTREAM_UNAVAILABLE`；本地"Bot 未连接/网络异常"（非 `ActionFailed`）被 `except Exception` 归为未知 ACCEPTED，永不重发 | 半实现 | P1 | `__main__.py:46-56`；`actions.py:95-102`；`doc/17:68,102` | 限频/需验证/群不存在/被踢无法区分；明确未发生的副作用也可能永不执行且无终态（只能等 Core deadline 过期） |
| 11 | **Outbox 容量与保留冲突**：10000 条上限统计 **所有** 记录（含 7 天内已确认行），按 7 天保留折算约 1 msg/min 即打满；打满后 `enqueue` 抛 `RATE_LIMITED`，`receive` 只记一条 warning 后丢弃平台消息 | 半实现 | P1 | `outbox.py:49-52`；`__main__.py:62-69`；`doc/04:161` | 正常活跃群会周期性进入"拒收新消息"状态并静默丢失；与"有界 spool + 明确失败"承诺不符（平台无消费 ACK，不可重放） |
| 12 | **failed 记录永不清理**：仅删除 acked；永久拒绝的记录长期占用 10000 容量，无运维清理/导出工具 | 半实现 | P1 | `outbox.py:49`；`outbox.py:75-84` | 长期运行后 outbox 容量被失败记录耗尽，只能手工删库（且有丢失未决事件风险） |
| 13 | **Action 容量满时无 ActionResult**：`RATE_LIMITED` 以异常抛出（未按 ActionResult 返回），连接断开后 Core 重投同一 action 仍抛异常，直到 deadline 过期 | 半实现 | P1 | `actions.py:79-82`；`client.py:142-143`（日志标签还误写 `operation=outbox`） | Core/插件看不到明确失败码，只能等到期 EXPIRED；高 Action 量时表现为"发送无响应" |
| 14 | **无发送速率/令牌桶**：适配器发送侧不限速，重连后可能突发补发 | 未实现 | P1 | `actions.py:56-60`（直接调用）；`doc/17:101`（适配器层建议） | 平台限频/风控风险；Core 侧配额若未实现（`doc/17:66`）则无任何兜底 |
| 15 | **序列失配无重同步路径**：适配器 outbox 丢失/回退（例如只恢复 Core 备份）后 `sequence` 从 1 重来，Core 判定 `out_of_order`→`CONFLICT`（非 retryable），适配器把每一事件标 failed，持续丢消息 | 半实现 | P1 | `outbox.py:53-63`；`core/include/kiarabot/core/domain/runtime_engine.hpp:47-59`；`adapter_ingress.cpp:242-245`；`client.py:115-120`；doc 只要求"备份须同时保存两个库"（`adapters/nonebot/README.md`、`doc/08:11`） | 备份恢复演练（T07b 未做）一旦只恢复一侧即造成静默数据丢失，无自动恢复 |
| 16 | **平台→outbox 无消费 ACK**：onebot 事件进入 NoneBot 后直接入队，失败只记日志；无落盘确认、无重放 | 未实现（已自认） | P1 | `__main__.py:62-69`；`doc/04:161`；`tests/acceptance/README.md:5` | 进程在"平台已推送、outbox 未落盘"窗口崩溃会丢消息；真实可靠性只能靠 T07b 验证 |
| 17 | **心跳语义不完整**：忽略注册响应 `heartbeat_interval_seconds`（固定用 `idle_interval=1s` 且仅 outbox 为空时发）；`Heartbeat.sent_at_unix_ms` 未填；Core 不校验心跳超时 → 心跳不构成存活判定 | 半实现 | P2 | `client.py:93-101`；`adapters.proto:28,50-51`；`core/src/runtime/bridge_server.cpp:232-240` | 连接"假活"只能靠 TCP/读写超时发现；参数协商形同虚设 |
| 18 | **无诊断接口**：文档允许原始 OneBot JSON 出现在"日志或诊断接口"，但两者都没有可用诊断面；NoneBot 日志被 sanitize 清空文本后无法定位连接失败 | 未实现 | P2 | `__main__.py:30-42`；`doc/04:88` | 排障只能看稳定错误码，无法判断 NapCat 侧失败原因 |
| 19 | **测试缺口**：无私聊发送、无 mute/kick/approve/react/schedule 拒绝、无 notice/request/meta、无 NapCat WS 断线重连、无健康接口、无平台返回缺 `message_id` 的 unknown 路径测试；fake 平台只实现群 `send_group_msg` | 测试缺失 | P1 | `test_actions.py:22-39`（仅 group）、`test_nonebot_process.py:30-41`（fake 仅群发送）；`test_mapping_outbox.py:44-58`（仅 post_type=notice 拒绝） | `doc/04:138-145` 验收项"接收图片/@、Action 发回群聊和私聊、断线自动重连"缺少对应自动测试；T07b 前无回归保护 |
| 20 | **默认验收入口不包含适配器**：`tests/acceptance/run.sh` 仅当 `KIARABOT_ADAPTER_DIR` 非空才跑适配器测试；CI 虽设置该变量，但工作流已暂停自动触发 | 未验证/未接线 | P1 | `tests/acceptance/run.sh:25-31`；`.github/workflows/ci.yml:3-16,104-109`；`tests/acceptance/README.md:7` | 当前无人自动跑适配器测试；34/34 只是历史自述（`doc/16:115`），无法在 HEAD 复核 |

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/04:59` 进程边界写 "OneBot V11 / reverse WebSocket" | 实现为 NoneBot websockets driver 的**正向** WebSocket（`onebot_ws_urls`），反向连接未配置（`doc/04:159` 自己也这么写） | `__main__.py:23-32`；`doc/04:59` vs `:159` |
| `doc/04:79-86` 适配器需要处理"文本、图片、回复、@、文件等消息段"，含"不支持消息段的降级表示" | 仅 text/image(URL)/at/reply；file/voice/video/face 等降级 UNKNOWN 且不留内容；AUDIO/VIDEO/FILE/LOCATION 永不产生 | `mapper.py:74-95`；`common.proto:70-88` |
| `doc/04:90-101` "Action 转换"列出 `SendMessage/MuteMember/KickMember/ApproveRequest`，称适配器负责转换成 OneBot API 调用 | 仅 `send_message` 纯文本；其余 6 种 kind 一律 `CAPABILITY_NOT_SUPPORTED` | `actions.py:44-49`；`doc/04:5-9`（T04b 子集，已与 :90-101 矛盾） |
| `doc/04:120` "NapCat 断线时，适配器报告 `AdapterStatusChanged`" | 无任何状态事件发送 | 全 `src/` 无 `AdapterStatusChanged`；`grep` 仅 Core 侧处理 |
| `doc/04:121` "Action 超时必须返回 `TIMEOUT`" | 只有"执行前 deadline 已过"返回 `EXPIRED`+`TIMEOUT`；平台调用进行中超时/断线返回 ACCEPTED（未知），不返回 TIMEOUT | `actions.py:86-102`；与 `doc/04:9`（保留 ACCEPTED/未知）一致，与 `:121` 矛盾 |
| `doc/04:32-43` 目录树列出 `onebot/`、`event_mapper/`、`action_executor/`、`grpc_client/`、`health/`、`main.py` | 实际为扁平 `mapper.py`/`actions.py`/`client.py`/`outbox.py`/`__main__.py`，无 `health/` | `ls adapters/nonebot/src/kiarabot_adapter_nonebot/` |
| `doc/04:107-113` 配置只列 `CORE_GRPC_ENDPOINT/CORE_AUTH_TOKEN/ONEBOT_WS_URL/ONEBOT_ACCESS_TOKEN/ACCOUNT_ID` | 启动还**强制**读取 `ADAPTER_ID`、`OUTBOX_PATH`（缺失即 `KeyError`→退出 1） | `__main__.py:20-22,45,56`；`adapters/nonebot/README.md` 有完整列表 |
| `doc/04:22`（职责）"报告连接和平台能力" | 只在上线时声明 2 个能力；连接状态、下线、错误、健康均不上报 | `client.py:49-57`；无 status 代码 |
| `doc/04:153`（T02a）"队列满会回复 `RATE_LIMITED` 并关闭流，所有尚未确认事件必须重投"；`doc/01-runtime-semantics.md:119` 同 | Core 未把 `RATE_LIMITED` 标 `retryable=true`；适配器把非 retryable 拒绝标 failed 并继续，不重投 | `adapter_ingress.cpp:26-31,242-245`；`client.py:115-120` |
| `doc/18:401-402,415`（受保护基线）称"仓库没有 `adapters/`；Python NoneBot2 适配器暂存于同级检出，尚未迁入" | 适配器已迁入 `adapters/nonebot/`（commit `f8388f0`） | `adapters/nonebot/`；`git log --oneline -- adapters/nonebot` |
| `doc/14:26`、`doc/15:13`（**历史快照**）称"仓库无适配器实现目录/独立适配器现状未知" | 同上，已过时 | 同上，需按历史快照口径阅读 |
| `doc/16:115,178` "迁入后验收 34/34" | 34 个用例可静态清点，但其中 `test_cpp_core.py:16`、`test_action_pipeline.py:22` 需 `CORE_TEST_BINARY` 才运行；本机无工具链无法复核 | 测试文件 skipif；第 8 节 |
| `tests/acceptance/README.md:7` "CI 在 pinned vcpkg 构建中运行默认入口；适配器测试由该目录提供" | 工作流自 2026-10-30 起暂停自动触发，仅 `workflow_dispatch`；适配器分支仅在设置 `KIARABOT_ADAPTER_DIR` 时接入 | `.github/workflows/ci.yml:3-16,107-109`；`tests/acceptance/run.sh:25-31` |
| `doc/08:138` 自认"适配器 8081 健康服务尚未实现" | 与代码一致（无健康接口）；但 `doc/08:70` 又要求"适配器未 ready 时不接收新 Action"，无实现载体 | `doc/08:70,138`；全 `src/` 无 health |

## 6. 跨模块影响（只陈述，不替其他模块下结论）

1. **Core Action 校验 vs 适配器会话 ID 语义（待 core-runtime 确认）**：`core/src/application/action_router.cpp:104-126` 只校验字符集与 `target.conversation_id == send.conversation.id`；事件侧 `conversation.id` 恒为 `group:`/`private:` 前缀（`mapper.py:45,63`）。若以 Core/契约的"事件会话 ID"为准，需适配器侧把前缀还原为数字；若以数字为准，需 Core/事件映射改 ID 方案。二者必须统一，否则出站 Action 在真实 ID 下必然失败（本模块缺口 #1）。
2. **Core 错误 `retryable` 语义（待 core-runtime 确认）**：`adapter_ingress.cpp:26-31` 错误工厂默认 false，且无置 true 调用；`doc/01-runtime-semantics.md:119`、`doc/01-contracts.md:263` 要求 `RATE_LIMITED`/存储暂时故障可重试并给 `retry_after_ms=1000`。适配器已实现"retryable 才保留 pending"的消费逻辑（`client.py:115-120`），Core 补标记后即可生效；当前状态下背压即丢事件。
3. **运维 compose/systemd（待 operations 确认，T11b 未验收）**：`deploy/compose.staging.yml:50-72`、`deploy/compose.prod.yml:50-72` 要求 adapter 镜像、`CORE_ENDPOINT`、`HEALTH_PORT=8081`、`/health/ready`、默认命令 `kiarabot-adapter-nonebot`；本模块无镜像/Dockerfile/console script/健康接口，且只接受 `CORE_GRPC_ENDPOINT=127.0.0.1:port`。`deploy/systemd/kiarabot.service` 只管理 compose 栈，无独立适配器单元。`doc/08:138` 已声明部署骨架不可运行，与本模块现状一致，但 compose 与适配器的环境变量契约需双方对齐。
4. **控制面 `/adapters` 状态数据（待 control-api / core 确认）**：`api/openapi.yaml:191-214` 的 `Adapter` schema 要求 `state`，但适配器不上报状态（缺口 #3）；Core `AdapterIngress` 只记录 identity→instance（`adapter_ingress.cpp:181,272-274`）。`/adapters` 能否给出真实健康状态取决于 Core/控制面，不在本模块单方面可证。
5. **示例插件 ai-reply 的发送路径（待 plugin-sdk 确认）**：`plugins/ai-reply/ai_reply.py:30-36` 直接复用事件 `conversation`，在本模块当前校验下必然 FAILED INVALID_ARGUMENT；该示例是否作为"可运行示例"被验收，需要 SDK/插件模块给出结论，或由双方统一 ID 方案。
6. **契约侧（待 contracts 确认）**：`events.proto` 的 `MessageSent/GroupMemberChanged/FriendRequest/AdapterStatusChanged`、`common.proto` 的 `AUDIO/VIDEO/FILE/LOCATION` 段类型目前没有任何适配器会生产；`actions.proto` 的 6 种 Action kind 无实现。契约本身不需改，但"已声明 ≠ 已实现"的口径应在台账中体现。
7. **tests/acceptance 的归属**：`tests/acceptance/` 是 T07a 建立的跨模块验收入口（`doc/16:234-246` 允许修改 `tests/`、各模块测试入口；`tests/acceptance/README.md:3-9` 描述 Core/SDK/控制面/adapter 多环境），不专属适配器；适配器只是可选分支（`run.sh:25-31`），T07b（真实 QQ/NapCat）仍为 `tests/acceptance/real-environment.md` 的人工 runbook，无脚本与结果记录。

## 7. 覆盖结论

- 一句话结论：接收链路（群/私聊、text/image(URL)/at/reply、有界持久化 outbox、确认/断线重投/幂等）实现完整且有 fake gRPC + fake 平台 + 真实 C++ Core 的测试骨架；但出站 Action 仅纯文本、且与真实会话 ID 契约不匹配，事件/消息段覆盖窄，健康/状态与容器化接口缺失，背压/容量/错误分类存在可靠性缺口，真实平台（T07b）与备份恢复未验证。
- 覆盖度估计：**完整覆盖 33 / 68 个功能点；部分覆盖 15；未覆盖 20**。
  - 口径：按"契约/基线/模块文档/运维文档明确声明"的适配器能力逐条枚举，分 6 组：
    - A 事件类型 6 点（群/私聊/MessageSent/GroupMemberChanged/FriendRequest/AdapterStatusChanged）→ 完整 2，未覆盖 4；
    - B 消息段映射 13 点（text、image-URL、image-file/base64、at、at-all 区分、reply、face、voice、video、file、location、forward/node、未知降级）→ 完整 5，部分 1，未覆盖 7；
    - C 会话/身份/幂等元数据 6 点（会话 ID、私聊临时会话、时间戳/message_id、稳定 event_id、schema/delivery_mode/sequence、原始 JSON 不越界）→ 完整 5，未覆盖 1；
    - D Action 11 点（群文本、私聊文本、reply/段拒绝、Delete/React/Mute/Kick/Approve/Schedule 六种 kind、归属+deadline 校验、幂等/终态复用）→ 完整 3，部分 8；
    - E 传输与可靠性 18 点（认证、注册、能力声明、退避、确认语义、retryable 保留、failed 记录、重连重投、心跳、超时、容量/保留、崩溃原子性、水位/乱序、平台→outbox 确认、日志脱敏、健康上报、HTTP 探针、容器命令）→ 完整 11，部分 4，未覆盖 3；
    - F 验收与测试 14 点（单测/fake gRPC、fake 平台群发送、私聊发送测试、notice/request 测试、NapCat 断线测试、映射测试、段降级测试、各 kind 拒绝测试、deadline/容量/取消测试、Core+SDK 闭环、真实 Core 客户端测试、默认入口接线、T07b 真实联调、备份恢复演练）→ 完整 7，部分 2，未覆盖 5。
  - 说明：状态按"契约声明了该能力、代码/测试是否有对应实现与验证"判定；"部分"包含"有实现但有硬编码限制/只覆盖一半/无测试"。

## 8. 未核实事项

1. **测试未运行**：本机无 uv/venv，Python 仅 3.9（适配器要求 `>=3.11,<3.13`），无 cmake/vcpkg/构建产物，无法执行 `pytest`、`CORE_TEST_BINARY` 集成或 `tests/acceptance/run.sh`。第 3/4 节所有"对应测试"只是静态清点（34 用例 = 12+7+12+1+1+1 参数化展开），**未验证通过**；`doc/16:115,178` 的 "34/34" 无法在 HEAD 复核。
2. **ONEbot 模型序列化字段**：`__main__.py:65` 依赖 `event.model_dump(mode='json')` 输出 `original_message`/`message` 为 segment 列表（`mapper.py:66` 优先取 `original_message`）。`nonebot-adapter-onebot==2.4.6`（`uv.lock:256-263`）未安装，无法确认字段名/结构；现有测试通过 fake WS 输入原始 dict 验证映射，不能代替该序列化路径的确认（仅 `test_nonebot_process.py` 覆盖到进程级，且依赖真实依赖安装）。
3. **Core 行为细节**：`ActionRouter`/`BridgeServer` 的派发、`sent` 集合、`complete` 对 ACCEPTED 的幂等处理已读代码，但未运行；`AdapterBridge` 的流取消/重连时序只按测试代码推断。
4. **`/adapters` 与健康聚合**：未核实控制面/Core 对适配器状态的当前返回内容（本模块无状态上报），见第 6 节第 4 条。
5. **T07b 真实环境**：`tests/acceptance/real-environment.md` 只是准备步骤；无 NapCat/QQ、模型服务、真实会话配置，`doc/16:248-261` 三项验收未勾选；真实收发、回滚、备份恢复均未验证。
6. **CI**：`.github/workflows/ci.yml:3-16` 自 2026-10-30 起暂停自动触发；`doc/16` 末尾称此前 5 次运行均未通过（截至 `e70de83`），本机无法查看远端运行结果，也**没有证据表明适配器测试在 CI 中通过过**。
7. **容器/部署**：compose 引用的适配器镜像 `ghcr.io/leekiara1215/kiarabot-adapter-nonebot`、生产 digest 全零占位、`KIARABOT_ADAPTER_COMMAND` 的镜像内可用性均未核实（仓库内无适配器 Dockerfile）；T11b 属于运维模块验收范围。
8. **文档自述与历史快照**：`doc/14`、`doc/15` 明确是历史快照，`doc/18` 为受保护基线；本报告只报告差异，未修改任何 `doc/` 文件。
