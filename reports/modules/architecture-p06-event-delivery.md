# P06 事件投递与外部 Bridge 模块报告（target 实现与全链路验收）

- 状态：P06 target 事件投递链与外部 Adapter/Plugin Bridge 已完成，真实进程/真 TLS/真 PG16.4 下由
  已发布的 P06b SDK 与 P06c Adapter 客户端端到端通过（2026-11-02）。
- 代码边界：wire 契约 `proto/kiarabot/v1/{adapters,plugins,platform,services}.proto`；Runtime 领域/端口
  `core/include/kiarabot/core/domain/target/event_delivery.hpp`、`core/include/kiarabot/core/ports/target/business_ports.hpp`；
  PG 五表与仓储 `infrastructure/postgres/schema/runtime_events.hpp`、`src/runtime_events.*`；Bridge
  应用/传输 `core/src/application/target/runtime_bridge_application.cpp`、`core/src/runtime/platform/runtime_bridge.cpp`；
  组合根 `core/app/runtime_composition.cpp`；客户端 `sdk/python/src/kiarabot_sdk/runtime.py`、
  `adapters/nonebot/src/kiarabot_adapter_nonebot/{bridge,client,outbox,mapper}.py`。
- 不新增进程/服务/数据库/权限；外部监听为 Runtime 同一进程内可选 `bridge_listen`（server-verified
  TLS，不请求客户端证书），内部 owner `grpc_listen` 保持 mTLS。

## 已实现语义

- 拓扑：`bridge_listen` 严格配置（Runtime-only、地址合法、不与 grpc/probe 冲突；缺省不监听）；外部监听
  只注册 AdapterBridge/PluginBridge，平台 owner 监听只注册 RuntimeService/HostBridge；Bridge 与内部
  listener 同证书材料，客户端仍验证服务端证书。
- 注册/授权：首帧 Bearer bootstrap（PG 只存 SHA-256）→ Runtime 分配/递增 connection_generation →
  返回完整 Fence + ConnectionAuthorization（32B proof/refresh、56s、refresh_generation）→ 后续每帧
  fence+proof；refresh 单飞、旧材料失效、丢响应重连新 generation；撤销/禁用通过 ≤500ms 本地 DB
  复核 gate 在下一帧拒绝并关流；连接表有界（128），首帧 Read 有 5s watchdog。
- A1/B1：BindAdapter 不要求 AdapterRecord 预存在（schema v9 放宽外键），首次 bridge 登记以 bootstrap
  resource_id 为 assigned instance 创建/补全记录并校验声明账号绑定；外部 plugin 首次分配并持久
  execution_generation=1，重连只推进 connection_generation（Host 受管仍用真实进程代次）。
- 事件：Adapter event → 校验 source/adapter_instance/account A1 binding → inbox accept（同 ID 异内容
  CONFLICT、重放 DUPLICATE、旧 sequence CONFLICT、跳号 sticky degraded、容量背压）→ 逐实例目标快照
  fan-out（当前连接的 plugin 订阅/ordered）。opaque envelope 以 bytea 原样持久化，重试与重放不丢字节。
- 投递：30s 租约、≤300s 上限、1/2/4/8/16s ±20% 退避、最多 5 次重试后死信；每秒有界 maintenance
  sweep/claim/push + 启动 recover + 每 60 轮 prune；ACK/Renew 条件写映射
  ACKNOWLEDGED/DUPLICATE/STALE/REJECTED/RENEWED，旧 attempt 不确认新 attempt。
- 停机：停止信号后向活动 bridge 流发送有界 ShutdownNotice（drain_timeout_ms=1000，单次写 1s deadline），
  随后关闭监听/流；未收到 notice 的客户端仍按未知结果处理。
- 客户端对齐：P06b SDK 要求正 execution_generation/完整 fence/refresh 字段；P06c Adapter bridge
  client 在真实服务端上验证 assigned_instance_id、30s heartbeat、fence/proof、刷新、错误码和
  ShutdownNotice。P06c 注册 capabilities 收敛为六个 `action.*` 执行能力（事件上报不是协商能力；
  真实 QQ/NoneBot 平台 self_id 与 Runtime 账号资源 id 的映射留 P07/M2）。

## 验收与证据

- `ctest -R p06_runtime_event_delivery`：纯领域规则（水位/背压/租约/退避/死信/容量）。
- `ctest -R p06_postgres_runtime_events`：真实 PG16.4 五表/仓储/事务/租约/重试/恢复。
- `ctest -R p06_bridge_acceptance`：真 PG+真 TLS+真 Runtime 的 wire 级 Bridge 验收（注册、授权、
  refresh、事件 ACK、租约、REJECTED/STALE、撤销、跨账号拒绝、ShutdownNotice、无秘密泄漏）。
- `ctest -R p06_client_e2e`：**已发布客户端全链路**——真实 `kiarabot_sdk.Runner` 插件与真实
  NoneBot Adapter `BridgeClient`/`Outbox`（经 `map_message`）连接真实 Runtime：注册/授权/心跳/
  事件 lease+Renew+ACK/证明 refresh/断线重连新 generation/ShutdownNotice drain；同时校验
  对端假设（assigned_instance_id、fence、heartbeat=30s、refresh_generation 递增、ACK 状态）。
- 上游回归：`ctest -R 'p06|p05|p03_postgres'`、`sdk/python pytest`（81 passed/23 skipped）、
  `adapters/nonebot pytest`（108 passed，使用部署依赖 venv）。
- 证据登记：`proto/tests/contract_status.json` target overrides/evidence 指向 current Runtime
  Bridge 源码与 `tests/architecture/p06_bridge_acceptance.py` / `p06_client_e2e.py`；
  `doc/01-contracts.md` target execution-scope 表同步。已翻 implemented：bridge_methods 两条、
  three semantic.bridge.*、semantic.transport.graceful_shutdown、shutdown 帧/字段、
  ConnectionAuthorization/Refresh/Result、DeliveryAttempt/Receipt、Bridge 注册/refresh 帧字段、
  DeliveryDisposition 5 值、`EventEnvelope.message_received`。

## 未完成/后续

- 真实 QQ 与 NoneBot 平台进程、OneBot WebSocket 未接线（P07/M2）；P06 e2e 用合成 OneBot JSON 经真实
  mapper/bridge/outbox，不伪造网络平台。
- Adapter 本地 outbox 队列/背压的 `semantic.adapter.backpressure_retryable_rate_limited` 未做真实
  饱和测试，保持 not_implemented；Runtime 入站容量拒绝已返回 retryable RATE_LIMITED+关流。
- 插件 manifest permissions 的 Identity 授予过滤未实现（P06b/P08 后续）；当前为声明词表原样回显 +
  逐操作在线鉴权占位，fan-out 只基于当前连接实例的声明订阅。
- P05 `RuntimeBindAdapterResponse.bootstrap_credential`/`CreateInstanceResponse.bootstrap_credential`
  的 RPC 级 target 字段验收未覆盖（本任务以 seed digest 直连 Bridge 验收）；真实 Control 下发
  bootstrap 的端到端验收留 P05/P07 联合。
- 慢消费者 per-stream writer 线程与 ASAN 重连竞态压力测试未做（shutdown 写已有 deadline，
  注册首帧/连接数已设界）。
