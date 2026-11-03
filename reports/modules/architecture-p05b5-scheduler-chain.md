# P05b-5：P10k 真实 Scheduler 委托链（2026-11-03）

- 基线：main=`241f983`（P05b-7 已合入）；worktree `.worktrees/p05b5`。
- 结论：**Runtime provider/Exchange→真实 Scheduler Create/Get/Cancel 正向链已实测通过**；SDK
  PluginBridge ingress 的完整单进程链路仍待接 Host harness（见 §4）。未改 proto/RPC/权限。

## 1. 实现/修复

1. `RuntimeIdentityClient::exchange_delegation`：parent proof→Identity Exchange audience=scheduler
   hops2；补 `RequestContext.idempotency_key`（此前 C++ 缺 key 导致 UNAUTHENTICATED）。
2. `SealedInstanceDelegationSource`：按 P05b-6 envelope（0600/原子/instance+installation/
   audience=runtime/permissions/scope/≤56s/JWT iat-exp 一致）读取；provider 只缓存子证明到期。
3. Scheduler 侧 B1 接线修复：
   - `SchedulerIdentityClient` 从 delegation 的 `RESOURCE_INSTANCE` scope 派生 owner，而不是
     plugin principal subject；INSTANCE scope 缺失时保留 subject 回退（legacy）。
   - `prepare_platform_peer` 对**携带 delegation 的请求**返回 nullopt，交给正常 delegation
     校验；避免 Runtime platform-peer allowlist 吞掉 P10k 插件 task 请求（P13h 生命周期路径无
     delegation，保持 platform-peer）。
   - CheckAuthorization 的 fallback scope 使用派生 instance 资源 id（此前误用 subject）。
   以上不改变 P13h lifecycle 语义；P13 rank/platform-peer 测试继续通过。

## 2. 真实验证（真 PG16.4 + 真 mTLS Identity/Scheduler/Runtime）

`p10_timer_e2e`（p10/p05/p13 标签）扩展块使用 P05b-6 refresh tool 生成的 task sealed 文件，
经 Runtime provider/Exchange（Runtime 叶）→**真实 Scheduler RPC**：

| 场景 | 结果 |
|---|---|
| sealed 文件（admin token→Identity IssueDelegation） | 成功 |
| Runtime C++ Exchange（parent→audience=scheduler hops2） | 成功（同 token 与 Python 请求 wire 对齐，idempotency 修复） |
| Scheduler CreateTask | 成功；任务行 owner=实例资源 UUID |
| Scheduler GetTask | 成功 |
| Scheduler CancelTask（取消窗口内 pending） | 成功，任务行 Cancelled |
| 同 idempotency key/content replay | 成功且 Scheduler 任务行仍为 1（不重复创建） |
| 跨实例 sealed 文件 | fail-closed（reader instance 不匹配，无 Scheduler 任务） |
| Scheduler 离线（错误 endpoint） | fail-closed（不产生任务） |
| 编译/回归 | 全量构建；`ctest -R 'p10|p05|p06|p03_postgres'` 通过；P10m shutdown 回归保留 |

撤销/过期/错误证书：P05b-7 的 `p05_runtime_identity_e2e` 已覆盖撤销后 Exchange/Check 拒绝、
过期 child 拒绝、错误证书；sealed reader 单测覆盖过期/篡改/TTL/权限/实例不匹配。本次 e2e 未重复
销毁管理员 assignment（p10_timer 共享 fixture 顺序限制），以这些专项矩阵为准。

## 3. 未完成

- **真实 SDK plugin Bridge→Runtime→Scheduler 单链路**尚未在新 harness 合并运行：当前 e2e 直接
  构造 Runtime provider/client（与生产 Runtime 相同组件），P10k PluginBridge ingress 仍由
  `p10_sdk_tasks_e2e`（fake Scheduler owner）覆盖。两者组合需要在 Host/P08e harness 中新增
  Scheduler + sealed 文件 + plugin task 调用，属后续 P10k 单链路验收。
- COMPLETED=已入队 在既有 P10 timer e2e 覆盖，但未通过本 sealed delegation 任务路径重跑。
- 真实付费 Provider/跨主机等环境项不受影响。
