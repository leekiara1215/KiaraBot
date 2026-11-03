# P13h-1：Scheduler ListTasks/CancelTask 受限平台 peer 授权（实现方案）

日期：2026-11-03；基线 main=`fa3030c`。范围：仅 Scheduler 对 Runtime 平台 peer 的两个只读/取消方法；
**不新增 proto/权限编号/RPC**，不改变 Control/用户 delegation 旧调用者行为。生产联合验收（BeginDelete
等真实 Host 回执 + Scheduler 清理后才 DELETED）由 P13h-3 负责，本任务不宣称完成。

## 1. 先例与约束

- P10b-2 的 Scheduler→Runtime `AcceptTimerTrigger` 是单方法平台 peer 豁免：Runtime 侧从真实 mTLS
  叶证书 URI SAN `spiffe://kiarabot/scheduler…` 认证部署注册的 Scheduler 平台身份，校验 handoff
  完整性，不要求用户 delegation；该豁免明确不扩散到其他 Runtime owner 方法。
- 现有 Scheduler 六方法 authorizer（`SchedulerIdentityClient`）要求 Runtime/用户 delegation
  （authz=user 路径）；Runtime 没有可获得的 `audience=scheduler` 实例委托（P05b/P13f 缺口）。
  因此 P13h-1 采用 P13f 方案 A 的**对称、范围收窄**实现：平台 peer 路径只对 ListTasks/CancelTask
  生效，且必须在部署配置中显式登记实例级授权；其余方法继续走原 delegation authorizer。

## 2. 授权模型（fail-closed）

Scheduler 配置新增 Scheduler-only `runtime_peers` 数组，每项：

```json
{
  "certificate_sha256": "<runtime leaf DER SHA-256 hex>",
  "instance_id": "<目标 Runtime 实例 UUID>",
  "installation_id": "<该实例所属安装 UUID>",
  "account_id": "<该实例所属账号 UUID>",
  "permissions": ["task.read", "task.schedule"]
}
```

- **mTLS 平台身份**：真实入站叶证书 SAN 必须是 `spiffe://kiarabot/runtime…`（Runtime platform
  peer），且叶证书 DER SHA-256 必须精确命中 `runtime_peers` 之一；否则不进入平台分支。
- **白名单/注册**：Identity `platform_peers` 离线 bootstrap 仍负责平台 principal/证书注册与撤销
  记录；Scheduler 部署配置的 `runtime_peers` 是显式的实例级能力白名单，二者同时满足才允许。
- **显式授权**：ListTasks 需要该项 `task.read`；CancelTask 需要 `task.schedule`。缺失即
  `PERMISSION_DENIED`，不回退到 delegation 路径（避免同一证书被宽松旧路径放大权限）。
- **scope/owner 推导**：平台路径要求请求 `ResourceScope` 恰好包含一个 `RESOURCE_INSTANCE`，其 id
  必须是该 peer 项 `instance_id`；owner/installation/account 只由配置项派生，不采用客户端自报的
  其他 owner 字段。跨实例 scope 立即拒绝（不查询、不泄露存在性）。
- **ListTasks 过滤**：应用层查询 owner=该 instance 的任务后，仅返回
  `installation == 配置 installation && target.account == 配置 account` 的行；不返回其他实例/
  安装/账号任务。
- **CancelTask 归属**：先按 `(owner=instance, task_id)` 查询（跨实例自然 NOT_FOUND，不泄露），
  再核对 `task.installation == 配置 installation && task.target.account == 配置 account`；
  不匹配返回 `NOT_FOUND`（不可区分存在性）。取消窗口/occurence 冲突仍由既有应用规则返回
  `CONFLICT`。
- **旧 caller 回归**：未命中 `runtime_peers` 的证书继续走 `options.authorize`
  （SchedulerIdentityClient + Control/用户 delegation）原逻辑；平台分支仅当授权器返回
  “已识别的 Runtime peer”时接管，返回 nullopt 表示继续旧路径。未注册/白名单外证书无
  delegation 时最终 `UNAUTHENTICATED`，有合法 delegation 的旧 Control/用户调用者不受影响。
- 其他四个 Scheduler 方法（CreateTask/GetTask/GetOccurrence/GetOperation）不进入平台分支。

## 3. 实现点

1. `SchedulerRpcOptions` 增加 `platform_peer` 授权器；`prepare` 在 ListTasks/CancelTask 先尝试，
   命中后构造 `SchedulerCommandContext{owner,installation,account,platform_peer=true}` 并要求
   方法所需权限；否则回退原 `authorize`。
2. `SchedulerCommandContext` 增加 `platform_peer` 标志与可选 account；`SchedulerService`
   ListTasks/CancelTask 对平台路径执行上述安装/账号归属过滤，旧路径语义不变。
3. Scheduler 配置解析 `runtime_peers`（严格 schema v1，仅 Scheduler 允许）；composition 用
   `extract_peer_certificate` 生成平台授权器。
4. target_spec caller/authz 注记 + doc/01 变更：仅 ListTasks/CancelTask 增加“Runtime platform
   peer（仅实例任务查询/取消；部署实例级 allowlist 显式授权，无用户 delegation）”的 caller
   语义；不新增字段/权限/RPC。doc/02/08 + P13/P10 报告记录风险与 P13h-3 验收边界。

## 4. 验证矩阵（真 PG16.4 + 真 mTLS 双进程 Runtime/Scheduler）

- 允许：Runtime 叶证书 + 配置实例 scope → ListTasks 仅返回该实例任务、CancelTask 成功；
- 跨实例 scope → PERMISSION_DENIED；跨安装任务 → NOT_FOUND；跨账号任务 → NOT_FOUND；
- 未注册证书/白名单外证书 → UNAUTHENTICATED；白名单项缺 task.read/task.schedule → PERMISSION_DENIED；
- 旧 caller：Control 的 Scheduler delegation 调用 ListTasks/CancelTask 保持成功；
- 其他方法：平台 peer 路径不得用于 CreateTask/GetTask 等，旧 delegation 行为不变。

## 5. 明确不做

- 不实现 Runtime 生产 `SchedulerLifecycleParticipant` 注入/真实 BeginDelete 联合链路（P13h-3）；
- 不新增 proto 字段/权限编号/RPC，不改变 Scheduler 任务状态机；
- 不把“真实 Runtime→Scheduler 生产联测”作为本任务结论；无真实 Host 回执前不宣称 P13 生产退出。


## 6. 实现与验证（P13h-1 交付）

- 已实现：`SchedulerRpcOptions.platform_peer` 分支（仅 ListTasks/CancelTask）；Scheduler
  配置 `runtime_peers`；`SchedulerCommandContext.platform_peer/account`；应用层 ListTasks
  instance/installation/account 过滤与 CancelTask 归属复核；旧 delegation 路径不变。
- 真 PG16.4 + 真 mTLS 双进程（`p10_timer_e2e`，labels 增加 p13）：Runtime 叶证书平台路径
  ListTasks/CancelTask 成功；跨实例 scope、缺 instance scope → PERMISSION_DENIED；跨安装/
  跨账号 task 行不出现在 list 且 cancel → NOT_FOUND；只读证书可 list、cancel → PERMISSION_DENIED；
  未注册证书/平台证书调用 CreateTask → UNAUTHENTICATED；Control delegation 旧路径继续成功。
- 残余：冻结 `CreateTask`/`CancelTask` 不携带 account，Scheduler 任务行的 target account 对
  经 RPC 创建的任务为空；P13h-1 的 account 检查只在任务行确有记录时生效，无法对无 account
  记录的 task 独立证明 account 归属。此项需契约澄清；instance/installation 归属已强制。
- 明确未做：Runtime 生产 SchedulerLifecycleParticipant/真实 BeginDelete 联合链路（P13h-3）；
  未宣称 P13 生产退出通过。
