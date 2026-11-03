# P05b-5：P10k 真实 Scheduler 委托链联测记录（2026-11-03）

- 基线：main=`9311d2c`（P05b-6 已合入）；worktree `.worktrees/p05b5`；本报告记录 Runtime reader
  与 P05b-6 规范对齐及真实链联测结果。**未宣称 P10k 真实 Scheduler 成功链完成。**
- 非目标：不修改 Model 侧 sealed/generalization（P05b-7 由 ref-infra 负责），不扩 proto/RPC。

## 1. 已完成

1. `SealedInstanceDelegationSource` 按 P05b-6 冻结 envelope 对齐：`schema_version=1` 精确字段集，
   `instance_id/installation_id/audience=runtime/permissions/scope/issued_at/expires_at/
   ttl_seconds/delegation_proof`；proof 必须三段 JWT 且内层 `iat/exp` 与 envelope 一致；
   0600 regular 非符号链接、owner=euid、64KiB 上限；跨实例/跨安装/权限/audience/TTL/过期/
   畸形 JSON/权限位/symlink 全部 fail-closed。
2. `RuntimeIdentityClient::exchange_delegation` + `RuntimeInstanceDelegationProvider`：
   parent token 经 Identity Exchange 换 scheduler hops2 子证明，绑定 Runtime mTLS 叶；子证明缓存
   不超过其到期，父 token 不缓存/不延长；provider 只在配置 sealed 文件时接入 P10k task client。
3. 单测：`p05_sealed_delegation_source`（P05b-6 schema 正负例）、
   `p05_instance_delegation_provider`（source 缺失/不可用/空 parent/Identity 拒绝/过期子证明/
   跨实例/坏证书）；全量构建通过，`ctest -R 'p10|p05|p06|p03_postgres'` 19/19。

## 2. 真实链联测结果（阻塞）

使用 P05b-6 的 `scripts/deploy/instance-delegation-refresh` + 真 PG16.4/真 Identity/真 Scheduler
（p10_timer harness 扩展，未提交失败测试代码）：

| 步骤 | 结果 |
|---|---|
| admin token + Control mTLS -> Identity IssueDelegation(instance_id) -> sealed 文件 | **成功**（P05b-6 e2e 已覆盖） |
| Python Runtime 证书 ExchangDelegation(parent, audience=scheduler, instance+installation scope, task.schedule) | **成功**，子证明 subject=实例、aud=scheduler、hops=2、actor_chain=[Control,Runtime] |
| Python 直接 Identity `CheckAuthorization(subject_proof=child, method=SchedulerService/CreateTask, scope=INSTANCE)` | **UNAUTHENTICATED** |
| Python Scheduler `CreateTask`（带同一 child proof） | **PERMISSION_DENIED**（另一次运行出现 TIMEOUT） |
| Runtime C++ provider Exchange（同一 sealed 文件/Runtime 叶） | 初次 **UNAUTHENTICATED**；根因为 C++ 请求缺少 `RequestContext.idempotency_key`（Identity 实际要求非空），补 key 后 C++ Exchange **成功**（本地 diagnostic，不提交失败断言） |

结论：
1. **C++ Exchange UNAUTHENTICATED 是 client bug**：`RuntimeIdentityClient::exchange_delegation`
   未设置 `RequestContext.idempotency_key`；Identity 的 Exchange 实际要求该字段非空（尽管 proto
   `requires_idempotency=false`）。已修复并补单测，C++ Exchange 与 Python 一样成功。
2. 剩余阻塞是 **Identity 对 Exchange 产生的实例主体子委托的 `CheckAuthorization` 语义**：
   `CheckAuthorization(subject_proof=child, method=SchedulerService/CreateTask, scope=INSTANCE)`
   稳定 UNAUTHENTICATED，Scheduler CreateTask 因此 PERMISSION_DENIED；属 P05b-2 遗留，已并入
   P05b-7（task-150）修复范围。

## 3. 未完成/下一步

- 已修复并提交 C++ Exchange 的 idempotency-key client bug；Exchange 正向可复现成功。
- 待 P05b-7（task-150）修复 child `CheckAuthorization` 后，再跑真 SDK PluginBridge→Runtime→
  真实 Scheduler 成功链与撤销/过期/错误证书/离线矩阵。
- 本 worktree 未提交任何“正向成功”测试/断言；negative/fail-closed 与 reader 对齐保留。
- P05b-7/model 泛化不在本任务范围。
