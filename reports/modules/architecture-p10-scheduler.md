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

## 8. P10g target 登记（2026-11-02，后续任务）

P10g 已按本报告第 3 节候选完成 target 登记（只翻真实实现+测试通过项）：
- `target_spec.json`：SchedulerService CreateTask/CancelTask/GetTask/ListTasks/GetOccurrence/GetOperation
  → `implemented`，`exposure=wire`，附 status_note 引用本报告。
- `execution_scopes.target.overrides` + `evidence`：proto_messages 14 项、proto_fields 52 项
  （含 `TimerTriggered.occurrence_id`）、semantic_rules 4 项；evidence 统一指向
  `core/src/application/target/scheduler_{application,rules,service}.cpp`、
  `core/src/runtime/platform/scheduler_{rpc,runtime_client,identity_client}.cpp` 与
  `core/tests/platform/{scheduler_service_test,scheduler_rpc_test,scheduler_identity_client_test,p10_timer_e2e_test.py}`。
- doc/01 execution-scopes 表补三行覆盖上述 70 项；doc/23 P10 状态更新为已实现+已登记。
- **未翻转**：`operations.getTask/cancelTask/getOccurrence` 3 项——Control REST owner_gateway
  已实现，但 target evidence 要求 `tests/architecture/`/`core/tests/{platform,architecture}/`
  下的真实测试；当前任务 REST 验收位于 `control/api/tests/`，不在限定路径内，故按“不证不翻”
  保持 not_implemented，需补 target 目录 REST 测试后再翻。真实 7 天 EXPIRED/多 installation
  容量/真实平台触发/生产委托来源仍为待环境项。
- 门禁：check_implementation_status PASS（含 doc/01 行与 evidence 校验）、
  `evidence_scopes_test.py` 9/9 PASS（expected 集合补 P10g 70 项 + main 既有 P12c/P13 4 个
  operation 探针）、target_api_test PASS、check_contracts.sh PASS；check_target.sh 仍受本机
  protoc/protobuf 版本限制（记录替代证据）。

## P10h Scheduler 任务 REST target 翻转与证据（2026-11-02，HEAD 439fb9e）

工作树 `.worktrees/p10h`，分支 `codex/p10h`，基于 main@439fb9e。P10g 的 Scheduler owner RPC target 已合入；P10h 补齐 Control 任务 REST 的 target 目录证据并翻转 REST operation。

### 新增验收（允许的 target 证据目录）
`tests/architecture/p10_scheduler_rest.py`：真实 `kiarabot-control` 进程 + 真 TLS + Identity/Model/Scheduler owner fixture（`auth_https_test` 同源 fixture 二进制），通过真实 Control HTTPS 边界断言：
- `GET /api/v1/tasks?scope=...&page_size=2` → 200，items `task-fixture`，page_size 转发（分页字段）；
- `GET /api/v1/tasks/{id}?scope=...` → 200 task 投影；
- `POST /api/v1/tasks/{id}/cancel?scope=...` → 200 + owner task 结果（`SCHEDULED_TASK_CANCELLED`），同 Idempotency-Key 重复 cancel 仍 200 同结果；
- owner CONFLICT（fixture marker）→ 409 CONFLICT；Control 路由空 task → 404 NOT_FOUND；
- `GET /api/v1/occurrences/{id}` 当前无 Control handler → 404（不伪成功，故 getOccurrence 不翻转）。
CTest：`p10_scheduler_rest`（labels `p10;control;scheduler;integration;fault`，RESOURCE_LOCK control_https，1.88s）。

### target 翻转/核对
- 翻转 `operations.getTask`、`operations.cancelTask` target override + evidence（`control/api/src/target/owner_gateway.cpp`、`control/api/app/main.cpp`、`tests/architecture/p10_scheduler_rest.py`、`control/api/tests/owner_gateway_test.cpp`）。
- `operations.getOccurrence` **保持 not_implemented**：SchedulerService.GetOccurrence owner RPC 已 implemented，但 Control 尚无 `/occurrences/{id}` REST handler；没有成功链证据不能翻转，未新增 handler（Control 实现不在本任务写范围），作为 P10 退出缺口报告 Lead。
- target_spec Scheduler 六方法（Create/Get/Cancel/List/GetOccurrence/GetOperation）此前已 implemented，本次不变；`schema/契约` 未改。REST cancel 契约成功响应是 **200**（`TargetSchedulerCancelTaskResponse`），不是 202；测试按现有 OpenAPI 契约断言，未为满足任务措辞改实现/契约。
- doc/01 execution-scopes 增加 `operations.getTask/cancelTask` implemented 行与 `getOccurrence` 未实现说明；evidence_scopes 冻结集加入两项。

### 实际运行
```text
cmake --build --preset full-locked --parallel 8   # build=0
ctest --preset full-locked -R 'p10|p12|control|p04_control|p03_postgres'
# 18/18 passed, 158.67s（含 p10_scheduler_rest 1.88s）
proto 门禁：check_implementation_status / target_api_test / evidence_scopes_test / check_contracts.sh 全通过
```
（前一轮 P10g 门禁 143/143 的漂移已由 core-target 修复合入，本次门禁绿。）

### P10 退出判定
P10a/g 的 Scheduler domain/RPC/PG16.4/三进程 mTLS、P10g target RPC 翻转保持通过；P10h 使 `operations.getTask/cancelTask` 的 Control REST 成功链有 target 证据并翻转。**getOccurrence REST 仍未实现/未翻转**，因此 P10 退出为“目标 RPC + list/get/cancel REST 已验收；getOccurrence REST 待 P12 Control handler；真实 7 天 EXPIRED、多 installation 容量、真实平台触发/委托来源仍待环境”。

## P10j SDK 任务方法端到端验证（2026-11-02，HEAD 3f43c8f）

工作树 `.worktrees/p10j`，分支 `codex/p10j`，基于 main@3f43c8f。本任务验证 SDK 任务方法（task_create/get/cancel），并核对生产宿主链路。

### 实际运行
```text
cmake --build --preset full-locked --parallel 8   # build=0
ctest --preset full-locked -R 'p10|p14_native_single_host'
# 8/8 passed, 89.80s
# p10_scheduler_service/p10_scheduler_identity/p10_scheduler_rpc/p10_timer_e2e/
# p10_sdk_tasks_e2e(5.16s)/p10_scheduler_rest/p10_postgres_scheduler
# p14_native_single_host 15.74s（Identity+Runtime+Scheduler+Model+Control 原生五进程真 PG16.4 栈）
```
P14f 原生单机栈本身通过（真实五进程 + 临时 CA/叶子证书 + owned PG16.4 + Control HTTPS），但 P14f harness **不连接 PluginBridge、不覆盖 SDK 任务帧**；只能作为栈健康证据，不能替代 SDK 任务全链路。

### 新增 SDK 端到端协议验收
`tests/architecture/p10_sdk_tasks_e2e.py`（CTest `p10_sdk_tasks_e2e`，labels `p10;sdk;integration;fault`）：真实 SDK Runner 经真实 TLS `PluginBridge` fixture（`kiarabot_sdk.testing.FakeTargetRuntime`）执行 task_create/get/cancel；fake scheduler owner 实现任务状态机与错误语义，覆盖：
- create→PENDING、get 回 task/version、同幂等键+同内容回放、同键异内容 CONFLICT；
- cancel PENDING→CANCELLED、重复 cancel 同结果；COMPLETED（occurrence_id 存在）为终态，cancel→CONFLICT；
- capacity 满→RATE_LIMITED retryable=true；
- DST gap（America/New_York 2027-03-14 02:30）与未知 zone→INVALID_ARGUMENT（SDK 不做本地换算，稳定码由 owner 回传）；
- 未知/跨实例 task_get→NOT_FOUND；
- 断言 COMPLETED 只代表 TimerTriggered 已入队（SDK doc 语义），不代表插件回调成功。

### P10k 宿主 task 帧 ingress（已实现，委托仍 fail-closed）
P10j 报告的“Runtime 无 task 帧消费”已由 P10k 修复：`runtime_bridge.cpp` 现在处理
task_create/get/cancel，校验认证 instance/owner/installation、`host.tasks.v1`、
`task.schedule`/`task.read`、`event.timer.triggered` 与幂等/输入上限，经新增
`RuntimeTaskApplication` 调用 `SchedulerTaskPort`；`SchedulerTaskClient` typed client 与可选
`scheduler_endpoint` 已接线。**真实 Runtime→Scheduler delegation 来源尚未确认（P05b 同类）**，
生产 composition 注入 fail-closed provider，调用稳定返回 `UNAUTHENTICATED`/`UPSTREAM_UNAVAILABLE`，
不会伪造成功。真实 TLS PluginBridge 夹具（P10k 扩展 `p11_model_bridge`，labels p10/p06/p11）
验证 ingress→application→fake port 的成功/权限/幂等/取消/CONFLICT/超时/provider fail-closed。
因此 SDK→真实 Runtime→真实 Scheduler 的成功链仍待“委托来源确认 + 真 Scheduler 联测”，
不能宣称 P10 原生成功链验收通过。另：冻结 `TaskCreateRequest` 只带 `target_conversation` 字符串、
无会话类型；P10k 暂以 GROUP 构造 Scheduler `ConversationKey`，私聊任务类型需用户确认/后续契约澄清。
另：tzdata 缺失稳定错误（UPSTREAM_UNAVAILABLE）由真实 Scheduler readiness 决定，fake owner 未模拟；真实 qq/平台触发仍需环境。
