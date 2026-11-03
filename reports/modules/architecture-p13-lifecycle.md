# P13 生命周期清理、drain 与可观测性（P13a–P13d 汇总）

日期：2026-11-02；P13d worktree `.worktrees/p13d`，分支 `codex/p13d`；P13a/P13b/P13c 已分别合入 main（见同目录各自报告）。

## 可观测性最小面（P13d，不新增 API/权限/RPC/metrics）

- **结构化日志**：Runtime dependency hook 在 lifecycle maintenance 有进展/异常时向 owner stderr 输出一行
  `operation=lifecycle_maintenance outcome=ok|degraded reconciled=N completed=N blocked=N purged=N pruned=N cleanup_failed=true|false`。
  `blocked>0` 或 cleanup prune 失败 → `outcome=degraded`；`pruned` 仅在 60 秒保留批次非零时出现。cleanup 失败非致命，不伪完成 operation、不删未决行。
- **operation/participants**：现有 `GetOperation` 快照投影 `state`、`blocked_reason`、`next_attempt`、`participants[].state/reason/next_attempt_at`；BLOCKED/UNKNOWN 保持非终态，不映射成功。
- **审计/日志查询**：begin_delete/restore 等命令继续写 owner 审计，`ListAudit` 可按 resource/actor/时间过滤，`ListLogs` 是同一审计的脱敏投影；权限仍为现有 `audit.read`/`log.read`。
- **容量/租约/积压**：使用 owner 私有表只读 SQL 核对（operations/outbox/host_commands/delivery_targets），不新增查询 RPC；指标/告警/集中日志留 P14。

## P13 退出验收矩阵（P13f 刷新，基线 main=d28020e）

决策包见 [P13f 退出决策包](architecture-p13f-exit-decision.md)；P13 退出判定：**未通过**。

| doc/23 P13 验证点 | 状态 | 证据/说明 |
| --- | --- | --- |
| 重复删除/恢复并发（重复 delete/restore、已删恢复、窗口外恢复） | 已实测（应用/单机） | `p13_runtime_lifecycle`；`p05_runtime_identity_e2e`（真 PG/Identity/Runtime Account BeginDelete/Restore） |
| 共享成员删除不误删他人数据 | **已实测** | `p06_bridge_acceptance`（P13e：真 PG16.4 storage/namespace 帧，成员撤销/namespace 删除隔离） |
| 清理批次失败/满盘 | 部分实测：cleanup fault 已测，真实满盘待 P14 环境 | `p13_runtime_lifecycle` cleanup 故障注入；真实磁盘满/只读未跑 |
| 失效 outbox 满/容量 | 部分实测：Identity/owner outbox 容量有覆盖，无四库同时打满 | `p03_postgres_database`、`p08_*`、P06/P07 e2e；全链路容量压力未做 |
| 未决副作用（UNKNOWN/EVIDENCE_ONLY、不提前清理） | 已实测 | `p13_runtime_lifecycle`、`p07_action_rules`/`p07_postgres_action_records`、`p08_host_journal`/`p08_postgres_host_registration`/`p13_host_sdk_drain` |
| 保留期与配额（30/180 天、60s 批次、period 配额） | 已实测（应用/PG）；生产规模待环境 | `p13_runtime_lifecycle`、`p03_postgres_database`、`p07`/`p09` 配额测试；长周期/生产数据量未验证 |
| Host/SDK drain 与 UNKNOWN 不伪 stopped | 已实测 | `p13_host_sdk_drain`（真实 Host+SDK SIGTERM/SIGKILL/重启/journal） |
| Host participant outbox 收敛 | 部分实测 | `p13_runtime_lifecycle` fake receipt BLOCKED→Done/UNKNOWN；`p08_host_bridge_e2e`（P13e live Stop/Drain 帧 + PG receipt）；**缺 BeginDelete→真实 Host receipt 单链** |
| Scheduler participant 真实 ListTasks/CancelTask | **部分实现（P13h-1 Scheduler 侧平台 peer 授权）** | Scheduler 已支持 Runtime 平台 peer 的受限 ListTasks/CancelTask（mTLS+SAN+部署实例 allowlist+归属校验）；Runtime 生产 participant 注入与真实 BeginDelete 联合链路仍待 P13h-3 |
| BeginDelete/Restore 与真实 Host 单链到 DELETED | **未验收** | 依赖 Scheduler participant 生产路径；P13e 记录 |
| 跨主机/真实平台故障/生产规模 | 未验收 | P14 环境项 |
| 指标/告警/集中日志 | 部分实现（最小观测行/审计查询），集中 metrics/告警未交付 | P13d/doc/08 |
| P10k task ingress + fail-closed 委托（非 P13 participant） | 已实测 fail-closed | `p10_sdk_tasks_e2e`、`p10_scheduler_rpc`、P10k 报告；真实 Runtime→Scheduler 委托仍待确认 |

## 排障入口（与 P14c runbook 交叉引用）
见 doc/08 “P13d” 节：GetOperation → participants/reason/next_attempt；audit/log 查询；`operation=lifecycle_maintenance outcome=degraded` 日志；owner 私有库积压/租约 SQL；满盘/恢复按 P14c 备份恢复 runbook 停机处置，不用生命周期快捷方式绕过业务门禁。


## P13e 补充（2026-11-02）

- 新增真 PG16.4 + 真 Runtime + fake mTLS Host 的 HostBridge live StopExecution/DrainHost 帧
  下发/heartbeat 触发/receipt 落 PG 断言（p08_host_bridge_e2e）。
- 共享 namespace/成员删除不误删断言：撤销成员后访问 DENIED 但共享值/其他成员/另一安装
  namespace 保留；删除共享 namespace 不影响私有 namespace（p06_bridge_acceptance，真 PG）。
- Scheduler participant 仍只有 fake/注入端口证据（P13a 应用测试覆盖 BLOCKED→Done、离线/
  恢复/超时），生产 Runtime 未接 ListTasks/CancelTask 客户端与委托。
- 未完成：BeginDelete/Restore 与真实 Host 的单一联合 e2e（需 Scheduler participant 生产路径或
  用户确认的测试注入方式）；P13 退出判定：**未通过**（上述两项为剩余缺口）。

## P13g test-only 联合 e2e（2026-11-03，test-only，非生产证据）

新增 `tests/architecture/p13_joint_delete_e2e.py`（CTest `p13_joint_delete_e2e`，label
`p13;test-only`）与独立测试二进制 `kiarabot-runtime-p13g`（生产 `kiarabot-runtime` 不变）：
真 PG16.4 + 真 mTLS Runtime + fake HostBridge 客户端 + 注入 fake Scheduler participant
（P13f 方案 C；生产 Scheduler 委派仍 fail-closed，待用户确认 A/B）。

矩阵（全部 test-only）：
- 实例 BeginDelete→初始 participants（runtime Succeeded、host/scheduler Accepted）→Host 待
  StopExecution receipt/Scheduler 待取消→Blocked；Host receipt Receipted + fake Scheduler
  Done→participants 全 Done→operation Succeeded、instance DELETED。
- 同幂等键重复 BeginDelete 返回同一 operation。
- Host 离线时操作保持 Blocked/DELETING，绝不到 DELETED；重连后 receipt 完成。
- Host receipt=UNKNOWN 时保持 Blocked，不伪 stopped/完成。
- fake Scheduler done/unknown/offline 矩阵：unknown/offline 保持 Blocked，切回 done 后完成。
- Restore：instance 恢复 Active；bridge_credentials 仍 revoked（PG 断言）；fake Scheduler
  任务/租约不被重建；Restore operation 只有 runtime participant；再次 BeginDelete 重新调度
  host+scheduler participants 并完成。
- 修复暴露缺陷：Host command identity 增加 instance version，避免 Restore 后再次删除复用旧
  StopExecution receipt 误判 Done（生产运行时行为修复，无 proto/权限变更）。

边界：fake Scheduler 任务的“ListTasks”仅由 fake 端口内部建模（生产 participant 端口只有
cancel_tasks；未新增方法）；任务/租约的 PG 断言在 Scheduler 私有库未包含于本 harness，当前以
fake Scheduler 状态文件断言未重建；生产 Scheduler 委托、真实 Host 回执单链仍未验收。
P13 退出判定：test-only 联合链路通过；**生产退出仍未通过**（P13f A/B 待用户确认）。
