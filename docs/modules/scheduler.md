# Scheduler 任务调度模块设计

本文描述 `kiarabot-scheduler` 进程：一次性定时任务的领域所有者、occurrence 与触发 outbox 的权威、向 Runtime 可靠交接 `TimerTriggered` 的发送方。跨进程契约唯一来源是 `proto/kiarabot/v1/scheduler.proto`；应用/领域实现在 `core/src/application/target/scheduler_service.cpp`、`core/src/application/target/scheduler_rules.cpp` 与 `core/include/kiarabot/core/domain/target/scheduler_rules.hpp`；RPC/客户端在 `core/src/runtime/platform/scheduler_rpc.cpp`、`core/src/runtime/platform/scheduler_identity_client.cpp`、`core/src/runtime/platform/scheduler_runtime_client.cpp`，组合根在 `core/app/scheduler_main.cpp`、`core/app/scheduler_composition.cpp`。模块在整体架构中的位置见[架构总览](../architecture.md)（§3、§5、§7）。

## 1 模块定位与职责

Scheduler 负责：

- 一次性任务的定义与查询：`CreateTask`/`CancelTask`/`GetTask`/`ListTasks`/`GetOccurrence`/`GetOperation`。
- 任务状态机：`Pending/Running/Retrying/Completed/Failed/Cancelled`；owner/installation 只从认证上下文派生。
- 到期扫描：`due_sweep` 在任务到达 `run_at` 时创建 occurrence、Scheduler 侧 operation、持久 trigger outbox 并把任务置 `Running`。
- 到期的可靠交接：用 outbox claim 在事务外调用 Runtime `AcceptTimerTrigger`，成功后把 occurrence 置 `SUCCEEDED`、任务置 `COMPLETED`。
- 交接过期处置：handoff 有效期 7 天，outbox attempts 达到 100 转 NeedsAttention；超过有效窗口置 occurrence `EXPIRED`、任务 `FAILED(TIMEOUT)`，不自动重放。
- 任务/operation 的只读观察：`ListOperationChanges`（operation journal）与 `ListTaskChanges`（v1.2 task 安全元数据 feed），由 Control 调用。

Scheduler **不负责**：插件进程与执行、事件投递/ACK、消息或任务 payload 的业务语义、QQ/平台触发、跨服务事务。“COMPLETED”只表示 Runtime 已把 `TimerTriggered` 持久入队，不代表插件已执行或回调成功。

## 2 边界与依赖

- 上游调用者：Runtime（受管插件 `task_create/get/cancel` 经 PluginBridge 与 `SchedulerTaskClient`；生命周期清理平台 peer 调用 `ListTasks/CancelTask`）与 Control（任务 REST/owner 查询，使用 Identity 委托）。
- 下游依赖：Runtime `AcceptTimerTrigger`（`SchedulerRuntimeClient`，内部 mTLS，5s 预算）与 Identity `CheckAuthorization`（`SchedulerIdentityClient`）。
- 存储：Scheduler 私有 PostgreSQL 逻辑库（`tasks`/`occurrences`/quota 表 + 共享 receipts/audit/operations/outbox/inbox/观察日志），唯一来源 `infrastructure/postgres/schema/v1.hpp`（当前 v14），细节见[基础设施模块](infrastructure.md)。
- 部署依赖：`database`（私有库）、可选 `identity_endpoint` 与 `runtime_endpoint`、Scheduler-only `runtime_peers` 数组；宿主需提供 IANA zoneinfo（`/usr/share/zoneinfo`）。
- 不读 Runtime/Model 私有表，不使用跨库外键；插件不能直连 Scheduler。

## 3 进程/组件结构

```text
kiarabot-scheduler（core/app/scheduler_main.cpp → scheduler_composition.cpp）
  ├─ SchedulerService unary 适配器      core/src/runtime/platform/scheduler_rpc.cpp
  │    ├─ 普通授权：SchedulerIdentityClient（mTLS + delegation + Identity CheckAuthorization）
  │    ├─ 受限平台授权：runtime_peers（仅 ListTasks/CancelTask）
  │    ├─ ListOperationChanges → 共享 operation 观察应用
  │    └─ ListTaskChanges     → v1.2 task feed 应用
  ├─ SchedulerService（应用/状态机/outbox/交接）  scheduler_service.cpp
  ├─ SchedulerRuntimeClient（→ Runtime.AcceptTimerTrigger）
  └─ 私有 PG UoW + 共享 journal（receipts/outbox/inbox/audit/observations）
```

组合根创建 Scheduler 的私有库 UoW 工厂、IANA 时区适配端口、CSPRNG 的 occurrence/operation id 端口与 SHA-256 摘要端口；Runtime/Identity 客户端按配置可选装配。维护循环由共享 `run_service` 的 1 秒 dependency hook 驱动，每轮：读取数据库权威时间 → `due_sweep(now,10)` → `expire_stale_triggers(now,10)` → 配置 Runtime 时 `deliver_triggers(now,5)` → 有界 pruning（收据与 inbox 各 100）。

## 4 数据模型与持久化

- `tasks`：`(owner, task_id)` 稳定键、`installation_id`、`target`（会话 id/type）、`payload`、`run_at`、`state`、`version`、`occurrence`、`cancel_requested`、`reason`、`created_at/updated_at`；创建时固定 occurrence id，occurrence 行只在到期扫描建立交接意图时出现。
- `occurrences`：`occurrence_id`、`task_id`、owner/installation、`task_version`、`due_at`、`HandoffContext`（source、message=occurrence_id、operation、resource、version、payload hash、valid_until、actor）、`state`、`accepted_event_id`、`acknowledged_at`、`attempts`、`last_error` 与时间列。
- `scheduler_quota_counters`：实例/安装非终态任务容量计数；其余收据/审计/outbox/inbox 复用共享关系。
- 规则常量（`scheduler_rules.hpp`）：horizon 366 天、实例容量 1024、安装容量 8192、handoff 有效期 7 天、尝试上限 100（outbox claim 每次递增 attempts，达到上限转 NeedsAttention，不自动删除）、payload ≤64KiB。
- 幂等：创建/取消写共享 `command_receipts`，键含主体+方法+资源+幂等键，值含创建字段的确定性指纹（排除 transport request/trace/deadline）；同键同指纹返回原任务，异指纹 `CONFLICT`。
- 观察：operation 变更进共享 operation journal（600 秒有界窗口）；task 变更以 v1.2 feed 同一 UoW staged，回滚不发布。

## 5 关键流程/状态机

**创建**：在线授权 `task.schedule` → 在单 UoW 内读数据库时间、算创建指纹、查幂等收据 → 检查实例/安装容量（超限 `RATE_LIMITED`，不驱逐有效收据）→ `resolve_run_at`：`run_at` 与 `local_time` 严格二选一，必须严格未来且 ≤366 天；`local_time` 只经 IANA 端口解析，DST gap/fold、非法 IANA zone 为 `INVALID_ARGUMENT`，tzdata 缺失为 `UPSTREAM_UNAVAILABLE`；→ 分配 occurrence id、写 `Pending` 任务与收据、staged task feed 后提交。owner 由授权上下文派生，installation 由 delegation 的 Installation scope 派生，均非请求字段。

**取消**：仅当任务仍 `Pending`、`cancel_requested=false` 且 occurrence 尚未创建时，置 `Cancelled + cancel_requested=true` 并写幂等收据；一旦交接意图（occurrence/outbox）已建立或任务终态，返回 `CONFLICT`，不假装撤回已发生的交接。

**到期扫描**：先短读列出最多 `limit` 个到期候选，回滚释放读事务；对每个候选另起 UoW 复核仍 `Pending`、`run_at<=now` 且无 occurrence，然后同事务写：Scheduler operation（Accepted）、`HandoffContext`（`valid_until=now+7d`）、occurrence（Accepted）、outbox `Pending` 记录、任务 `Running`（version+1）与 task feed。逐任务独立提交；重复扫描不重复建 occurrence/outbox。

**交接（deliver_triggers）**：`claim_triggers` 以 outbox claim（lease 20 秒）领取；先在短 UoW 复核任务/occurrence 归属，事务外调用 Runtime `AcceptTimerTrigger`；成功（含 duplicate）时 `confirm_trigger_accepted` 把 occurrence 置 `SUCCEEDED`（记录 accepted_event_id/acknowledged_at）、任务置 `COMPLETED`。Runtime 永久拒绝（`INVALID_ARGUMENT`/`PERMISSION_DENIED`/`CONFLICT`/`CAPABILITY_NOT_SUPPORTED`）把 occurrence/task 置 `FAILED`；可重试失败（`UNAVAILABLE`/`DEADLINE_EXCEEDED`/`RATE_LIMITED`）保留 claim 至过期，用同一 occurrence/event id 重试（Runtime 按 event id+fingerprint 去重）。超过 `valid_until` 由 `expire_stale_triggers` 置 occurrence `EXPIRED`、任务 `FAILED(TIMEOUT, retryable=false)`，不自动重放；重复扫描不产生第二个事件。

## 6 接口与契约

`SchedulerService`（`proto/kiarabot/v1/scheduler.proto`）只接受 mTLS，逐操作在线授权：

| RPC | 权限 | scope | 默认/最大期限 | 幂等 |
| --- | --- | --- | --- | --- |
| `CreateTask` | `task.schedule` | `RESOURCE_TASK`+`RESOURCE_INSTANCE` | 5s/30s | 是 |
| `CancelTask` | `task.schedule` | `RESOURCE_TASK`+`RESOURCE_INSTANCE` | 5s/30s | 是 |
| `GetTask`/`ListTasks`/`GetOccurrence`/`GetOperation` | `task.read` | `RESOURCE_TASK`+`RESOURCE_INSTANCE` | 2s/5s | 否 |
| `ListOperationChanges` | `task.read` | `RESOURCE_TASK`+`RESOURCE_INSTANCE` | 500ms/500ms | 否 |
| `ListTaskChanges` | `task.read` | `RESOURCE_TASK`+`RESOURCE_INSTANCE` | 500ms/500ms | 否 |

- 线上 `TaskRecord` 的 target 只含 conversation id/type；冻结的 `CreateTask`/`CancelTask` 不携带 account，因此经 RPC 创建的任务 account 为空（instance/installation 归属仍严格强制）。
- RPC 边界校验 `RequestContext`：canonical request/trace UUID、必填 deadline 且有效期限 ≤30s、写命令幂等键 16–128 字符；wire⇄domain 转换与稳定 `RpcFailure`/gRPC 状态映射在适配器，业务规则全部在应用层。
- 普通授权：delegation 结构/audience=`scheduler`/叶证书绑定校验，owner 取 delegation subject，installation 取 permission bindings 中的 Installation resource scope；随后 Identity `CheckAuthorization`（方法+请求 scope+已知授权版本）做当前权威判定。缺 proof/installation 绑定、Identity 不可达一律 fail-closed。
- 受限平台授权：仅 `ListTasks`/`CancelTask` 接受 Runtime 平台 peer；真实入站叶证书 SAN 必须为 `spiffe://kiarabot/runtime…` 且 DER SHA-256 命中 `runtime_peers` 项，显式要求 `task.read`/`task.schedule` 能力，请求 scope 必须恰为一个 `RESOURCE_INSTANCE` 且等于配置实例；owner/installation/account 由配置派生。未命中 allowlist 的证书回退普通 delegation 路径。
- `ListTaskChanges`/`ListOperationChanges` 只对 Control 类调用者开放，不与插件/Runtime 平台路径共享。

## 7 权限与安全

- owner 隔离：跨实例的 `GetTask`/`CancelTask` 天然 `NOT_FOUND`；平台 peer 路径还要核对 installation/account，跨安装/跨账号返回 `NOT_FOUND`（不泄露存在性），缺能力返回 `PERMISSION_DENIED`。
- 平台 peer 路径是显式收窄豁免：只对 ListTasks/CancelTask 两个方法生效，不扩散到 CreateTask/GetTask 等其他方法，也不改变普通 delegation 行为；未登记证书不会因为“看起来是 Runtime”而被放行。
- 时区解析不猜测 UTC 偏移：非法/不存在的 zone、DST gap/fold 都是稳定 `INVALID_ARGUMENT`，tzdata 不可用是 `UPSTREAM_UNAVAILABLE`。
- payload 是插件提交的受管数据，不进入普通日志/审计正文；任务行只暴露给当前授权的 owner/Control scope。
- 任务包含插件业务参数，Scheduler 不做内容校验或执行；执行语义留给 Runtime/插件。

## 8 错误/失败/恢复语义

- 输入/时区/DST：`INVALID_ARGUMENT`；容量超限：可重试 `RATE_LIMITED`（不驱逐有效收据）；幂等键复用但内容不同：`CONFLICT`；不存在/不可见/跨实例：`NOT_FOUND`。
- 交接期间 Runtime 可重试失败不伪造 `COMPLETED`；outbox claim 到期后以同一 occurrence/event id 重试，确认丢失由 Runtime 去重吸收（duplicate=true），事件与 fan-out 目标恒为 1。
- 崩溃恢复：due sweep 已提交 occurrence/outbox 后进程崩溃，重启后由 claim 继续，不重复建 occurrence；确认已落库但未回执时重试同一 event id。
- 过期：handoff 超过 7 天或尝试耗尽 → `EXPIRED`/`FAILED(TIMEOUT)`，无自动重放；`COMPLETED` 语义仅为“Runtime 已持久入队”。
- 依赖不可用：数据库/Identity/Runtime 未配置或不可达时 readiness=false、RPC fail-closed；维护循环忽略单次有界扫描的失败（busy/Conflict 跳过本轮），读取权威时间等不可恢复的数据库故障按服务生命周期退出。

## 9 可观测性与运维

- 健康：共享进程探针 `/health/live` 与 `/health/ready`；ready 依赖私有库、IANA zoneinfo、Runtime delivery 与 Identity 授权链全部可用。未配置数据库时进程可启动但 readiness=false，不接受业务调用。
- 维护循环每秒在进程线程执行有界扫描，不持写锁跨网络；`due_sweep`/`expire`/`deliver` 每轮都有数量上限，避免长事务。
- 观察面：`ListOperationChanges`（operation 变化，600 秒窗口）与 `ListTaskChanges`（v1.2 task 安全元数据）供 Control WS/运维查询；任务状态以 owner 表只读核对。
- 部署：`runtime_peers` 必须与 Identity 离线登记的 Runtime platform principal 证书一致；`identity_endpoint`/`runtime_endpoint` 缺失时对应功能 fail-closed 而非“无限制”；宿主 zoneinfo 缺失会导致时区任务失败，需部署检查。
- 受管插件任务链依赖 Runtime 的 sealed 实例父委托（`instance_delegation_sealed_file`，≤56s，部署/Control 侧刷新），Scheduler 本身不保存用户委托材料。

## 10 测试与验收证据

- 应用/边界单测：`p10_scheduler_service`（`core/tests/platform/scheduler_service_test.cpp`：状态机、取消窗口、容量、时区 fake、due/claim）、`p10_scheduler_identity`、`p10_scheduler_rpc`、`p10_scheduler_runtime_client`（canonical UUID 与 AcceptTimerTrigger 映射）、`p10_scheduler_task_client`（Runtime→Scheduler client：无 delegation 平台路径、显式 instance scope、幂等键）。
- 真实 PG16.4：`p10_postgres_scheduler`（`infrastructure/postgres/tests/`：tasks/occurrences 条件写、claim、收据/审计同 UoW、容量与 due 索引）。
- 多进程真 mTLS：`p10_timer_e2e`（`core/tests/platform/p10_timer_e2e_test.py`，RUN_SERIAL）：三进程定时交接、崩溃重启收敛、取消/到期竞争、确认丢失/重复、EXPIRED、容量矩阵、P13h-1 平台 peer 允许/拒绝矩阵。
- Control/SDK：`p10_scheduler_rest`（真实 Control HTTPS list/get/cancel、404/CONFLICT、幂等重放）、`p10_sdk_tasks_e2e`（真实 SDK Runner + TLS PluginBridge fixture 的任务语义矩阵）、`p05b_task_chain_e2e`（真实 plugin-host + 受管 SDK 插件→Runtime→真实 Scheduler）。
- 生命周期联动：`p13_scheduler_lifecycle`（Runtime 参与者 fake worker：pending-only、跨实例不取消、离线/超时保持 BLOCKED）与 `p13h3_joint_delete_production_e2e`（真实四库/Identity/Runtime/Scheduler/Host：BeginDelete 经真实 Scheduler ListTasks/CancelTask 收敛或 BLOCKED）。
- SDK 单测：`sdk/python/tests/test_tasks.py`（创建/查询/取消、参数与时区校验、错误/幂等映射）。

## 11 已知限制与后续

- 真实 7 天 EXPIRED 等待未执行，仅以受控时钟/短窗口等价验证；多 installation 跨实例容量并发压测、真实 QQ/平台触发仍是环境项。
- 仅一次性任务；重复/cron 任务、任务列表插件 API、动态订阅均无契约入口。
- 冻结 `CreateTask`/`CancelTask` 不带 account，任务行 account 为空；target account 归属的完整语义需契约澄清。
- 安装级（`RESOURCE_INSTALLATION`）删除的 Scheduler 清理不在已批准的实例级方案 A 内：生产参与者返回 `BLOCKED(CapabilityNotSupported)`，不做跨实例枚举。
- sealed 任务父委托需部署侧持续刷新（≤56s 寿命，刷新工具与最小权限运营流程见运维文档）；刷新停止时任务创建 fail-closed。
- 观察窗口 600 秒：更长历史需从 owner 表或 Control REST 快照恢复，WS/cursor 超窗会要求 resync。
- 真实生产规模、跨主机与 HA 不在当前范围；Scheduler 自身仍以单活动写入进程 + 私有库为前提。
