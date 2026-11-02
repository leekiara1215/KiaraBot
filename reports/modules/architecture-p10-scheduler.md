# P10 Scheduler 退出证据矩阵（P10f）

状态（2026-11-02）：**P10 未退出**。P10a domain/PG/RPC、P10b 三进程定时交接 e2e、P10c SDK
任务方法在本地真 PG16.4/真 mTLS/多进程范围内**已实测通过**；但 **P10 的 target 契约登记尚未
翻转**（见第 3 节），且真实 7 天 EXPIRED 等待、多 installation 跨实例容量、真实平台触发与
生产委托来源仍属**待环境/待验收**。本任务（P10f）只做证据矩阵与状态核对，**不写实现、不改
核心语义、不翻转 target override**。

- 基线：`d508d67`（main，P13d 文档合入后）
- 工作树：`.worktrees/p10f`（分支 `codex/p10f`，从 main 新建）
- 证据口径：`已实测` = 本 HEAD 上实际运行命令 + 测试名；`已实现` 不代表 target 的生产可用
  声明，target 以 `proto/tests/contract_status.json` 的 execution_scopes 为准。
- 相关文档：[契约状态](../doc/01-contracts.md)、[Core](../doc/02-core.md)、
  [基础设施](../doc/05-infrastructure.md)、[事务/迁移](../doc/22-transactions-and-migration-design.md)、
  [实施计划](../doc/23-implementation-plan.md)。

## 1. 验收矩阵（本 HEAD 实测）

命令前缀（工作树根）：

```
cmake --preset full-locked ...（共享依赖路径见构建环境）
cmake --build --preset full-locked --parallel 3
ctest --preset full-locked -R 'p10' --output-on-failure
ctest --preset full-locked -R 'p03_postgres' --output-on-failure
sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_tasks.py -q
```

| # | 层 | 测试 | 覆盖 | 结果 |
|---|---|---|---|---|
| 1 | domain/application | `p10_scheduler_service` | 任务/occurrence 状态机、创建/查询/取消、取消窗口冲突、owner 派生、容量与 due 索引、时区规范化（fake clock/UoW） | PASS（unit, p10） |
| 2 | authorization | `p10_scheduler_identity` | Scheduler→Identity 在线授权调用、委托/拒绝、依赖故障映射 | PASS（unit, p10, security） |
| 3 | RPC | `p10_scheduler_rpc` | SchedulerService CreateTask/CancelTask/GetTask/ListTasks/GetOccurrence/GetOperation 的真实 gRPC 编解码、认证/权限、幂等、稳定错误码 | PASS（integration, p10） |
| 4 | PG16.4 | `p10_postgres_scheduler` | 真 PG schema v9 fixture：tasks/occurrences/quota 条件写、claim、收据/审计同 UoW、清单与容量 | PASS（integration, p10） |
| 5 | 三进程 e2e | `p10_timer_e2e` | 真 mTLS：Scheduler↔Runtime 定时交接（Register/领取/outbox/AcceptTimerTrigger/Runtime 事件去重/确认 COMPLETED）、崩溃恢复、取消/到期竞争、创建后宕机与确认丢失恢复 | PASS（integration, fault, p10） |
| 6 | PG 基线 | `p03_postgres_database` | 显式空库初始化、四服务 schema/descriptor/manifest 校验 | PASS（integration, p03） |
| 7 | SDK | `sdk/python/tests/test_tasks.py`（43 tests） | SDK 任务方法：创建/查询/取消/参数与日期时区校验、错误/幂等映射 | 43 passed |

汇总：`ctest -R 'p10'` **5/5 passed**（Total Test time real ≈ 58s）；`ctest -R 'p03_postgres'`
**1/1 passed**；pytest **43 passed**。本矩阵未重跑 `p04/p05/p06/p07/p08/p09/p11` 等门禁。

## 2. 已实现/已实测范围

- P10a：Scheduler application 任务/occurrence 状态机、create/get/list/cancel、到期 claim、
  收据/审计、容量计数与 due 索引；`SchedulerUnitOfWork` 的 `tasks()/occurrences()/task_quota()`；
  PG schema v9（tasks 扩列 + scheduler_quota_counters + occurrences_due/tasks_owner_state 索引）。
- P10b：Runtime `AcceptTimerTrigger` 在受限上下文中以 `inbox(source=scheduler,
  message=occurrence_id)` 去重后持久产生 `TimerTriggered`；确认丢失后同 occurrence 重发可恢复，
  `COMPLETED` 仅表示事件已持久入队。三进程真 mTLS e2e 覆盖交接、崩溃/重启收敛、取消/到期竞争。
- P10c：SDK 任务方法（`create/get/list/cancel` 等）与 Control REST 任务查询/取消转发层。
- 时区：`LocalTaskTime`/IANA 解析规则由 P10d/基础设施时区端口覆盖（见 doc/05）。

## 3. target/契约状态核对（关键发现）

现状（`proto/tests/contract_status.json` + `proto/tests/target_spec.json`）：

- `execution_scopes.target.overrides` 中 **没有任何 Scheduler 相关项**（scheduler/task/
  occurrence/timer 关键字命中 0 项）；target 默认 `not_implemented`。
- `target_spec.json` 的 SchedulerService 六个方法（CreateTask/CancelTask/GetTask/ListTasks/
  GetOccurrence/GetOperation）全部仍为 `not_implemented`。
- reference categories 中：`Scheduler*Request/Response`、`TaskRecord`、`OccurrenceRecord`、
  `TimerTriggered.occurrence_id`、`semantic.scheduler.*`（4 条）均为 `not_implemented`；
  `LocalTaskTime.*`、`TaskCreate/Get/CancelRequest`、`TaskResponse` 等为 implemented（历史/
  reference 语义，不等于 target 生产可用）。`operations.listTasks` 为 reference implemented，
  `operations.getTask/cancelTask/getOccurrence` 为 not_implemented。
- doc/01 的契约状态表与上述 registry 一致（未出现“文档标 implemented 而 target 未登记”）。

结论：**P10 代码与测试证据已具备，但 target 生产登记（contract_status overrides +
target_spec 方法状态 + evidence + doc/01 execution-scopes 行）没有翻转**，与 P04/P07/P08/P09
等已退出阶段的做法不一致。这是 P10 退出判定的**契约登记缺口**，不是实现缺失。按“不证不翻、
不改核心语义”的约束，本任务**未自行翻转**；建议单独任务 P10g 按下面清单登记：

翻转候选（需同时补 `execution_scopes.target.evidence` 与 doc/01 execution-scopes 行）：
1. `SchedulerService` 六个方法 → target_spec `implemented`，`status_note` 引用本报告。
2. proto_messages：`SchedulerCreateTaskRequest/Response`、`SchedulerCancelTaskRequest/Response`、
   `SchedulerGetTaskRequest/Response`、`SchedulerListTasksRequest/Response`、
   `SchedulerGetOccurrenceRequest/Response`、`SchedulerGetOperationRequest/Response`、
   `TaskRecord`、`OccurrenceRecord`（共 20 项）。
3. proto_fields：上述 request/response 字段 + `TaskRecord`/`OccurrenceRecord` 字段
   （registry 中共 28 个 `Scheduler*` 字段 + 23 个 `TaskRecord/OccurrenceRecord` 字段）+
   `TimerTriggered.occurrence_id`。
4. semantic_rules：`semantic.scheduler.owner_derivation`、`cancel_window`、`handoff_expiry`、
   `timer_occurrence_identity`（4 项；测试证据 `p10_scheduler_service`、
   `p10_timer_e2e`）。
5. operations：`operations.getTask/cancelTask/getOccurrence`（`listTasks` 已 reference
   implemented；target 需按 Control→Scheduler owner 转发证据翻转）。

证据要求（`evidence_scopes.validate_scopes`）：每项 implemented 需同时有 target source
（如 `core/src/application/target/scheduler_application.cpp`/`scheduler_rules.cpp`/
`core/src/runtime/platform/scheduler_rpc.cpp`）与 target test（`core/tests/platform/
scheduler_service_test.cpp`、`scheduler_rpc_test.cpp`、`scheduler_identity_client_test.cpp`、
`core/tests/platform/p10_timer_e2e_test.py`）。`p10_postgres_scheduler`（infrastructure 路径）
不能单独作为 target test evidence。

## 4. 未验收/待环境缺口

| 缺口 | 当前状态 | 缺口说明/后续 |
|---|---|---|
| 真实 7 天 EXPIRED 等待 | 已用时钟回拨/短 deadline 等价覆盖（unit/RPC/e2e），未真实等待 7 天 | 需要可控真实时钟/长期运行环境；语义已由 `semantic.scheduler.handoff_expiry` 契约与等价测试覆盖，但“真实 7 天”不是本地 fixture 能证明的 |
| 多 installation 跨实例容量 | `p10_postgres_scheduler` 覆盖单 fixture 内容量计数与条件写；未跑多 installation 并发压测 | 需要多 installation/多实例并发环境或压测 harness |
| 真实平台触发 | 定时触发只在本机多进程（真 mTLS）验证；未接真实 QQ/平台事件链 | 需要真实平台环境（QQ/NapCat）与人工验收 |
| Scheduler authorizer 生产委托来源 | 授权测试用真实 Identity client + fake/服务端 fixture；生产部署的 Scheduler→Identity 委托来源（parent proof/Exchange audience）未接线 | 依赖 P05b 委托来源与生产部署配置，属跨阶段用户确认项 |
| target 契约登记 | 见第 3 节；`target_spec`/overrides/evidence/doc/01 未翻转 | 建议 P10g 按清单登记并跑 contract 门禁 |

## 5. P10 退出判定

**未达退出**。本地已实测：domain/RPC/PG16.4/三进程真 mTLS 定时交接/SDK 任务方法全部通过；
但 target 生产登记未翻转，且真实 7 天等待、多 installation 容量、真实平台触发、生产委托来源
四类环境/跨阶段项未验收。建议 P10g 完成 target 登记翻转（第 3 节清单）后，P10 可标记
“实现+本地实测完成，真实环境项按发布矩阵保留待验收”。

## 6. 契约/文档门禁实跑（2026-11-02，本 HEAD）

| 门禁 | 命令 | 结果 |
|---|---|---|
| 实现状态/文档表/证据 | `python3 proto/tests/check_implementation_status.py` | PASS（历史状态≠target 可用性；P10 未在 target implemented 列表） |
| 语义规则 | `python3 proto/tests/check_semantic_rules.py` | PASS |
| 引用/行为源锚点 | `python3 tests/architecture/check_references.py` | PASS（25 源锚点保留） |
| target 合约 | `bash proto/tests/check_target.sh` | **未通过（环境限制）**：本机 Homebrew protoc 生成的 `google/protobuf/runtime_version.h` 与仓库 vcpkg protobuf 版本不匹配，编译 roundtrip 失败；非本次文档/登记改动导致，建议在 CI/工具链容器内跑该门禁 |
| evidence scopes 单测 | `python3 proto/tests/evidence_scopes_test.py` | **1 项 pre-existing failure**：`test_real_registry_only_probes_are_currently_available_in_target` 的期望集合未包含 P12c/P13 已验证的 `operations.listAuditEntries/listLogs/beginDeleteResource/restoreResource`（registry 本身已登记且主门禁 PASS）；本任务未改 registry，不修测试期望，记录为 gate 维护缺口 |

## 7. 文档影响判断

- 已更新：`doc/01-contracts.md`（execution-scopes 段补充 P10 target 登记缺口说明，不翻转状态
  行）、`doc/02-core.md`（P10a/b 状态与证据）、`doc/05-infrastructure.md`（Scheduler schema
  v9 真 PG 证据）、`doc/22-transactions-and-migration-design.md`（P10a/b 事务/交接实测状态）、
  `doc/23-implementation-plan.md`（P10 总览状态与验收证据），本报告。
- 未更新：`proto/`/`api/`（本任务不改契约）、`doc/18`（受保护基线，未改）、其他模块文档
  （无行为变化）。
- 校验命令见第 1 节；契约门禁（`proto/tests/check_implementation_status.py`、
  `check_semantic_rules.py`、`target_contract_test.py`）在文档提交后运行。
