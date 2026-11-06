# Core Runtime 运行时模块设计

本文描述 `kiarabot-runtime` 运行时服务：适配器/插件协议入口、账号与绑定、事件 inbox/顺序/投递账本、插件安装/实例与期望状态、Action/策略/配额、插件 namespace 存储、Host 连接与 admission gate、Runtime 资源生命周期（含与 Host/Scheduler 的联合删除）、任务交接接收与 owner 观测查询。跨进程契约唯一来源是 `proto/kiarabot/v1/runtime.proto`、`services.proto`、`host.proto`、`plugins.proto`、`events.proto` 与 `platform.proto`；实现位于 `core/src/runtime/platform/{runtime_rpc,runtime_bridge}.cpp`、`core/src/application/target/runtime_*.cpp`、`core/app/runtime_composition.cpp`，PostgreSQL 适配在 `infrastructure/postgres/`。模块在六服务拓扑中的位置与运行语义见[架构设计](../architecture.md) §3、§5、§7。

## 1 模块定位与职责

Runtime 是 Core 运行时状态的**唯一权威**，负责：

- 外部 Adapter/Plugin 的连接认证、注册、心跳、连接代次（connection_generation）与 Runtime epoch；
- 账号目录、适配器绑定、插件安装（installation）/实例（instance）、期望状态与期望配置版本；
- 事件接收：来源/账号/连接校验、`(adapter,event_id)` 内容指纹去重、会话四元组顺序水位、事件与目标快照同事务持久化；
- 逐插件投递账本：`delivery_targets`/`delivery_attempts`、30 秒租约与续租、ACK、重试退避、死信；
- Action 受理、幂等、配额窗口/日预算、危险动作策略、派发、结果/证据、取消与过期；
- 插件 namespace 存储：实例私有、安装内受控共享、CAS、TTL、配额与幂等收据；
- HostBridge 注册、心跳、Host 命令 outbox/观察 inbox，以及 Runtime 所有的 Host admission gate（`host_admission_gates`）；
- Runtime 资源生命周期：`BeginDelete`/`RestoreResource`/`GetOperation`、tombstone、participants 协调（Host 停止进程、Scheduler 取消任务）；
- Scheduler 交给 Runtime 的 `AcceptTimerTrigger` 持久接收、去重与事件入队；
- owner 查询/审计/日志/operation 变化/消息元数据变化/bootstrap 受控交付。

Runtime **不**执行平台私有 API（由 Adapter 负责）、不启动 OS 进程（由 Host 负责）、不调用模型供应商（由 Model 负责）、不拥有任务定义（Scheduler 负责），也不直接读写其他服务数据库。

## 2 边界与依赖

| 对端 | 交互 | 约束 |
| --- | --- | --- |
| Adapter 实例 | 外部 `AdapterBridge.Connect`（server-verified TLS + bootstrap 凭据/`ConnectionAuthorization`） | 每帧 fence+proof；平台转换与平台副作用在 Adapter |
| Plugin 实例 | 外部 `PluginBridge.Connect`（同上） | 实例/installation/权限来自认证连接，不接受自报 owner；插件只经 SDK |
| Plugin Host | 内部 mTLS `HostBridge.Connect`（部署签发客户端证书映射 `host_id`） | Host 先持久 command journal 再执行；Runtime 只存注册/连接/命令/观察事实 |
| Scheduler | Runtime → Scheduler：插件任务 create/get/cancel（用户/实例委托）；Scheduler → Runtime：`AcceptTimerTrigger` 平台 peer 交接 | 交接用持久 outbox/inbox 去重，COMPLETED 只表示事件已入队 |
| Model | Runtime → Model：插件 `HostRequest.model_chat`/`model_stream_start`（受限委托） | 不转发插件自报权限或供应商密钥 |
| Identity | `CheckAuthorization`、委托交换、Host gate 窄检查、验签材料 | 每个高风险操作在线核验；Identity 不可用 fail-closed |
| Control | 类型化查询/管理命令（携带当前用户委托）与 owner 观测流 | Control 不拥有 Runtime 业务表；查询逐页在线授权 |

依赖：私有 PostgreSQL 逻辑库；mTLS 证书与部署配置（`identity_endpoint`、`model_endpoint`、`scheduler_endpoint`、`bridge_listen`、`hosts` 证书 allowlist、`resource_catalogs`、`control_runtime_peers`、sealed delegation 文件）；不依赖其他服务进程内对象或数据库。

## 3 进程/组件结构

- 进程入口 `core/app/runtime_main.cpp` → `platform::run_service()`；组合根 `core/app/runtime_composition.cpp` 装配应用、客户端、监听与每秒维护钩子。
- 监听面：内部 mTLS owner `grpc_listen` 注册 `RuntimeService` 与 `HostBridge`；可选 `bridge_listen` 只注册 `AdapterBridge`/`PluginBridge`，不请求客户端证书；`probe_listen` 提供存活/就绪探测。
- 内部应用（均在 `core/include/kiarabot/core/application/target/`）：`RuntimeResourceApplication`（兼容 facade，委派 `runtime_resources/{account_adapter,instances,lifecycle,host_commands,timer,bootstrap}.cpp`）、`RuntimeBridgeApplication`、`RuntimeActionApplication`、`RuntimeStorageApplication`、`RuntimeTaskApplication`、`RuntimeHostApplication`/`RuntimeHostGateApplication`、`RuntimeAuditApplication`、`OperationObservationApplication`、`ObservationFeedApplication`、`RuntimeBootstrapDeliveryApplication`、生命周期 participants。
- 出站客户端：`runtime_identity_client.cpp`（在线授权/委托交换/失效订阅）、`runtime_model_client.cpp`（model.chat 与 streaming）、`scheduler_task_client.cpp`（插件任务与生命周期 ListTasks/CancelTask）、`sealed_instance_delegation_source.cpp`（读取 0600 sealed 父委托文件，不作为信任根）。
- 基础设施：`infrastructure/postgres/src/{runtime_events,action_records,storage_records,host_registration,host_command_outbox,observation_feeds,operation_observations,bootstrap_delivery_records,runtime_management_receipts,common_work}.cpp`；SQLite 只属 Host/Adapter，不在 Runtime 内。
- 维护循环：`check_dependencies` 每秒调用 bridge `maintenance()`（租约过期、重试/死信、领取并推送到期投递）、action `maintenance()`（到期扫描）与 `lifecycle_maintenance()`（墓碑/participants/60 秒保留清理批次）；停止时先发 `ShutdownNotice`，再用固定 500ms 预算 drain。

## 4 数据模型与持久化

Runtime 私有 PostgreSQL 库由显式初始化建立，进程只校验 schema；schema 版本见 `infrastructure/postgres/schema/v1.hpp`（Runtime=25），关系/约束/descriptor 数量由 `schema/manifest.hpp` 固定（173 个关系）。

| 关系组 | 文件 | 内容 |
| --- | --- | --- |
| 资源 | `schema/services.hpp` | `accounts`、`installations`、`instances`、`adapters`、`adapter_bindings`、`hosts`、`instance_configs`、`bridge_credentials`（只存 SHA-256）、`lifecycle_tombstones` |
| 事件/投递 | `schema/runtime_events.hpp` | `events`（保留原 serialized envelope 字节）、`conversation_watermarks`、`delivery_targets`、`delivery_attempts`、`ordered_delivery_heads` |
| Action | `schema/runtime_actions.hpp` | `actions`、`action_idempotency`、`quota_windows`、`daily_budgets`、`adapter_result_evidence`、`action_policies` |
| 存储/namespace | `schema/runtime_storage.hpp` | `namespaces`、`namespace_values`、`namespace_members`、`namespace_quota_counters`、`namespace_receipt_results` |
| Host | `schema/runtime_host_*.hpp` | `host_connections`（连接代次/在线/期望配置版本）、`host_commands`（持久 outbox）、`host_observations`（inbox）、`host_admission_gates` |
| 命令/观测 | `schema/common.hpp`、`observation_feed.hpp`、`operation_observations.hpp` | `command_receipts`、`audit_entries`、`operations`、`operation_participants`、`outbox`/`inbox`、`replay_results`、`observation_feed_*`（600 秒/16384 条）、`operation_observation_*`（600 秒/65536 条） |
| 交付/回执 | `schema/runtime_bootstrap_deliveries.hpp`、`runtime_management_receipts.hpp` | bootstrap delivery 意图/回执（不含明文/摘要/路径）、管理命令 operation 回执 |

状态与标识：`LifecycleState = Active/Disabled/Deleting/Deleted/Restoring/Purged`；`OperationState = Accepted/Running/Blocked/Succeeded/Failed/Cancelled/Expired`；`ExecutionOutcome = NotStarted/KnownSucceeded/KnownFailed/Unknown`。Fence 绑定实例、Runtime epoch、执行代次、连接代次，分别用于拒绝旧连接/旧尝试；资源版本是条件更新权威。删除使用 30 天恢复窗、180 天墓碑保留（`kLifecycleRestoreWindow`/`kLifecycleTombstoneRetention`）。

## 5 关键流程/状态机

- **适配器注册与事件接收**：外部 TLS 流用 bootstrap 凭据换取 `Fence`+`ConnectionAuthorization`；同 adapter 新连接替换旧连接代次。`PublishEvent` 校验来源/账号绑定/连接代次/事件类型（单帧受 gRPC 1 MiB 收包上限约束）；同 `(source,event_id)` 同内容为 `DUPLICATE`、异内容 `CONFLICT`；会话顺序按 `(adapter,account,conversation,type)` 水位判断，缺号/乱序标记 degraded 或拒绝；在目标快照事务内写事件、水位、每实例 `delivery_targets` 与审计，提交后才回 `PERSISTED`。
- **插件注册与配置**：`RegisterPlugin` 校验清单/能力/权限/installation 绑定并协商 feature（`plugin.config.v1`、`action.result.v1`、`host.tasks.v1`、`host.model.v1`、`host.model.stream.v1`；streaming 仅在配置 Model endpoint 时可协商）；storage 不是 feature，按 `HostRequest.storage_*` 帧与 `storage.namespace` 权限/namespace 规则准入。注册响应返回 `desired_config_version`；`RuntimeGetInstanceConfig`（target implemented）返回期望/已应用版本与 apply_state 投影。目标 `PluginConfig` 下发帧与 `PluginConfigAck` 尚未登记实现，`UpdateConfig` 只把期望版本置 Pending，当前没有配置应用的 Bridge 帧路径；期望配置版本、协议注册、配置应用、投递 ACK 仍是不同事实，不得把注册/版本投影当作配置已生效。
- **投递与租约**：每实例独立 `delivery_attempts`，租约 30 秒、每 10 秒续租、自首次领取总上限 300 秒；ACK/Renew 必须匹配当前 `event+instance+attempt+fence`，旧尝试迟到 ACK 返回 `STALE` 且不能确认新尝试；失败按首次之外最多 5 次重试（1/2/4/8/16 秒 ±20% jitter），不可重试错误直接死信；`ordered` 按实例+会话四元组串行。
- **Action**：插件提交后在本地 UoW 原子写 Action、幂等键、配额窗口与 UTC 日预算；六种 kind 各有封闭权限（`message.send/react/delete`、`member.mute/kick`、`request.approve`）与适配器 capability；Mute/Kick 另需显式启用、群白名单与正预算。派发前在线复核权限/账号 gate/白名单/policy，提交 `dispatched` 后才写流；Action deadline ≤300 秒，终态保留 168 小时，dispatch 序列上限 100；超时/断线/迟到结果保持 UNKNOWN 或只追加 evidence，不覆盖终态；取消只在派发前有效。
- **存储/namespace**：默认按认证实例派生私有 namespace；共享 namespace 由获授权管理者显式开启并配置成员；写入/CAS 绑定调用方、scope、operation 与幂等收据；限额为实例 1024 keys/1 MiB、单值 64 KiB、key 256 B、TTL ≤30 天、安装聚合 8192 keys/8 MiB、共享成员 1024、收据 10000/80000；同键重放不延长 TTL。
- **Host gate**：Host 在内部 mTLS 上注册/心跳（5 秒心跳、15 秒疑似离线，只投影 online 与 UNKNOWN，不写进程停止）。`SetHostAdmissionGate` 只允许当前已登记 Host 为自己操作：校验部署证书 allowlist、当前 HostBridge fence、Host 维护意图单调版本，再经 Identity `CheckHostGateAuthorization` 核对实际 Host 叶证书指纹与 `host.execute` 精确授权，最后在单个 Runtime UoW 内以 `expected_gate_version` CAS 持久化 `host_admission_gates`。gate 关闭时 Runtime 拒绝为该 Host 派发/迁移新工作；Host 的 maintenance operation/阶段仍由 Host 本地 SQLite 权威，Runtime 不伪造完成。
- **联合删除/恢复（Joint delete）**：`BeginDelete` 在单个 UoW 内校验 `resource.manage`/存在/预期版本，把资源置 `Deleting`（实例同时 desired=STOPPED；适配器/实例进入 `Deleting` 时撤销 Runtime 本地 `bridge_credentials`），创建 operation 与 participants（`runtime` 恒 Done；受管实例含 `host`；实例/安装含 `scheduler`），写 tombstone（30 天恢复、180 天清理）；无外部参与者的账号/适配器/主机同事务直接 `Deleted`。每秒 maintenance 推进：Host participant 通过 `host_commands` outbox 下发 StopExecution，只有持久回执为已知终态才 Done；Scheduler participant 只对目标实例 `ListTasks(PENDING)` 后 `CancelTask` 其 owner 任务，离线/拒绝保持 `Blocked`（安装级取消当前返回 `CAPABILITY_NOT_SUPPORTED`）。全部参与者 Done 才 `Succeeded`/`DELETED`；UNKNOWN/离线绝不伪完成。`RestoreResource` 只接受未过恢复窗、版本匹配的 `DELETED` 资源，回到 `Active` 并删除墓碑，不复活凭据/任务/租约；再次 `BeginDelete` 重新调度参与者。
- **定时交接**：`AcceptTimerTrigger` 只接受 Scheduler 平台 peer（`spiffe://kiarabot/scheduler` + 离线注册的 `timer.accept`）并校验 source/operation owner/occurrence/event id/payload hash/valid_until/schedule version；在本地事务内按 `(source,message_id,occurrence_id)` 去重后入队 `TimerTriggered`（event_id=occurrence），返回持久接受/重复/明确拒绝；过期交接 `TIMEOUT`。

## 6 接口与契约

契约源：`proto/kiarabot/v1/runtime.proto`（`RuntimeService`，64 个类型化 RPC）、`services.proto`（`AdapterBridge.Connect`/`PluginBridge.Connect`）、`host.proto`（`HostBridge.Connect`、HostService/HostManagementService）、`plugins.proto`（插件 HostRequest/HostResponse 与配置帧）、`policy.proto` 方法选项。target 可用性以 `proto/tests/target_spec.json` 与 `proto/tests/contract_status.json` 的 `execution_scopes.target` 为准：RuntimeService 当前 35/64 登记 `implemented`，3 条 Bridge 流均登记 `implemented`；`.proto` 内联 `CONTRACT_STATUS` 是 reference 历史注释，不能作为当前可用性依据。

| 面 | 方法族 | 暴露面与说明 |
| --- | --- | --- |
| 外部 Bridge | Adapter：注册/心跳/事件/ACK/续租/Action 结果；Plugin：注册/心跳/事件 ACK 续租/HostRequest/Action 帧/模型流帧（配置 ACK 帧未实现） | `exposure=wire`；bootstrap+`ConnectionAuthorization` 鉴权，每帧 fence+proof |
| 内部 owner RPC | 账号/安装/实例/适配器/主机 Get/List、`GetInstanceConfig`、Action 查询、共享 namespace 管理、生命周期、审计/日志/观测、bootstrap 交付 | 由 Control 携带当前用户委托或平台 peer 调用；逐页在线授权；`GetHost`/`ListHosts` 尚未登记实现 |
| 内部 dispatcher | `RegisterAdapter`/`Heartbeat`/`PublishEvent`/`ClaimEvent`/`DeliverEvent`/`AckDelivery`/`RenewDelivery`/`RegisterPlugin`/`HeartbeatPlugin`/`SubmitAction`/`QueryAction`/`CancelAction`/storage unary | `exposure=internal_dispatcher`，外部客户端不得直接调用；桥接帧与 dispatcher 走同一应用准入 |
| 平台 peer | `AcceptTimerTrigger`（Scheduler）、`SetHostAdmissionGate`（当前 Host）、bootstrap 交付上报/查询（Control allowlist） | 不使用用户 delegation 的窄豁免，不扩散到其他方法 |

公共类型以 `platform.proto` 为准：`RequestContext`（request_id/trace/deadline/幂等键）、`ResourceRef`、`Fence`、`OperationRef`/`Operation`/`OperationProgress`、`FailureContext`、`StreamCursor`；`Error`/`ErrorCode` 在 `common.proto`。幂等键作用域为“认证主体+方法+资源 scope”：同键同内容重放原结果，异内容 `CONFLICT`；内部成功命令收据至少保留 7 天（`kMinimumReceiptRetention`），容量不足拒绝新命令而不是淘汰有效记录。

## 7 权限与安全

- **连接身份**：内部 owner 监听用 mTLS；外部 Bridge 用 server-verified TLS + 逐实例可撤销 bootstrap 凭据换取 `ConnectionAuthorization`（`proof`/`refresh_credential` 为 CSPRNG ≥256 位，绑定实际 TLS 流、主体、fence、Runtime epoch、凭据版本），名义寿命 ≤56 秒，客户端在到期前 10 秒单飞续期；每帧携带当前 fence 与 proof，失败关闭流，不回退旧流。
- **业务授权**：每个 RPC 的必需权限来自 `policy.proto` 方法选项并由应用再次校验；插件实例权限来自已认证连接的注册结果（manifest 声明不构成授予），最终按 kind/资源/账号 scope 检查；危险动作还要 policy/白名单/预算；Host 注册与 gate 使用部署证书 allowlist 的 SHA-256，空 allowlist 全部拒绝（fail-closed）。
- **秘密与凭据**：`bridge_credentials` 只存 SHA-256；bootstrap 交付表不存明文/摘要/路径；sealed delegation 文件是 0600 普通文件且不是信任根，Runtime 仍 Exchange 并在线 CheckAuthorization；密钥/token 不进入日志、公开投影、outbox 或 argv。
- **隔离**：Host 没有任意 shell/路径执行能力，只能执行已登记包摘要与允许参数；插件无数据库账户，只经 SDK；Runtime 不跨服务读写表；撤销后 gate/lease/连接代次共同阻止旧连接继续产生新副作用。

## 8 错误/失败/恢复语义

- 稳定错误码：`INVALID_ARGUMENT`、`UNAUTHENTICATED`、`PERMISSION_DENIED`、`NOT_FOUND`（不可见统一隐藏）、`CONFLICT`（版本/幂等内容/旧代次）、`RATE_LIMITED`（容量/配额）、`TIMEOUT`、`UPSTREAM_UNAVAILABLE`、`PLUGIN_UNAVAILABLE`、`CAPABILITY_NOT_SUPPORTED`、`INTERNAL`；`Failure` 同时给出 acceptance/recovery/operation/retry_after。
- **旧连接/旧尝试**：fence 或 attempt 不匹配返回 `STALE`/`CONFLICT`，绝不确认新尝试；连接替换只推进 connection_generation，不把旧进程当作已停止。
- **投递恢复**：进程重启后按持久 `events`/`delivery_targets`/`delivery_attempts` 恢复，插件重连后重投相同 `event_id`、新 attempt/trace；死信保留可查询（`ListDeadLetters` 属后续能力）。
- **Action 未知结果**：派发后超时/断线/崩溃保持 UNKNOWN，迟到结果只写 evidence；取消只在派发前有效；外部副作用没有 exactly-once，幂等与 attempt 只保证不重复确认/不盲目重发。
- **生命周期阻塞**：参与者 Pending/Dispatched/Unknown/依赖故障保持 `Blocked` 并带稳定 reason/next_attempt，operation 非终态；恢复后下一轮收敛；清理失败只记 `cleanup_failed`，不撤销已生效 gate。
- **事务结果未知**：COMMIT 结果未知返回 `UPSTREAM_UNAVAILABLE`+`Acceptance::Unknown`，按原幂等身份查询恢复，不重新执行外部副作用；数据库死锁只在纯本地事务内有限重试。

## 9 可观测性与运维

- **就绪**：`RuntimeReadiness` 汇总 `BusinessImplementation`（应用已装配）、`DatabaseSchema`（Runtime 私有库可写）、`Identity`（当前授权链可验证）三项真实观测；缺失观测为 Unverified 而非可用，组合根是唯一能把整体置 ready 的位置。
- **日志**：结构化 stderr 行只含 operation/outcome/error_code 等标识；生命周期维护在非空闲轮输出 `operation=lifecycle_maintenance outcome=ok|degraded reconciled=… completed=… blocked=… purged=… pruned=… cleanup_failed=…`，用于检索而不新增 API。
- **审计与查询**：`ListAudit` 返回 owner 审计事实，`ListLogs` 是脱敏日志投影（`request.recorded|request.rejected` 闭集），`ListOperationChanges` 发布 600 秒/65536 条 operation journal，`ListLogChanges`/`ListMessageMetadataChanges` 发布 600 秒/16384 条家族 journal；游标绑定 owner/epoch/scope/sequence，超窗或 epoch 变化要求 resync。
- **运维**：配置为严格 JSON schema v1；`grpc_listen`/`bridge_listen`/`probe_listen` 与各 endpoint 由部署注入；Host 证书 allowlist、`resource_catalogs`、`control_runtime_peers` 为空时对应能力 fail-closed。收到停止信号后先向 Bridge 发送 `ShutdownNotice`，以固定预算 drain 后释放监听；不丢未决投递状态。
- **容量处置**：事件全域 1024、每实例未终态投递 1024、Action 全域 100000/每 owner 未决 1024 等达到阈值时拒绝新受理并返回 `RATE_LIMITED`，已受理工作保留；清理由显式批次执行，清理失败告警不撤销 gate。

## 10 测试与验收证据

- 领域/应用单元测试（`core/tests/architecture/`）：`runtime_rules_test.cpp`、`runtime_event_delivery_test.cpp`、`runtime_resource_application_test.cpp`、`runtime_storage_application_test.cpp`、`runtime_plugin_capability_test.cpp`、`runtime_host_application_test.cpp`、`runtime_bootstrap_delivery_test.cpp`、`runtime_scheduler_lifecycle_participant_test.cpp`、`runtime_bridge_presence_test.cpp`、`operation_observation_application_test.cpp`、`observation_feed_application_test.cpp`、`observation_feed_identity_test.cpp`、`p12e_audit_query_test.cpp`、`foundation_test.cpp`。
- 传输/客户端（`core/tests/platform/`）：`runtime_identity_client_test.cpp`、`instance_delegation_provider_test.cpp`、`sealed_instance_delegation_source_test.cpp`、`scheduler_runtime_client_test.cpp`、`scheduler_task_client_test.cpp`、`host_bridge_test.cpp`、`host_bridge_server_test.cpp`、`host_runtime_client_test.cpp`、`model_bridge_test.cpp`。
- 端到端（CTest 名 / 脚本）：`p05_runtime_identity_e2e`、`p05_runtime_catalog_e2e`（`core/tests/platform/runtime_identity_e2e_test.py`，真 PG16.4 + 真 mTLS Identity/Runtime + 委托/资源授权）、`p06_bridge_acceptance`、`p06_client_e2e`、`p06_postgres_runtime_events`、`p06_bridge_presence`、`p07_adapter_e2e`（`tests/architecture/p07_adapter_e2e.py`：真 adapter/fake OneBot/Runtime/PG）、`p07_postgres_action_records`、`p09_postgres_storage_records`、`p08_host_bridge_e2e`、`p08_postgres_host_registration`、`p10_timer_e2e`（`core/tests/platform/p10_timer_e2e_test.py`，Scheduler→Runtime 触发交接）、`p13_joint_delete_e2e`（`tests/architecture/p13_joint_delete_e2e.py`，test-only 组合根）、`p13h3_joint_delete_production_e2e`（真 Host/Scheduler 参与者）、`p14_native_single_host`、`p05v12_owner_feed_e2e`、`p05_postgres_bootstrap_delivery`、`p12_bootstrap_delivery_e2e`（`tests/architecture/bootstrap_delivery_e2e.py`）、`ws02_postgres_operation_observations`。
- 覆盖要求：正常、拒绝、超时/租约过期、依赖故障、旧 fence/旧 attempt、重复/乱序事件、UNKNOWN 矩阵、崩溃重启恢复；真实验收结论以 CTest 执行记录与[架构设计](../architecture.md) §7、§11 的阶段证据为准，测试注册本身不等于通过。

## 11 已知限制与后续

- 29 个 RPC 仍登记 `not_implemented`：适配器/插件桥接路径依赖的注册、心跳、事件、投递、Action、storage 方法通过 Bridge 帧行使（unary 面仅为内部 dispatcher/测试）；`ListActionMetadata`、`InspectActionMetadata`、`ListDeadLetters`、`ReplayDelivery`、`GetHost`、`ListHosts` 当前没有 RPC 处理实现；`DisableAccount`、`BindAdapter`、`UnbindAdapter`、`SetDesiredState`、`UpdateConfig`、`GetResourceState` 已有应用/RPC 代码但契约 target 尚未翻转，文档不宣称其对外可用。
- 同安装多实例首期采用 fan-out（每个获准实例各有一条投递），不是负载均衡副本；有序只保证实例+会话四元组内串行，跨实例不排序。
- Scheduler participant 只批准实例范围：`ListTasks(PENDING)` + `CancelTask`（owner=目标实例），安装级删除的 Scheduler 清理返回 `CAPABILITY_NOT_SUPPORTED`，跨实例任务不受影响。
- Host gate 只开放 `SetHostAdmissionGate` 这一条 Host→Runtime 写路径；Host 维护 operation/阶段真相在 Host 本地 SQLite，Runtime 只保存 gate 与命令/观察投影。
- `AcceptTimerTrigger` 在代码中有平台 peer 处理与真实验收用例，但契约 target 登记仍未翻转；对外可用性必须按 target 登记判断，不能以代码存在为准。
- 目标插件配置下发/ACK（`PluginConfig`/`PluginConfigAck`）未登记实现；当前只有注册响应 `desired_config_version` 与 `RuntimeGetInstanceConfig` 投影，`UpdateConfig` 置 Pending 后没有生产应用路径，`plugin.config.v1` 仅作为协商词表存在。
- 全部限定单活动写入进程（数据库内排他锁）；无 HA/多副本/自动故障切换。跨主机部署、断网演练、真实 QQ 长稳、真实满盘与生产规模仍属环境项；外部平台副作用没有 exactly-once，UNKNOWN 可能长期存在直到查询或人工处置。
