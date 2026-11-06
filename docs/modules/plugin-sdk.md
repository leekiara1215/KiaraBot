# Plugin SDK 模块设计

本文描述 `sdk/python/`（当前唯一已实现的 PluginSDK）如何作为进程外插件与 Runtime PluginBridge 之间的稳定边界。线协议唯一来源是 `proto/kiarabot/v1/plugins.proto`、`proto/kiarabot/v1/platform.proto` 与 `proto/kiarabot/v1/services.proto`；SDK 实现位于 `sdk/python/src/kiarabot_sdk/`，目标 Bridge handler 位于 `core/src/runtime/platform/runtime_bridge.cpp`（`PluginBridgeImpl`），应用层准入契约位于 `core/include/kiarabot/core/application/target/runtime_bridge_application.hpp`。模块在整体架构中的位置见[架构总览](../architecture.md)。

## 1 模块定位与职责

PluginSDK 是插件进程访问 Core/Runtime 能力的唯一受支持通道，负责：

- 解析并本地预检 `plugin.json`（`PluginManifest`），协商 API 版本、能力与权限；
- 管理插件生命周期：`on_start`、事件/配置回调、`on_stop` 与协作式超时；
- 事件投递的租约、续租、ACK 与 `delivery_receipt` 处置；
- Action 客户端（`action.result.v1`）、Host 存储/模型/模型流/任务客户端；
- `plugin.config.v1` 配置应用与 ACK；
- 连接级传输行为：server-verified TLS、bootstrap、`Fence`/`connection_proof`、单飞 refresh；
- 把稳定错误码、接收事实（acceptance/recovery）和“未知结果”忠实暴露给插件。

它不负责：QQ/OneBot 平台解析、业务规则与授权决策、Core 数据库与持久化、平台对象或数据库连接。SDK 生成的 UUID、幂等键只是调用关联，不能替代 Core 的鉴权和持久事实。

## 2 边界与依赖

- 只依赖 `proto/kiarabot/v1/` 生成的 Python 类型；不导入 `core/`、`adapters/nonebot/`、控制面板或第三方模型 SDK。
- 依赖固定为 Python 3.11、`grpcio==1.85.0`、`protobuf==6.33.6`（含开发组 `grpcio-tools`、`pytest`、`pytest-asyncio`），见 `sdk/python/pyproject.toml` 与 `sdk/python/uv.lock`。
- 生成代码写入构建目录，不手工维护消息镜像；生成入口为 `sdk/python/tools/generate_proto.py`。
- 与 Core 的边界：插件只能发 `PluginToCore` 帧；实例身份、namespace、任务 owner 和 installation 由 Runtime 从认证连接派生，SDK 不接受自报值。
- 与 Host 的边界：受管启动、环境变量白名单、start/stop 硬超时归 `core/src/runtime/platform/host_process.cpp` 与 Host 进程；SDK 只读取部署注入的 endpoint、CA 与 bootstrap 凭据。
- 与适配器的边界：SDK 不感知 OneBot 或适配器实现，只消费统一 `EventEnvelope` 与 Action 语义。

## 3 进程/组件结构

```text
Runtime PluginBridge (gRPC bidi stream)
   │  PluginToCore / CoreToPlugin（每帧 Fence + connection_proof）
   ▼
kiarabot_sdk Runner（单进程，单活动流）
   ├─ reader：顺序读取帧并分发（不等待慢消费者）
   ├─ event worker：串行执行 on_event/on_config（队列上限 2）
   ├─ refresh：到期前单飞轮换 proof/refresh_credential
   └─ per-attempt renew：投递租约 10 秒续租，300 秒上限
```

| 模块 | 职责 |
| --- | --- |
| `runtime.py` | `Plugin`、`Context`、`Options`、`Runner`、manifest 校验与会话主循环 |
| `transport.py` | TLS channel、`Fence`/`ConnectionState`、注册响应与 refresh 结果校验 |
| `delivery.py` | 30 秒租约、10 秒续租、300 秒上限与 receipt 处置 |
| `actions.py` | Action 提交前置校验、结果矩阵与风险判定辅助函数 |
| `tasks.py` | `task_create/get/cancel` 帧与本地参数预检 |
| `model_stream.py` | 流 registry、有序事件校验、取消与 partial 事实 |
| `errors.py` | `PluginError`（稳定码、retryable、流式/交付附加事实） |
| `testing.py` | `FakeTargetRuntime`：真实 gRPC + TLS 的协议测试替身 |
| `__main__.py` | `python -m kiarabot_sdk --manifest <path>` 入口与环境解析 |

一个插件实例同一时刻只允许一个活动流；重连时实例身份保持，`connection_generation` 前进。事件与配置回调串行，RPC 响应由 reader 独立接收，因此回调内可安全调用 Host API。

## 4 数据模型与持久化

SDK 自身不持久化任何业务或交付事实；`applied_version`、投递目标、attempt、Action/Model 结果均为 Runtime 权威。进程内仅保存当前连接所需状态：`granted_permissions`、`enabled_features`、最近成功配置快照副本（`context.config`）、配置 ACK 缓存（≤32 个 delivery）、在途 unary 请求（≤64）、stream registry（≤4 个活动流）、receipt waiter 与最近投递处置。

线上核心结构（字段编号以 proto 为准）：

- `PluginManifest`：`id`/`name`/`version`/`api_version`/`runtime`/`entrypoint`、`capabilities`、`permissions`、`dependencies`、`config_schema`、`isolation`、`ordered`；`runtime` 只接受 `python-rpc`，`isolation` 必须显式为 `process`。
- `Fence`：`instance_id`、正 `runtime_epoch`、正 `execution_generation`、正 `connection_generation`；每帧带完整 fence。
- `ConnectionAuthorization`：opaque `proof`/`refresh_credential`（SDK 要求 32 字节）、正 `refresh_generation`、`issued_at`/`expires_at`（名义寿命 ≤56 秒）、`principal_id`、`credential_version`、`authorization_version`。
- `DeliveryAttempt`：`attempt_id`、`event_id`、`attempt_number`、`started_at`/`lease_until` 与完整 fence；事件 envelope 的 `event_id` 必须与 attempt 一致。
- `PluginConfig`：`version`、`values`、`delivery_id`、`apply_timeout_ms`、`config_version`、`fence`。
- `HostRequest`/`HostResponse`：通过 tag 10..127 的 length-delimited operation 承载 storage/model/task；未知操作分类见第 6 节。

## 5 关键流程/状态机

注册与首次连接：SDK 用部署 CA 建立 server-verified TLS（无 insecure 回退），在 gRPC metadata 携带唯一 `authorization: Bearer <bootstrap>`；首帧为 `RegisterPluginRequest`（无 fence/proof）。注册成功后必须解析 `RegisterPluginResponse.fence` 与 `authorization`，并校验：`required_api_version` 与 manifest 一致、`granted_permissions` 与申请集合逐项相等、`enabled_features` 是本地已知 target features 的子集、授权 fence 与响应 fence 一致。`on_start` 在注册前执行，首次配置在其后到达。

事件投递：收到 `CoreToPlugin.event` + `delivery_attempt` 后校验 fence/attempt 形状，30 秒租约内执行 `on_event`（`hook_timeout`）；并行每 10 秒续租（单条在途），总长不超过 300 秒。完成后发送携带该 attempt 的 ACK，并等待 `delivery_receipt`：成功路径只接受 `ACKNOWLEDGED`/`DUPLICATE`，`STALE` 不代表成功、不确认新尝试、不重放旧副作用；receipt 丢失记为未知。`ShutdownNotice` 到达后停止接收新工作，在 `drain_timeout_ms`（受 `io_timeout` 约束）内完成并确认在途事件；drain 正常结束时不再自动重连，只有未收到该帧的流中断才按未知结果重连并重新注册。

配置应用：先校验帧 fence、`config_version`、`version`、`delivery_id`、`apply_timeout_ms`；同一 delivery 重放返回缓存 ACK 且不重复回调。低于已应用版本只做幂等确认；交付版本高于注册返回的 durable desired version 时以 `accepted=false` + `INVALID_ARGUMENT` ACK 且绝不应用。回调成功才更新 `context.config` 与本地 applied 版本；失败只回稳定码并保留上一成功快照。

授权轮换：到期前 10 秒发起单飞 refresh；服务端接受后旧材料立即失效，SDK 在响应到达前不发送其他帧，成功后原子替换 proof/refresh 与 generation。refresh 失败、超时或响应丢失都关闭旧流，经仍有效的 bootstrap 重新注册产生新 `connection_generation`，禁止用旧材料猜测结果。

模型流：`host.model.stream.v1` 需独立协商且复用 `model.chat` 权限。SDK 在发送 start 前登记 stream registry，随后只接受 `started(sequence=0)`、连续 `delta(1..N)`、`terminal(N+1)`；重复/乱序/gap/未知 oneof/关联错误使本 attempt 本地终结为“交付未知”，保留已收 partial 并抛 `PluginError.delivery_unknown=True`，绝不伪造 `COMPLETED`。`cancel()` 走独立 `HostRequest.model_stream_cancel`，receipt（CANCELLATION_REQUESTED/ALREADY_TERMINAL/NOT_ACTIVE）只确认本连接取消处理，不替代 terminal、不证明未执行或未计费。

重连失败路径：仅 `PluginError.retryable=True` 的连接类失败按 `retry_delay` 重连；非可重试错误（如注册权限不符）向上抛出并结束进程。

## 6 接口与契约

gRPC 服务：`services.PluginBridge/Connect(stream PluginToCore) returns (stream CoreToPlugin)`（`proto/kiarabot/v1/services.proto`）。按 feature 协商的帧与入口：

| 能力 | SDK 入口 | 说明 |
| --- | --- | --- |
| 事件 | `on_event` + ACK/Renew | 基础事件插件；需对应事件权限 |
| `action.result.v1` | `submit_action`/`query_action`/`cancel_action` | 未协商本地 `CAPABILITY_NOT_SUPPORTED`；未知结果不转 FAILED |
| `host.storage.v1` | `storage_get`/`storage_set`/`storage_delete` | namespace 由认证身份派生；幂等键 16–128 字符 |
| `host.model.v1` | `model_chat` | 非流式；`stream=True` 保持 `CAPABILITY_NOT_SUPPORTED` |
| `host.model.stream.v1` | `model_stream` | 独立协商；沿用 `model.chat` 权限 |
| `host.tasks.v1` | `task_create`/`task_get`/`task_cancel` | 冻结协议无 `task_list`；创建还需 `event.timer.triggered` 订阅 |
| `plugin.config.v1` | `on_config` + `PluginConfigAck` | 单连接最多一条在途配置 |

`HostRequest` 形状规则：真正缺 operation 返回 `INVALID_ARGUMENT`；无已知 operation 且恰有一个 10..127 内 length-delimited 未知候选时返回 `CAPABILITY_NOT_SUPPORTED`；已知+未知候选、多个未知候选或错 wire type 返回 `INVALID_ARGUMENT`；区间外未知 metadata 被忽略。分类在任何业务副作用前完成。

manifest 预检（`validate_manifest`）：`id`/`version` 匹配 `[A-Za-z0-9_.-]{1,128}`；`name` 非空且 ≤128；`api_version` 必须为 `kiarabot.plugin.v1`；`entrypoint` 形如 `module:factory` 且 ≤256；`dependencies` 非空或 `config_schema` 非空返回 `CAPABILITY_NOT_SUPPORTED`；capabilities 只允许六种事件名且必须声明对应权限；`ordered` 原样转发，由 Core 持久化并在重连时比对。

启动接口：`Options(endpoint, token, tls_root_cert, tls_server_name=None, instance_id=None, io_timeout=5.0, hook_timeout=5.0, heartbeat_interval=5.0, retry_delay=1.0, lease_renew_interval=10.0)`；环境变量必填 `PLUGIN_CORE_ENDPOINT`、`PLUGIN_TLS_ROOT` 与凭据（外部自管用 `PLUGIN_AUTH_TOKEN`，受管实例由 Host 注入 `KIARABOT_PLUGIN_BOOTSTRAP`），可选 `PLUGIN_INSTANCE_ID`、`PLUGIN_TLS_SERVER_NAME` 与各超时/间隔项；非法值以 `INVALID_ARGUMENT` 退出。

## 7 权限与安全

- 传输只接受 server-verified TLS，`tls_root_cert` 缺失直接拒绝启动，无 insecure/匿名降级。
- bootstrap 凭据只从部署文件或环境读取，不出现在日志、异常或持久化；proof/refresh 为 opaque secret，数据类关闭 `repr`。
- 每帧都携带完整 fence 与当前 proof；服务端帧同样校验 fence，失配按认证失败关闭流。
- `granted_permissions` 是协商后的静态集合，SDK 不缓存也不放大授权；`model.chat`、`storage.namespace`、`task.schedule`、`task.read`、`message.*` 等最终决策都在 Core/Runtime。
- callbacks 异常转脱敏 `INTERNAL`（不含自由文本）；不把 `retryable` 当作自动重试许可。
- 进程隔离只是生命周期与故障隔离，不是操作系统安全沙箱；部署需限制插件可见环境与文件系统。

## 8 错误/失败/恢复语义

- `PluginError`：`code` 为封闭 `ErrorCode`，`retryable` 表示线上重试性而非自动重试授权；其他异常统一 `INTERNAL`，协作式超时 `TIMEOUT`。
- 无法协作退出的回调由 `UncooperativeHook` 触发，`__main__` 强制以退出码 1 结束进程；库调用方收到异常。
- 传输/超时/断线为未知结果：不把 Action 包装成 `FAILED`，不把模型流包装成完成；调用方用原 `action_id`/`idempotency_key`/`logical_call_id` 查询或重放。
- 投递 ACK 的 receipt 丢失只表示结果未知，Runtime 会重投；SDK 不因未知结果重放旧副作用。
- 断线时所有 pending unary 请求以可重试 `UPSTREAM_UNAVAILABLE` 结束，流 registry 以“未知交付”失败；已收 partial 文本与计数保留。
- 注册被拒（`PERMISSION_DENIED`、`UNAUTHENTICATED`、非可重试错误）结束进程；依赖故障按退避重连。

## 9 可观测性与运维

- 日志使用稳定字段：`operation=plugin_process|plugin_connect` 带 `error_code=<稳定码>`，`operation=plugin_delivery` 带 `outcome`/`attempt_id`；不输出自由文本或秘密；插件自身日志当前被丢弃，统一日志接通是后续任务。
- `Runner.last_dispositions` 保留最近 receipt 处置，供测试与观测；SDK 无独立健康 RPC 或指标端点。
- 依赖与环境准备：`uv sync --project sdk/python --python 3.11.18 --locked`、`sdk/python/tools/generate_proto.py`，随后 `python -m kiarabot_sdk --manifest <absolute path>`。
- 受管部署由 Host 注入最小环境（endpoint、bootstrap）；外部自管部署用 `PLUGIN_AUTH_TOKEN`。bootstrap 文件权限为 0600、只读一次，撤销随实例停用/解绑。
- 运行参数均可由环境覆盖，部署侧应保持 `io_timeout` > 模型/Host 调用时限、`hook_timeout` 与回调耗时匹配。

## 10 测试与验收证据

- SDK 单元/契约测试：`sdk/python/tests/test_runtime.py`、`test_options.py`、`test_process.py`、`test_closed_loop.py`、`test_actions.py`、`test_storage.py`、`test_tasks.py`、`test_model.py`、`test_model_sdk.py`、`test_model_stream.py`、`test_config.py`、`test_host_managed.py`。
- `sdk/python/src/kiarabot_sdk/testing.py` 的 `FakeTargetRuntime` 使用真实 gRPC + server-verified TLS，覆盖 bootstrap、注册 fence/authorization、单飞 refresh、30/10/300 秒租约与 ACK/Renew receipt 处置、`ShutdownNotice`、config fence/config_version，并支持注入时钟、`refresh_failure`、`refusal`、`heartbeat_acks=False` 与断线。
- 全链路验收（CTest，注册于 `infrastructure/postgres/CMakeLists.txt` 与 `cmake/platform.cmake`）：`p06_client_e2e` 运行真实 `kiarabot_sdk.Runner` + 真实 Runtime/PG/TLS；`p06_bridge_acceptance` 用手工 wire 客户端覆盖 Bridge 边界；`p07_adapter_e2e` 覆盖插件 Action 到适配器平台调用的闭环；`p05b_task_chain_e2e` 覆盖真实 PluginBridge→Scheduler 任务链；`p11h_managed_model_e2e` 覆盖受管 Host→Runtime→Model→本地 HTTP Provider；`p11_streaming_managed_model_e2e` 覆盖流式受管链。
- 运行命令：`sdk/python/.venv/bin/python -m pytest -q sdk/python/tests`；插件示例测试用 `-c sdk/python/pyproject.toml plugins/group_admin/tests plugins/reminder/tests`。

## 11 已知限制与后续

- 仅 C++ Core + Python SDK 组合有实现；C++ SDK、TypeScript SDK 与插件脚手架工具均未实现。
- 对话服务、权限查询 RPC、任务列表、动态订阅、依赖解析、非空 config schema、native/wasm 运行时均无协议入口或实现。
- 插件健康只有注册/断线推断；无健康查询帧、无插件日志帧、无后台自动重启协议。
- 模型流只支持文本；非文本/工具调用、真实付费 Provider 与多主机部署仍属后续环境验收。
- Host 受管链路与六服务目标栈已有真实 PG/TLS 端到端证据（如 `p08_managed_host_e2e`、`p11h_managed_model_e2e`）；真实生产部署、外部付费 Provider 与 start/stop 超时等环境项仍需补齐。`FakeTargetRuntime` 只证明协议夹具范围，不能替代 Core 授权与持久化。
