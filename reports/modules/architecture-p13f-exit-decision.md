# P13f：P13 退出决策包（Runtime→Scheduler 委托）与证据矩阵

日期：2026-11-03；基线 main=`d28020e`（Lead 报告 143/143 门禁绿）。本任务只读分析/文档，**不改代码、proto、权限、RPC 或冻结语义**。

## 1. 退出阻塞（事实核对）

- P13a–P13e 已合入：Runtime BeginDelete/Restore/GetOperation/清理、participants 投影、
  P13b Host/SDK drain、P13c Host participant outbox、P13d 观测行、P13e HostBridge live
  stop/drain 帧与共享 namespace/member 删除隔离证据。
- P10k 已合入 `RuntimeTaskApplication` + `SchedulerTaskClient`（`core/include/kiarabot/core/runtime/platform/scheduler_task_client.hpp`）
  与 PluginBridge task 帧 ingress；生产组合根注入 `unavailable_scheduler_task_delegation()`，
  delegation 为空时稳定 `UNAUTHENTICATED`/`UPSTREAM_UNAVAILABLE`，**不会伪造任务成功**。
- P13 的 Scheduler participant 仍只有 **应用层注入 fake 端口**：`RuntimeResourceApplication`
  接受可空 `SchedulerLifecycleParticipantPort`，但 `core/app/runtime_composition.cpp` 未创建/
  注入生产实现；instance BeginDelete 的 participants 永远含 scheduler 行，故真实 Runtime 进程
  上会一直 BLOCKED，无法到 operation Succeeded/DELETED。
- Scheduler 侧 `SchedulerService.ListTasks/CancelTask` 现有 authorizer（`scheduler_rpc.cpp` /
  `SchedulerIdentityClient`）要求：真实 inbound mTLS 叶 + Identity 签发 `audience=scheduler`
  的 delegation、subject 派生 owner instance、delegation scope 含 Installation，再在线
  `CheckAuthorization`。Runtime 目前没有可获得的这类实例委托（P05b 同源缺口）。
- P10b-2 的 `Scheduler→Runtime AcceptTimerTrigger` 例外是 **单方法平台 peer 豁免**：Runtime 按
  实际 mTLS 叶 URI SAN `spiffe://kiarabot/scheduler…` 验证平台 peer，不要求 delegation；
  `doc/21-service-contract-design.md` 明确该豁免“不扩散到其他 Runtime owner 方法”，且
  “Runtime→Scheduler 的用户/实例委托仍按 P05 delegation 链在线校验”。因此 P13f 的方案 A
  若采用，必须把它视为冻结调用者语义变更，而非既有例外自然延伸。

## 2. 备选方案

### A（推荐待确认）：平台 peer 内部服务授权（对称 AcceptTimerTrigger）

**做法**：Scheduler 的 `ListTasks`/`CancelTask` 增加与 AcceptTimerTrigger 对称的
platform-peer 分支：仅当入站叶证书 URI SAN 为 `spiffe://kiarabot/runtime…`、命中部署级
Runtime 平台 peer allowlist、且部署给 Runtime principal 的显式 capability 覆盖这两个方法时，
允许**不携带用户 delegation**；从请求的 `RESOURCE_INSTANCE` scope 取 owner instance，并在
Scheduler 应用内校验目标 task 的 owner_instance 与 scope 一致、拒绝跨实例/跨安装。其他
Scheduler 方法、以及 ListTasks/CancelTask 的 Control/user 路径保持原有 delegation 校验。

- **冻结语义影响**：中—高。`doc/01-contracts.md`/`doc/21` 的 caller 表与 “Scheduler 六方法
  caller=Runtime with instance delegation or scoped Control” 需要改为“Runtime platform peer
  (仅 ListTasks/CancelTask 的实例任务候选) 或原委托”；这是 authz 调用者语义变更，**无新增
  proto 字段/权限编号/RPC**，但需要用户确认。
- **实现范围**：Scheduler service config 增加 Runtime platform peer 允许项（或复用 Identity
  platform_peers 已注册条目 + Scheduler 部署配置；不新建 RPC）；`scheduler_rpc.cpp`
  authorizer 增加 platform-peer 分支与 scope/owner 校验；`SchedulerLifecycleParticipant`
  生产实现（可复用/扩展 `SchedulerTaskClient` 增加 list/cancel 方法与 mTLS 通道，或新增
  lifecycle client）；Runtime composition 注入；identity/platform peer 引导文档。
- **风险**：绕过每请求 Identity 在线授权/实例委托撤销传播；必须限制到恰好两个方法、禁止
  Control 伪造 Runtime SAN、严格 owner/task scope 匹配、平台 peer 证书轮换/撤销流程；否则
  可能扩大 Runtime 内部服务权限。
- **验证**：Scheduler RPC fixture（Runtime 平台叶 SAN 允许/拒绝、非 Runtime SAN 拒绝、scope
  不匹配/跨实例拒绝、Drain/其他方法仍拒绝）；真实 Runtime→Scheduler mTLS e2e
  （List/Cancel 成功、撤销/离线 fail-closed）；P13 联合 e2e（真 Host+真 Scheduler 或
  fake Host+real Scheduler）BeginDelete→participants Done→operation DELETED、Restore 不复活。
- **用户确认**：**必须**（冻结 authz 语义变更；与 P05b 同类决策）。

### B：Control 签发实例委托

**做法**：Control 以登录会话调用 Identity `IssueDelegation`，为 audience=scheduler、含
 Installation+Instance scope 的委托交给 Runtime；Runtime 再以该委托调用 Scheduler。

- **冻结语义影响**：高。Identity `IssueDelegation` 的 subject 恒为 Control 登录会话主体；
  Scheduler 按冻结 P10 规则从 delegation subject 派生 owner instance，因此得到的是 Control
  用户而非插件实例，破坏 per-instance 隔离与 owner 派生。要得到 subject=插件实例，必须给
  Identity 增加“Runtime/平台服务为受管实例签发实例委托”的语义（P05b 已因同类 authz 变更
  被用户暂缓），或改 Scheduler 的 owner 派生规则。
- **实现范围**：Identity issuance/exchange 语义+授权模型、Control 编排/撤销、P05b 联动、
  Scheduler 校验调整；跨服务安全评审，范围远大于 A。
- **风险**：最高；容易变成“Control 用户冒充插件实例”或长期委托扩散；撤销/租约/审计语义
  需要重新定义。
- **验证**：Identity 签发/撤销单测、真实 Control→Identity→Runtime→Scheduler 三/四进程
  mTLS e2e、越权/跨实例/撤销负例、与 P05b 一致的安全评审。
- **用户确认**：**必须**，且建议优先解决 P05b 的同类 instance-delegation 语义再谈 B。
- **结论**：不推荐；除非用户明确批准 Identity 委托冻结语义的扩展。

### C：测试注入/明确延期（非生产证据）

**做法**：保持生产 fail-closed；用现有 `RuntimeResourceApplication` 的可空
`SchedulerLifecycleParticipantPort` 注入 fake 端口（Done/Blocked/Unknown），跑联合 e2e 断言
participants 收敛/不伪完成；Host 侧仍用真实 PG16.4 + 真实 HostBridge（P08e/P13e 已证明）或
真实 Host，标注 **test-only / 非生产**。不新增任何契约/权限/代码语义。

- **冻结语义影响**：无。只是测试注入边界；但 P13 退出不能宣称生产通过。
- **实现范围**：C++ 应用级测试（现有 fake 端口扩展为真实 PG + 真 HostBridge 的联合 fixture，
  或新增测试组合根）或测试专用 composition driver；不改生产 composition。
- **风险**：证据边界被误读为生产已验收；需在报告/CTest 标签明确 `test-only`，且 P13 退出
  判定保持“生产阻塞”。
- **验证**：`p13_runtime_lifecycle` 扩展（real PG + real HostBridge + fake scheduler Done/
  Blocked + Host receipt/UNKNOWN），断言 BeginDelete→Host Done→Scheduler fake Done→DELETED、
  Restore、UNKNOWN 不完成；Host 离线/调度器 fake Blocked 不完成。
- **用户确认**：若仅作为 test-only 证据，Lead 可批准；若作为 P13 退出替代则需用户确认延期。

## 3. 推荐

**推荐 A 作为生产路径（需用户确认），C 作为在确认前的立即可用非生产证据；不建议 B。**
理由：A 不改 proto/权限编号/RPC，复用 P10b-2 已验证的平台 peer 证书/SAN 授权范式，范围
最小且与现有内部 mTLS 架构一致；但必须明确收窄到 `ListTasks`/`CancelTask` 两方法、严格
owner/instance scope 校验，并同步修订冻结 caller 语义。B 需要与 P05b 同类的 Identity
authz 语义扩展，风险/范围明显更大；C 不能关闭生产退出条件，只避免阻塞测试与证据刷新。

**需用户确认点**
1. 是否批准平台 peer 豁免扩展到 Scheduler `ListTasks`/`CancelTask`（仅 Runtime 实例生命周期/
   插件任务候选路径）及其冻结 caller 语义修订。
2. 平台 peer 的授权锚：Scheduler 部署 allowlist vs Identity platform_peers 登记 + Scheduler
   本地显式 capability（推荐两者都用：部署 allowlist 做启动门禁，Identity assignment 做
   离线能力授权，不引入新权限编号）。
3. 是否接受 C 作为确认前的 test-only 联合 e2e 证据（P13 仍标生产未通过）。
4. 是否要求同时解决 P05b 的 instance-delegation 语义（否则 B 不可行；A 不依赖 P05b）。

## 4. 若选 C：现有注入端口跑联合 e2e 的可行方式

- **应用级 C++ 测试**（推荐先做）：用 owner 真实 PG16.4 factory + 真实
  `RuntimeHostLifecycleParticipant`/`RuntimeResourceApplication`，注入 fake
  `SchedulerLifecycleParticipantPort`（Done/Blocked/Unknown）；用真实 HostBridge fake Host
  发送 stop receipt（或直接在该 PG 的 host_commands 上调用 receipt 仓储）；轮询
  `lifecycle_maintenance`/`GetOperation` 断言 participants 从 BLOCKED→Done、operation 终态
  Succeeded（DELETED）。Host 离线/UNKNOWN/调度器 Blocked 分支必须保持非终态。标签注明
  `test-only`，不改变生产组合根。
- **进程级 C** 需要测试专用 composition driver（在测试中装配真实 Runtime app + fake
  scheduler，不启动生产 Runtime 主程序），因此按本任务“不改代码”仅描述、不实现；若 Lead
  批准可在后续增量新增测试驱动（不改生产行为）。

## 5. P13 证据矩阵刷新（含 P13a–e）

| doc/23 P13 验证点 | 状态 | 证据（测试/报告） |
| --- | --- | --- |
| 重复删除/恢复并发、已删恢复、窗口外恢复 | 已实测（应用/单机） | `p13_runtime_lifecycle`（fake/in-memory）；`p05_runtime_identity_e2e`（真 PG/Identity/Runtime 的 Account BeginDelete/Restore） |
| 共享成员删除不清其他人数据 | **已实测** | `p06_bridge_acceptance` P13e 段（真 PG16.4 + storage frames：成员撤销/namespace 删除隔离） |
| 清理批次失败；满盘/只读 | 部分实测 | `p13_runtime_lifecycle` cleanup fault injection；真实满盘需 P14 环境 |
| 失效 outbox 满/容量（四库） | 部分实测 | `p03_postgres_database`（Identity outbox/capacity）、`p08_*`（host command pending/prune）；无四库同时打满压力 |
| 未决副作用 UNKNOWN/EVIDENCE_ONLY 不提前清理 | 已实测 | `p13_runtime_lifecycle` 保留未决；`p07_action_rules`/`p07_postgres_action_records`；`p08_host_journal`/`p08_postgres_host_registration`/`p13_host_sdk_drain` |
| 保留期与配额（30/180 天、60s 批次、period quota） | 已实测（应用/PG）；生产规模待环境 | `p13_runtime_lifecycle`、`p03_postgres_database`、`p07`/`p09` 配额测试；长周期/生产数据量未验证 |
| Host/SDK drain 与 UNKNOWN 不伪 stopped | 已实测 | `p13_host_sdk_drain`（真实 Host+SDK SIGTERM/SIGKILL/重启/journal） |
| Host participant outbox 收敛 | 部分实测 | `p13_runtime_lifecycle`（fake receipt BLOCKED→Done/UNKNOWN 不完成）；`p08_host_bridge_e2e`（P13e live Stop/Drain 帧 + receipt PG）；**缺 BeginDelete→真实 Host receipt 单链** |
| Scheduler participant ListTasks/CancelTask | 未验收（仅 fake 语义） | `p13_runtime_lifecycle` fake 端口 BLOCKED→Done/离线/超时；生产 Runtime composition 无 participant，Scheduler 委托未确认 |
| BeginDelete/Restore 与真实 Host 单链到 DELETED | **未验收** | 依赖 Scheduler participant 生产路径；P13e 已记录 |
| 跨主机/真实平台故障/生产规模 | 未验收 | P14 环境项 |
| 指标/告警/集中日志 | 部分实现（最小观测行/审计查询），集中 metrics/告警未交付 | P13d 报告/doc/08 |
| P10k task ingress + fail-closed 委托（非 P13 participant） | 已实测 fail-closed | `p10_sdk_tasks_e2e`、`p10_scheduler_rpc`、P10k 报告；真实 Runtime→Scheduler 委托仍待确认 |

**P13 退出判定**：**未通过**。核心阻塞 = Scheduler participant 生产委托/授权（方案 A 或 B 未获用户确认）
以及缺少 BeginDelete/Restore→真实 Host receipt 的单链 e2e；C 可提供 test-only 证据但不能替代生产退出。
