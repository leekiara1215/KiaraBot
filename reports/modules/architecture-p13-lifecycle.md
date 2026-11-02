# P13 生命周期清理、drain 与可观测性（P13a–P13d 汇总）

日期：2026-11-02；P13d worktree `.worktrees/p13d`，分支 `codex/p13d`；P13a/P13b/P13c 已分别合入 main（见同目录各自报告）。

## 可观测性最小面（P13d，不新增 API/权限/RPC/metrics）

- **结构化日志**：Runtime dependency hook 在 lifecycle maintenance 有进展/异常时向 owner stderr 输出一行
  `operation=lifecycle_maintenance outcome=ok|degraded reconciled=N completed=N blocked=N purged=N pruned=N cleanup_failed=true|false`。
  `blocked>0` 或 cleanup prune 失败 → `outcome=degraded`；`pruned` 仅在 60 秒保留批次非零时出现。cleanup 失败非致命，不伪完成 operation、不删未决行。
- **operation/participants**：现有 `GetOperation` 快照投影 `state`、`blocked_reason`、`next_attempt`、`participants[].state/reason/next_attempt_at`；BLOCKED/UNKNOWN 保持非终态，不映射成功。
- **审计/日志查询**：begin_delete/restore 等命令继续写 owner 审计，`ListAudit` 可按 resource/actor/时间过滤，`ListLogs` 是同一审计的脱敏投影；权限仍为现有 `audit.read`/`log.read`。
- **容量/租约/积压**：使用 owner 私有表只读 SQL 核对（operations/outbox/host_commands/delivery_targets），不新增查询 RPC；指标/告警/集中日志留 P14。

## P13 退出验收矩阵（doc/23 P13 验证/退出点）

| 验证点 | 状态 | 证据/说明 |
| --- | --- | --- |
| 重复删除/恢复并发（同资源重复 delete、重复 restore、已删恢复、窗口外恢复） | **已实测（应用/单机）** | `p13_runtime_lifecycle`：duplicate begin_delete=CONFLICT、restore 复活版本、non-deleted restore=CONFLICT、30 天外恢复=CONFLICT 且状态不变；`p03_postgres_database` 覆盖 owner UoW/PG 事务 |
| 共享成员删除不误删他人数据 | **待补（代码路径保证，未直接测试）** | P13a 删除只处理 owner lifecycle 记录/tombstone，不触碰 storage 表；尚无“删除实例后共享 namespace 其他成员数据保留”的直接集成断言，列入后续 P09/P13 交叉测试 |
| 清理批次失败/满盘 | **部分实测**：cleanup prune 故障注入 **已实测**；真实满盘 **待环境** | `p13_runtime_lifecycle` 新增 fail_cleanup 故障 prune：`cleanup_failed=true`、结构化行 `outcome=degraded cleanup_failed=true`、未决记录不删；真实磁盘满/只读由 P14c 演练环境注入 |
| 失效 outbox 满/容量 | **部分实测** | `p03_postgres_database` 覆盖 identity outbox 容量与 owner journal capacity、Host command pending/prune；HostBridge apply/receipt/observation 容量与 P08 真实 e2e 见 `p08_*`；Adapter outbox 容量由 P06/P07 e2e/证据覆盖。未做全链路同时打满四库的容量压力 |
| 未决副作用（UNKNOWN/EVIDENCE_ONLY、清理不提前删未决） | **已实测** | P13a 清理保留 unresolved receipts/inbox/outbox/operations；P07 Action UNKNOWN/EXPIRED/EVIDENCE_ONLY 由 `p07_action_rules`、`p07_postgres_action_records` 覆盖；Host UNKNOWN 由 `p08_host_journal`、`p08_postgres_host_registration`、`p13_host_sdk_drain` 覆盖 |
| 保留期与配额（30 天恢复、180 天墓碑/审计、60 秒批次、period 配额） | **已实测（应用/单机）** | `p13_runtime_lifecycle` 覆盖 30/180 天、purge、60 秒 prune 计数；`p03_postgres_database`/`p13` 单测覆盖 tombstone/retention 与 Action 配额；长时间保留与生产数据量 **待环境** |
| Host/SDK drain 与 UNKNOWN 不伪 stopped | **已实测** | `p13_host_sdk_drain`：真实 Host+SDK SIGTERM/SIGKILL/重启、journal integrity、UNKNOWN 不伪成功 |
| Host participant outbox 收敛（P13c） | **已实测（fake receipt 应用层）**；真实 HostBridge 收敛依赖 P08b-3b 链路 | `p13_runtime_lifecycle` 真实 participant（fake outbox receipt）BLOCKED→Done、UNKNOWN/拒绝保持 BLOCKED；P08b-3b dispatch/receipt 已有真实 e2e（p08），但 P13 删除→真实 Host 回执的联合 e2e 未跑（**待环境/后续集成**） |
| Scheduler participant 真实 ListTasks/CancelTask | **未实测（BLOCKED 语义已测）** | 应用/契约 BLOCKED 不伪完成已测；Runtime→Scheduler 实例委托配置（P10b-2）未接线，真实取消 **待环境/后续 P10b** |
| 跨主机/真实平台故障与规模 | **待环境** | 需 P14 两主机/真实网络分区/生产规模 PG；本机无 Docker/多主机 |
| 指标/告警/集中日志 | **未交付** | 用户决定不新增 metrics RPC；现有 stderr 结构化行 + owner 审计可用于日志采集，但集中收集/告警阈值留 P14 |

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
