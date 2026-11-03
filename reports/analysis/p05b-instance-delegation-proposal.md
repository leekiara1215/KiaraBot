# P05b：Runtime 实例委托语义方案（供用户决策，只读设计）

- 日期：2026-11-03
- 基线：main=`fa3030c`（本 worktree `codex/p05b`，只读设计，不改实现/proto/权限/契约）
- 任务：task-142（P05b 方案文档）
- 关联：P13f/`architecture-p13f-exit-decision.md`、P13g test-only 证据、P10k/P11/P12 报告、
  P05f 报告、doc/01-contracts、doc/21-service-contract-design、doc/27-phase-exit-evidence
- 明确非目标：本文不实现、不改 proto/权限编号、不启动新的生产接线；所有“方案”都是候选，
  涉及冻结语义的选项在用户批准前保持 fail-closed。

## 0. 结论摘要

P05b 的结构性缺口不是“缺少一个 endpoint”，而是**冻结的委托链无法表达“插件实例”这个主体**：
Identity `IssueDelegation` 的 subject 恒为 Control 登录会话的人类 principal（
`core/src/application/target/identity_delegation_application.cpp`），
`ExchangeDelegation` 只能把该 subject 换成 audience=Model/Scheduler 的
`business_hops=2` 子委托（`identity_exchange_application.cpp`），而 Model `Chat`/Scheduler
`ListTasks/CancelTask` 都要求 owner 从 delegation subject 派生。当前不存在“subject=插件实例”
的父委托来源，因此 Runtime 生产 provider 只能 fail-closed（P10k/P11 阻塞）。

**推荐组合（需用户批准）**：

1. **Scheduler 生命周期路径采用方案 A（用户已批 P13 限定 A）**：仅
   `SchedulerService.ListTasks`/`CancelTask` 接受 Runtime 平台 peer + 显式 capability +
   实例/任务归属校验，不加宽到其他方法；用于 P13 生产验收（BeginDelete 在真实 Host 回执 +
   Scheduler 清理都 Done 后才 DELETED）。
2. **Model/P11 单链路采用方案 B 的限定变体**：批准一次受控的**实例主体父委托签发**
   （subject=已注册 Plugin principal / RESOURCE_INSTANCE，audience=runtime，短 TTL、证书绑定、
   安装+实例 scope、在线授权），Runtime 用既有 `ExchangeDelegation` 换取 audience=model
   `business_hops=2` 子委托后调用 Model；Model 的 owner=delegation subject 语义保持不变。
3. **不推荐方案 C 作为主方案**：长凭据/本地授权缓存与冻结的 ≤56s 委托、逐请求在线
   `CheckAuthorization` 冲突，且不能自动获得实例主体父委托；最多作为 B 之后的延迟优化。

需要用户显式批准的**重大结构变更**只有一项：Identity 委托签发语义扩展（实例主体签发，
见方案 B）。方案 A 已是用户批准的 P13 范围，但需确认其生产实现与 P13 验收标准。决策问题
见第 7 节，落地拆分与验收见第 8 节。

## 1. 背景与已批准边界

### 1.1 用户已批准 P13 方案 A（限定）

来自 P13f 决策包：**仅 `ListTasks`/`CancelTask`**、mTLS 平台身份 + 部署 allowlist + 显式授权、
instance/task 归属校验、跨实例/跨安装拒绝；生产验收要求 **BeginDelete 等真实 Host 回执 +
Scheduler 清理完成才 DELETED**。P13f 方案 A 明确“不新增 proto 字段/权限编号/RPC”，但属于
冻结 caller 语义变更，需要用户确认（用户已批准限定范围）。

### 1.2 生产验收的既有机制

- Host 参与者早已接生产实现：`make_runtime_host_lifecycle_participant` 写 durable
  `StopExecution` host command；只有 receipt `KnownSucceeded/KnownFailed` 才算 Done，
  Pending/Unknown/拒绝保持 BLOCKED（P13c、`runtime_lifecycle_participants.cpp`）。
- Scheduler 参与者端口 `SchedulerLifecycleParticipantPort` 存在，但生产 composition 传入
  `nullptr`（`core/app/runtime_composition.cpp:364-375`），test-only 的 P13g 注入 fake
  （`#ifdef KIARABOT_P13G_TEST_COMPOSITION`，同文件 205-249）。因此**真实 Runtime 上
  BeginDelete 的 Scheduler participant 永远缺失/不完成，operation 无法 DELETED**。
- P13g 已提供 test-only 联合 e2e（`tests/architecture/p13_joint_delete_e2e.py`，
  CTest labels `p13;test-only`，`infrastructure/postgres/CMakeLists.txt:91-102`），证明
  “Host receipt + fake Scheduler Done → DELETED；UNKNOWN/offline → 不完成”，但报告明确
  **非生产证据**（`reports/modules/architecture-p13-lifecycle.md` P13g 段）。

## 2. 现状与阻塞点（代码 + 测试证据）

### 2.1 冻结委托语义（不可静默改变）

| 不变量 | 事实 | 证据 |
| --- | --- | --- |
| 短期证明 | 委托 TTL `expires-issued <= 56s`，`business_hops ∈ {1,2}`，actor_chain 长度=hops、末位=direct_actor | `infrastructure/identity_crypto/src/delegation_crypto.cpp` shape/verify |
| 权限/资源 | Permission 1–50；ResourceKind 内部上限 Catalog；Catalog id 为契约 printable id（P14m） | `delegation_crypto.cpp`、`target_codec.cpp` |
| 签发主体 | `IssueDelegation` 由 Control peer + 登录会话发起，subject=会话 principal，`business_hops=1`，client_certificate=Control 叶；须管理员对该 subject 的每个 permission/scope 有精确授权 | `identity_delegation_application.cpp` issue_delegation |
| 交换 | `ExchangeDelegation` 仅接受 Runtime peer + 父委托 audience=runtime/hops=1；子委托 audience∈{model,scheduler}、`business_hops=2`、actor_chain=[Control,Runtime]、client_certificate=Runtime 叶；permissions 必须是父集合子集，且原 subject 仍有对应精确授权 | `identity_exchange_application.cpp` exchange/authorize_exchange |
| 下游验证 | Scheduler 从 delegation binding 取 Installation 派生 installation、owner=subject（`scheduler_rpc.cpp`/`scheduler_identity_client.cpp`）；Model 要求 audience=model、client_certificate=实际 Runtime 叶、hops=2 才 `runtime_origin`、owner=subject，然后在线 `CheckAuthorization(full_method, scope)` | 两处 runtime client + Model app authorize |
| 在线权威 | Model/Scheduler 均每请求在线检查授权版本；委托只作短期证明，不替代授权 | P04/P05 报告、`p05_runtime_identity_e2e` |
| Model Chat 载荷 | `ModelGatewayChatRequest` 只有 context/logical_call_id/model_id/messages，**没有 owner/instance 字段**；owner 必须来自 delegation subject | `proto/kiarabot/v1/model.proto:138-143`、`doc/01-contracts.md` P11 契约节 |
| Task 载荷 | `TaskCreateRequest` 只带 target_conversation，无实例/owner；owner_instance/installation 由 Runtime→Scheduler 实例委托 claims 派生 | `doc/01-runtime-semantics.md`、`doc/01-contracts.md:1620` |

### 2.2 P11 单链路阻塞（Model）

- `RuntimeModelClient` 生产 composition 注入 `unavailable_model_delegation()`
  （`core/app/runtime_composition.cpp:435-446`，声明见
  `core/src/runtime/platform/runtime_model_client.cpp:56-60`），delegation 为空时稳定
  `UNAUTHENTICATED`；因此真实插件 `HostRequest.model_chat` 经 Runtime 到 Model 只能 fail-closed。
- P11 报告明确：真实插件→Runtime→Model→Provider **单链路未合入（P05b 委托语义待确认）**，
  不能把分段证据当单链路验收；SDK target Model 用例整体 skip：
  `sdk/python/tests/test_model.py:13` `deferred: target Model e2e needs P06a Bridge handler +
  P08 managed Host`（`reports/modules/architecture-p11-model.md` P11f 段）。
- 已有分段证据仅覆盖：插件 ingress→app→fake port（`core/tests/platform/model_bridge_test.cpp`，
  CTest `p11_model_bridge`，`cmake/platform.cmake:404-409`）、Control→Model REST e2e
  （`p11_control_model_rest`）、Model 单进程/owner 测试；均不能替代真实委托链。

### 2.3 P10k 阻塞（Scheduler 任务委托）

- P10k 已实现 `RuntimeTaskApplication` + `SchedulerTaskClient` + PluginBridge
  task_create/get/cancel ingress；生产 composition 同样注入
  `unavailable_scheduler_task_delegation()`（`core/app/runtime_composition.cpp:405-421`），
  返回稳定 `UNAUTHENTICATED`/`UPSTREAM_UNAVAILABLE`，绝不伪造成功
  （`scheduler_task_client.cpp:131-135`）。
- 测试仅到 fake Scheduler：`core/tests/platform/model_bridge_test.cpp:553+`（P10k 段）验证
  ingress/owner 派生/幂等/CONFLICT/TIMEOUT/denied/fail-closed；`tests/architecture/p10_sdk_tasks_e2e.py`
  docstring 明确“production Runtime delegation provider 故意 fail-closed，本 fixture 不是
  native-stack acceptance”。
- Scheduler 的 `ListTasks/CancelTask` authorizer 要求真实 inbound mTLS 叶 + Identity
  audience=scheduler 委托、delegation scope 含 Installation、owner 从 subject 派生，再在线
  `CheckAuthorization`（`scheduler_identity_client.cpp`、P13f §1）；Runtime 无来源。

### 2.4 P13 production 验收阻塞（BeginDelete→DELETED）

- 生产 composition 的 Scheduler participant 为 `nullptr`（§1.2），真实 Runtime 的
  BeginDelete 永远停在 Scheduler participant 未完成；Host participant 已有真实 receipt 路径。
- P13g test-only 证明联合状态机可行，但报告标记“非生产证据”；生产验收还缺“真实 Scheduler
  清理 + 真实 Host 回执 → participants Done → operation Succeeded/DELETED”的真实链路。
- 因此 P13 生产验收与 P05b/P10k 共享同一 Scheduler 委托来源问题；方案 A 只解 Scheduler 生命周期
  两个方法，不解 P11 Model 单链路。

### 2.5 结构性缺口（结论）

1. 冻结 `IssueDelegation` 只能以**人类登录主体**为 subject，不能产生 subject=插件实例的父委托；
2. `ExchangeDelegation` 已具备把父委托换成 audience=model/scheduler、hops=2 子委托的能力，
   但前提是父委托 subject 已经是实例主体；
3. Scheduler 可用平台 peer + 显式 resource scope 绕过逐请求委托（P13 A），因为 ListTasks/
   CancelTask 请求自带 INSTANCE/TASK scope、owner 校验可在 owner 内完成且用户已批准；
4. Model Chat 没有实例 scope/owner 字段，owner=delegation subject 是冻结语义；平台 peer 方案
   若要保留 per-instance owner，必须改 Chat 契约/owner 派生（重大变更），否则会退化为
   Runtime 平台主体统一计费/授权，破坏实例隔离；
5. 长凭据/本地缓存（C）不解决“实例主体父委托从哪来”，且与 ≤56s + 在线检查语义冲突。

## 3. 设计约束（所有方案必须满足）

- 不得把插件自报的 owner/instance 当作授权依据；owner/installation 必须由可信路径派生。
- 跨实例/跨安装请求必须稳定 NOT_FOUND/PERMISSION_DENIED，不能靠“调用方自觉”。
- 委托/凭据不得写入日志、审计、outbox、operation、argv；失败保持 fail-closed，不伪造成功。
- 任何涉及 proto 字段、权限编号、冻结 caller/owner 语义的变化都是**重大结构变更**，必须
  用户批准并同步 doc/01、doc/21、proto/target_spec 与验收证据。
- P13 生产验收硬条件：BeginDelete 只有 Host receipt 终态 **且** Scheduler 清理 Done 后
  operation 才可 DELETED；UNKNOWN/offline/Blocked 不得完成。
- P11 验收必须是真实插件→Runtime→Model→Provider 单链路，不接受 fake 端口/分段证据。

## 4. 备选方案

### 方案 A：平台 peer 内部服务授权（对称 P13 A，限定范围）

**做法**：针对枚举的 owner 方法，owner 接受 Runtime 平台 peer（真实 mTLS 叶/SAN +
部署 allowlist + Identity 显式 capability），不要求用户 delegation；从请求 scope 派生
instance/task，owner 内校验归属；其他方法保持委托校验。

- **冻结语义影响**：中—高（caller 表变更：`doc/21`/`doc/01-contracts` 的
  “Runtime with instance delegation or scoped Control”需补“Runtime platform peer（仅枚举方法）”），
  无新增 proto 字段/权限编号/RPC。**P13 A 用户已批准仅 ListTasks/CancelTask**；扩展到 Model
  Chat **不在已批准范围**，且 §2.5 说明 Chat 无实例 scope，不能直接套用。
- **实现范围**：Scheduler：service config allowlist + authorizer platform-peer 分支（复用
  P10b-2 `AcceptTimerTrigger` 认证范式）+ Runtime `SchedulerLifecycleParticipant` 生产实现
  （复用/扩展 `SchedulerTaskClient`，平台证书、无 delegation）+ composition 注入；Identity：
  platform peer 登记/显式 capability（P05 离线 bootstrap 已有 Runtime platform peer
  机制）+ 启动门禁；Model：**不适用**（除非改 Chat 契约/owner 语义，见方案 B）。
- **证书/凭据绑定**：Runtime 平台 clientAuth 证书（URI SAN `spiffe://kiarabot/runtime`），
  部署 allowlist + Identity platform principal assignment；无用户委托、无长期 secret。
- **scope 派生**：请求 `ResourceScope`（ListTasks/CancelTask 的 INSTANCE/TASK）→ owner
  instance；Scheduler 校验 task.owner_instance/installation 与 scope 一致；跨实例/跨安装拒绝。
- **过期/撤销**：证书到期/轮换、allowlist 移除、Identity assignment 撤销；无逐请求在线
  authorization（撤销传播依赖部署刷新/连接重建），必须在文档与测试中明确窗口。
- **失败语义**：非 Runtime SAN/未 allowlist/无 capability → `UNAUTHENTICATED`/`PERMISSION_DENIED`；
  scope 不符/跨实例 → `NOT_FOUND`/`PERMISSION_DENIED`；owner 不可达 → `UPSTREAM_UNAVAILABLE`
  （P13 participants 保持 BLOCKED）。
- **测试验收**：Scheduler RPC fixture（Runtime SAN 允许、非 Runtime SAN 拒绝、无 capability
  拒绝、scope 不匹配/跨实例拒绝、其他方法仍拒绝）；真实 Runtime→Scheduler mTLS e2e
  （ListTasks/CancelTask 成功、证书/SAN 撤销后 fail-closed）；P13 生产 e2e：真实 Host receipt +
  真实 Scheduler 清理 → DELETED，UNKNOWN/offline 不完成。
- **风险/工作量**：风险中（绕过逐请求在线授权；必须限定方法/严格 scope/证书轮换流程），
  工作量 M（Scheduler + Runtime composition + 测试），**不解 P11 Model**。

### 方案 B：Control 签发实例委托（subject/audience/scope/证书绑定）

**做法**：扩展 Identity 委托签发，使可信方能够为**已注册的插件实例 principal**
（`PrincipalKind::Plugin`，binding=RESOURCE_INSTANCE，见 `identity_admin_application.cpp`
与 `identity_policy.hpp`）签发父委托：subject=该实例 principal，audience=runtime，
permissions/scope 为安装+实例（任务）和 model 资源（Chat）的精确集合，client_certificate
绑定 Control 叶，TTL≤56s，`business_hops=1`。Runtime 用既有 `ExchangeDelegation` 换取
audience=model/scheduler、`business_hops=2`、client_certificate=Runtime 叶的子委托，再调用
Model/Scheduler；下游保持现有验证 + 在线 `CheckAuthorization`，owner=subject=实例。

- **签发者变体**：
  - B1（推荐）：Control 以管理员会话 + 显式“为实例签发”授权调用扩展后的 IssueDelegation；
    subject 由实例绑定/管理员授权确定，不能任意指定；请求/响应可能需新增 `subject` 或实例
    binding 字段（**proto/冻结语义变更**），或使用新的受控 RPC。
  - B2：Instance principal 以自身 mTLS 凭据向 Identity 自签发短委托（不经过 Control）；
    需要 Identity 接受 workload principal 作为 IssueDelegation 主体（同为冻结语义变更），
    但更贴近 per-instance 主体；Runtime 侧只做 Exchange 与转发。
- **冻结语义影响**：高。新增“实例主体委托签发”语义，改变 IssueDelegation caller/subject
  规则；需要新审批、proto/权限策略审计、撤销模型；不改 Model/Scheduler 已有验证/owner 语义。
- **实现范围**：Identity issuance/exchange 授权链（当前 IssueDelegation 仅 Control+session；
  Exchange 已支持 hops2，需扩展父委托来源）、Runtime delegation provider（获取/刷新父委托、
  Exchange、缓存 ≤56s、失败 fail-closed、不落日志）、Control 编排（B1）或实例凭据/
  身份注册（B2）、模型/调度 owner 无需改（仅可能补 capability/scope 说明）、运维文档/
  证书轮换。
- **证书/凭据绑定**：父委托 client_certificate=Control 叶（B1）或实例 principal 凭据（B2）；
  子委托 client_certificate=Runtime 叶（Exchange 强制）；下游 verify 实际叶指纹；无长期
  用户 secret，实例 credential 生命周期与注册/撤销绑定。
- **scope 派生**：签发时显式 ResourceScope：Task 需要 Installation+Instance（P10 派生约定）；
  Model 需要 Model/模型 id 精确 scope；Runtime Exchange 时只能收窄到父集合子集；下游按冻结
  owner=subject（实例 principal）派生，Model Chat 无需新字段。
- **过期/撤销**：父/子委托均 ≤56s，Runtime 在安全窗口内刷新；实例 principal
  authorization_version/installation 状态、Control 管理员授权撤销、credential 撤销都会在下一次
  online CheckAuthorization 生效；RPC 失败/Identity 不可达 → fail-closed（不伪造成功）。
- **失败语义**：无法签发/过期/刷新失败 → `UNAUTHENTICATED`（Runtime provider 稳定失败，
  P13 participant BLOCKED、P10k task 帧稳定错误）；跨实例/跨安装/scope 不符 →
  `PERMISSION_DENIED`/`NOT_FOUND`；在线检查拒绝 → `PERMISSION_DENIED`。
- **测试验收**：Identity 签发/撤销/跨实例正负例（含任意 subject 伪造拒绝、证书绑定、TTL、
  权限子集、scope 精确匹配）；Runtime provider 单元/双进程 mTLS；真实 P11 单链路 e2e
  （managed plugin→Bridge model_chat→Runtime→Model→真实 HTTP Provider fixture，成功/撤销/
  过期/跨实例负例）；真实 P10k task e2e；P13 生产 DELETED e2e。
- **风险/工作量**：风险最高（安全/冻结语义变更、审计与撤销模型），工作量 L；需要用户
  明确批准 + 安全评审。

### 方案 C：Runtime 长凭据/本地授权缓存 + online check

**做法**：Runtime 持有长期实例凭据（例如 deferred 的 `bootstrap_credential`/refresh token/
长期委托），或缓存短期授权决策；调用 Model/Scheduler 时使用本地凭据/缓存，按周期或 TTL
边界在线 CheckAuthorization，Identity 不可达时允许缓存授权或 fail-closed（必须决策）。

- **冻结语义影响**：高。长期委托违反 ≤56s；若新增长期 credential/refresh 语义，属于新
  proto/权限面；若只缓存决策，仍需“实例主体父委托”来源（B 的前置），不能单独解 P05b。
- **实现范围**：Identity 长期实例凭据/刷新/撤销、Runtime 安全存储与轮换、Scheduler/Model
  验证兼容、缓存失效与指标、Control/运维轮换；Model/Scheduler 冻结验证可能需接受新
  credential 类型（契约变更）。
- **证书/凭据绑定**：长期凭据绑定实例 principal + Runtime 平台证书/主机；密钥存储、轮换、
  吊销列表与泄漏响应成本高。
- **scope 派生**：签发时固定/可刷新 scope；本地缓存需带 authorization_version/scope，跨实例
  请求必须拒绝；过期靠刷新/在线检查。
- **过期/撤销**：撤销传播延迟（窗口 >56s）；断网时若允许缓存则产生越权风险，若 fail-closed
  则退化为 B/在线模式，收益有限。
- **失败语义**：刷新失败 → 按策略 fail-closed（推荐）或受限离线；凭据泄漏需要快速吊销。
- **测试验收**：凭据轮换/撤销/过期/断网矩阵；P11/P10k/P13 同 B 的 e2e，加离线/衰减窗口负例。
- **风险/工作量**：风险 High（长期凭据是最敏感资产；与最小权限/短证明方向相反），工作量 L；
  不建议作为主方案，可作为 B 的**只读决策缓存优化**（不改变证明链、每请求仍有在线检查）。

### 方案对比（摘要）

| 维度 | A：平台 peer | B：Control 实例委托 | C：长凭据/缓存 |
| --- | --- | --- | --- |
| 冻结语义影响 | 中—高（caller 表） | **高（签发主体语义，需用户批准）** | 高（TTL/新 credential） |
| proto/权限变更 | 无（限定方法） | 可能有（subject/实例 binding） | 可能有（credential/refreshed） |
| 解 P13 Scheduler 生命周期 | **是（已批限定范围）** | 是（但更重） | 是（不推荐） |
| 解 P11 Model 单链路 | **否**（Chat 无实例 scope/owner） | **是**（owner=subject=实例） | 仅在有实例父委托后；否则否 |
| 逐请求在线授权 | 否（证书/allowlist） | **是**（保留） | 取决于策略（有窗口） |
| 撤销延迟 | 证书/连接级 | ≤56s + 在线检查 | 可能 >56s/需吊销列表 |
| 跨实例隔离 | owner 内 scope 校验 | subject/scope/owner 派生 | 需显式实现 |
| 实现工作量 | M | L | L |
| 风险 | 中 | 高（需安全评审） | 高 |

## 5. 推荐

**推荐 A + B 的组合**，按已批准边界拆开：

1. **立即**：按用户已批 P13 A 实现 Scheduler `ListTasks`/`CancelTask` 平台 peer 分支 +
   `SchedulerLifecycleParticipant` 生产实现，完成 P13 生产验收（Host receipt + Scheduler
   清理都 Done 才 DELETED）。这是 P13 退出的最短路径，且不依赖 P05b 决策。
2. **需用户批准后**：采用 B1（Control 为已注册实例 principal 签发短父委托，Runtime
   Exchange 到 model/scheduler），解锁 P11 单链路与 P10k 真实 Scheduler provider；B 的
   owner 语义与冻结 doc/01 P11 契约一致（owner=委托 subject=插件主体），不需要改 Model
   Chat 字段。
3. **不建议 C 主方案**；可在 B 落地后按需加“只读授权缓存”优化，但每请求在线权威与
   ≤56s 不变。

选择 B 不选 A-for-Model 的理由：Model Chat 冻结 owner=delegation subject 且无实例 scope；
平台 peer 方案要么接受 owner=Runtime 平台主体（破坏实例隔离/计费/审计），要么改 Chat
契约（更大大变更）。

## 6. 决策问题（≤5）

1. 是否批准 **Identity 委托签发语义扩展**：允许为已注册 `Plugin` principal（binding=
   RESOURCE_INSTANCE）签发 audience=runtime 的短父委托（B1 Control 管理员触发；或 B2 实例
   自签），并接受这是重大冻结语义变更/需安全评审？
2. 实例主体如何确定与约束：使用现有 Plugin principal + RESOURCE_INSTANCE binding（推荐），
   还是新增实例/租户主体？签发时必须绑定 Installation+Instance 精确 scope 与允许
   permission 白名单（task.schedule/read、model.chat、model.call.inspect）？
3. 是否接受 **方案 A 的生产实现**（仅 ListTasks/CancelTask、平台 SAN/allowlist/capability、
   owner 内 scope 校验、撤销依赖证书/配置）作为 P13 Scheduler 生命周期路径，并在其生产
   e2e 通过前保持 P13“生产未验收”状态？
4. P13 生产验收硬条件确认：BeginDelete 在真实 Host receipt 终态且 Scheduler 清理 Done 后
   才 DELETED；UNKNOWN/offline/Blocked 不完成；Restore 不重建任务/租约——是否作为
   P05b-3/P13 退出门禁？
5. P11 单链路验收是否要求真实 managed plugin + 真实 HTTP Provider fixture（非 fake port），
   并覆盖撤销/过期/跨实例拒绝；P10k 真实 Scheduler provider 是否同批解锁？

## 7. 落地任务拆分（若选推荐 A+B）与验收标准

> 所有任务在用户批准 B 的语义变更前不得实现 B 相关代码；A 可在用户已批 P13 范围内实现。
> 禁止伪造成功；contract/proto 变更必须先契约文档、后实现、再测试（按 AGENTS.md）。

### P05b-1：契约/安全决策包（无代码，用户批准门）

- 产出：Identity 实例主体委托的冻结语义说明、proto 兼容性草案（是否新增字段/RPC）、
  威胁模型（伪造 subject、跨实例、撤销延迟、证书轮换）、撤销/审计设计。
- 验收：用户对第 6 节问题 1–2 明确批准；契约 owner 审核；未批准则 B 冻结。
- 证据：本文件 + 契约 diff 评审记录（后续任务）。

### P05b-2：Identity 实例主体父委托签发（B1/B2 选定实现）

- 范围：IssueDelegation/subject 授权扩展（或新受控 RPC）、证书绑定、TTL≤56s、权限/scope
  白名单、审计事件、撤销/版本检查；不改 Model/Scheduler owner 验证。
- 验收：正/负例（非授权主体伪造 subject、错误 scope/permission、证书不匹配、TTL 超限、
  撤销后拒绝、跨实例拒绝）；双进程 mTLS/真 PG；无 secret 落日志；P04/P05 回归通过。

### P05b-3：Scheduler P13 A 生产实现与 P13 生产验收

- 范围：Scheduler platform-peer 分支（仅 ListTasks/CancelTask）+ capability/allowlist +
  task/instance 归属校验；Runtime `SchedulerLifecycleParticipant` 生产实现与 composition
  注入；移除“test-only”依赖（保留 P13g 作回归）。
- 验收（P13 生产门禁）：真实 Runtime + 真实 HostBridge/Host + 真实 Scheduler；BeginDelete →
  Host StopExecution receipt（KnownSucceeded/KnownFailed）**且** Scheduler ListTasks/CancelTask
  清理 Done → participants Done → operation Succeeded/instance DELETED；Host UNKNOWN/offline、
  Scheduler offline/denied 保持 BLOCKED；Restore 不重建任务/租约；跨实例/跨安装拒绝；
  真实 mTLS/SAN/allowlist 正负例。

### P05b-4：Runtime 实例委托 provider（Model）与 P11 单链路 e2e

- 范围：Runtime 获取/刷新实例父委托、调用 ExchangeDelegation 换取 audience=model
  hops=2、client_certificate=Runtime 叶；生产 composition 替代
  `unavailable_model_delegation()`；失败 fail-closed、不缓存 secret；按冻结 host 帧路径
  `HostRequest.model_chat` 接真实链路。
- 验收：真实 managed plugin→Bridge→Runtime→Model→真实 HTTP Provider fixture（非 fake
  port）返回结果且 call record/usage owner=插件实例；委托撤销/过期/跨实例/错误证书 →
  稳定 `UNAUTHENTICATED`/`PERMISSION_DENIED`；Runtime/Identity 不可达 fail-closed；
  Provider 4xx/429/5xx/UNKNOWN 映射保持；无长期 secret/日志泄漏；P11 报告更新证据，
  目标 target 状态下不伪造翻转。

### P05b-5：P10k 真实 Scheduler 委托解锁

- 范围：Runtime 实例父委托/Exchange 后调用 Scheduler `CreateTask/GetTask/CancelTask`；
  或按批准组合只接 Scheduler 生命周期 A、任务方法走 B；替换
  `unavailable_scheduler_task_delegation()`。
- 验收：真实 plugin Bridge task_create/get/cancel→Runtime→真实 Scheduler（真 PG/真 mTLS）
  成功链；跨实例/跨安装 NOT_FOUND；撤销/离线 fail-closed；P10k 报告删除“委托来源未确认”
  阻塞描述并保留 P10m 回归。

### P05b-6：文档、运维与退出证据

- 范围：更新 doc/01-contracts、doc/21（caller 表/实例委托语义）、doc/27 证据、P05/P10/P11/P13
  报告；配置/证书轮换/撤销运维说明；验收命令与 CTest 标签。
- 验收：文档与实现/proto 一致；门禁（147/148 相关子集）绿；P13 生产验收与 P11/P10k 证据
  可复跑；报告明确剩余环境项（真实 Provider 付费、跨主机、真实 QQ 平台等）。

## 8. 风险与回滚

- **A 的越权风险**：平台 peer 绕过逐请求在线授权；严格限定两个方法/显式 capability/scope
  归属校验；证书轮换/allowlist 撤销流程与监控必须随实现落地。
- **B 的伪造 subject 风险**：签发接口必须验证实例 principal binding、管理员授权与 scope；
  禁止任意 subject 参数化；父/子委托均短 TTL + 在线检查，撤销可即时生效。
- **回滚**：所有生产 provider 保留 fail-closed 开关（未配置即 P10k/P11 原行为）；A/B 的
  composition 注入可配置禁用；文档记录回滚步骤与观测指标（delegation issue/refresh 失败、
  provider fail-closed 计数、participants BLOCKED 时长）。

## 9. 附录：证据与当前测试基线

- P05f 报告：`p05_runtime_identity_client`、`p05_runtime_identity_e2e`（真实双进程
  mTLS/PG16.4，允许/撤销拒绝/Identity 不可达/缺配置 fail-closed）。
- P10k：`p11_model_bridge`（C++，CTest）含 task ingress→fake Scheduler port 全错误矩阵；
  `p10_sdk_tasks_e2e` 的 fake Scheduler 不代表生产 provider。
- P11：`p11_model_bridge`、`p11_control_model_rest`、SDK pytest（141 passed/23 skipped；target
  Model e2e skip 原因见 `sdk/python/tests/test_model.py:13`）。
- P13g：`p13_joint_delete_e2e`（labels `p13;test-only`）证明 test-only 生命周期；生产
  Scheduler participant 仍为 null。
- 本文只读设计：未修改 proto/权限/实现；所有“未来任务”需用户批准并按 AGENTS.md 文档顺序
  先契约后实现。
