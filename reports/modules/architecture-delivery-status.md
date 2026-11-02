# P14n 最终交付状态与证据索引

- 日期：2026-11-03；worktree `.worktrees/p14n`，branch `codex/p14n`，基线 `a1a33d1`（main；P14o 刷新）。
- 任务：只刷新最终证据矩阵/交付口径，不修改实现、契约或部署脚本；`doc/18` 冻结基线只读。
- 结论：**实现与测试证据已收口到可交付审查状态；单机 Docker Compose、两主机、远端 CI、正式发布仍属未验收环境项；P13 生产未退出（test-only 证据已具备）、P05b/P12 host operation/bootstrap_credential 待用户决策。不得把本报告当作“已发布/全部验收”声明。**

## 1. 最终门禁与修复

| 项 | 结果 |
|---|---|
| full-locked build + CTest | **148/148 passed @a1a33d1**（`build/env/recheck-full-locked.sh`，日志 `build/logs/recheck-ctest-055853.log`，2026-11-03；708.53s；新增 `p13_joint_delete_e2e` 38.59s）。首跑 `sqlite_runtime_store` flake 已由 P00n 根因修复（SQLite 双连接写锁竞争；final gate `sqlite_runtime_store` 0.27s；此前孤立 14/14、全量重跑 148/148） |
| P10m 优雅停机 SIGSEGV | **已修复**：OpenSSL 3 `OPENSSL_cleanup` atexit 与 gRPC worker 线程 TLS 析构竞态；`run_service` 使用 `OPENSSL_INIT_NO_ATEXIT`。修复前受控诊断 4 次循环 1 次 exit -11；修复后新增 10 次“8192 filler→SIGTERM→Runtime 重启”循环，连续 5 次完整 e2e（50 次停机）exit 0，另 12 次完整 e2e 无崩溃。提交 `6bff2c9`/`532b1d3`/`e83a988` |
| 原生单机栈（P14f/i/j/k/l/m） | 真实 Identity/Runtime/Scheduler/Model/Control 五进程 + owned PG16.4 + 临时 PKI + Control HTTPS；最终 **success 9 + stable-ok 1 + known-gap 0**（health/principals/logs/audit/tasks_list/models/action_query/delete/restore success；action_policy GET stable-ok 404；无 known-gap） |
| Panel API client live（P12j） | 严格模式 `KIARABOT_P12J_EXPECT_FIXED=1` PASS：受权 `/api/v1/health` 200 HEALTHY 四 owner；login/refresh/401/400；其余 owner 按当时 known-gap/capability-gap 口径记录，最终由 P14 原生矩阵收口 |

## 1a. P00n SQLite 竞态修复与 P06h 真实链补齐（2026-11-03）

- **P00n**：两个 RuntimeTransaction 在独立 SQLite 连接上读同一 revision 后竞争 UPDATE；无 busy timeout 时双方都拿到 `SQLITE_BUSY`，测试观察到两个 Conflict。修复为对 peer writer 最多等待 50ms（WHERE revision 仍决定唯一 winner）+ 显式 ready latch；final gate `sqlite_runtime_store` Passed 0.27s，首跑竞态 flake 根因消除。
- **P06h**：`p07_adapter_e2e`（真实 adapter/fake OneBot/Runtime/真 PG）扩展 UNKNOWN/EXPIRED 迟到证据与 adapter 重连：fake OneBot 延迟 + PG deadline 过期→EXPIRED，迟到 ActionResult 只写 evidence；SIGKILL adapter 后同 OUTBOX_PATH recover 生成 UNKNOWN evidence 补报；重复平台帧不新增事件、乱序两事件不重复 target。final gate `p07_adapter_e2e` Passed 29.19s。真实 QQ/NapCat 与生产 adapter 重启窗口仍待环境。

## 1b. P13g test-only 联合生命周期 e2e（非生产退出证据）

- CTest：`p13_joint_delete_e2e`（label `p13;test-only`，38.59s；独立测试二进制 `kiarabot-runtime-p13g`，生产 `kiarabot-runtime` 不含注入）。
- 真实 PG16.4 + 真 mTLS Runtime + fake HostBridge 客户端 + 注入 fake Scheduler participant（P13f 方案 C）。
- 覆盖：BeginDelete→participants（runtime Succeeded/host+scheduler Accepted）→Host stop receipt/Scheduler done→Blocked→Done→operation Succeeded/instance DELETED；重复 BeginDelete 同幂等键返回同 operation；Host 离线保持 Blocked/DELETING 不伪造 DELETED，重连后完成；Host receipt=UNKNOWN 保持 Blocked；fake Scheduler unknown/offline 保持 Blocked、done 后完成；Restore 恢复 Active、`bridge_credentials` 仍 revoked（PG 断言）、fake Scheduler 任务/租约不重建、Restore operation 只有 runtime participant、再次 BeginDelete 重新调度并完成。
- 暴露并修复：HostCommand identity 增加 instance version，避免 Restore 后再次删除复用旧 StopExecution receipt 误判 Done（生产运行时行为修复，无 proto/权限变更）。
- **边界**：fake Scheduler 内部建模 ListTasks；生产 participant 端口只有 cancel_tasks；Scheduler 私有库任务/租约断言未在本 harness。**P13 生产退出仍未通过**（P13f A/B 待用户决策），不得把本 test-only e2e 当生产授权证据。

## 2. 阶段状态（详见 [doc/27](../../doc/27-phase-exit-evidence.md)）

| 阶段 | 状态 | 证据入口 |
|---|---|---|
| P00–P03 工具链/契约/PG | 前置基线通过 | `p01_*`、`p02_core_process`、`p03_postgres_database`、`sqlite_runtime_store`（P00n 竞态修复）；contract 门禁；`doc/11` |
| P04 Identity/Control 认证 | 退出通过（本机） | `p04_identity_*`、`p04_control_auth_https`、`doc/06` |
| P05 Runtime 资源/授权链 | 范围完成；P05b 待决策 | `p05_runtime_*`、`p05_runtime_identity_e2e`、[P05 报告](architecture-p05-runtime.md) |
| P06 事件/投递/SDK/Adapter | 退出通过（本机/真实进程） | `p06_bridge_acceptance`、`p06_client_e2e`、P06g `p07_adapter_e2e`、P06h UNKNOWN/EXPIRED/reconnect、[P06 报告](architecture-p06-event-delivery.md) |
| P07 Action/配额/执行 | 退出通过（P07f-2） | `p07_action_rules`、`p07_postgres_action_records`、`p07_adapter_e2e`、[P07 报告](architecture-p07-action.md) |
| P08 Host/期望状态/配置 | 退出通过（P08f） | `p08_host_*`、`p08_host_bridge_e2e`、`p13_host_sdk_drain`、[P08 报告](architecture-p08-host.md) |
| P09 Namespace/Storage | 范围完成/部分环境项 | `p09_postgres_storage_records`、`p09a_runtime_storage_application`、[P09 报告](architecture-p09-storage.md) |
| P10 Scheduler/定时交接 | 退出通过（P10m 后） | `p10_scheduler_*`、`p10_scheduler_rest`、`p10_timer_e2e`、P10m 段 |
| P11 Model 网关 | 分层证据退出（单链待 P05b 决策） | `p11_model_*`、`p11_control_model_rest`、[P11 报告](architecture-p11-model.md) |
| P12 Control/Panel | **范围完成** | `p12_control_owner_gateway`、`p12l` principals、`p12j_panel_live`、[P12l](architecture-p12l-identity-principals.md)/[P12j](architecture-p12j-panel-live.md) |
| P13 生命周期/可观测 | **test-only 证据已具备；生产未退出** | `p13_runtime_lifecycle`、`p13_host_sdk_drain`、**`p13_joint_delete_e2e`**（38.59s，test-only；BeginDelete→Blocked→Done→DELETED/Restore 不复活/Host 离线/UNKNOWN Blocked；HostCommand identity 修复）、[P13f 决策](architecture-p13f-exit-decision.md) |
| P14 部署/故障/交付 | 材料/原生矩阵完成；环境未验收 | final gate、P14f/i/j/k/l/m 报告、`doc/08`、`target-smoke`/`target-drill`/`check-workflows` |

## 3. 未验收清单（不得混写）

| 类别 | 状态 | 退出条件 |
|---|---|---|
| 单机 Docker Compose | **未验收** | Linux+Docker：镜像 build、`docker compose config/up`、官方 PG16.4 初始化/迁移/healthcheck、非 root/只读/秘密挂载、备份恢复 |
| 两主机/跨主机演练 | **延期**（用户确认可选） | 两主机拓扑、网络分区、TLS/撤销/重连、协调恢复 |
| 远端 CI | **未运行** | `gh` 未认证、GitHub Actions 未触发；需 workflow_dispatch full-build 绿后设 `CI_AUTO_ENABLED` |
| 正式发布 | **未就绪** | release SHA 全量归档、GHCR 推送/签名、SBOM/cosign、release-gated Environment/变量/审批 |
| 真实 QQ/NapCat | 待环境 | 平台长期稳定性/网络抖动/reaction API；fixture 只证明协议链 |
| 真实外部 Model Provider | 待环境 | T07b 付费联调/计费核对 |
| 生产 CA/密钥托管 | 待用户决策 | CA/root key 托管、轮换/吊销、KMS/离线/分片 |
| 备份保留/离站 | 待用户决策 | 保留周期、离站位置/频率、生产数据量恢复演练 |

## 4. 待用户决策

1. **P13 生产授权（A/B）**：A 平台 peer 豁免扩展到 Scheduler ListTasks/CancelTask（需确认；严格 owner/instance scope）；B 不推荐；C test-only 联合 e2e 已由 P13g 提供（`p13_joint_delete_e2e`），但**不等于生产退出**，P13 仍不得标生产通过。
2. **P05b 委托语义**：Runtime 以何种 parent proof/Exchange 获取 audience=model/下游实例委托；生产 delegation provider 当前 fail-closed，决定 P11 单链与 P10 真实委托。
3. **P12 host operation/bootstrap_credential**：Host 未知 operation 错误码契约（当前 INVALID_ARGUMENT vs 契约 CAPABILITY_NOT_SUPPORTED）与 `bootstrap_credential` 字段级 target/消费闭环；是否作为发布前门槛。

## 5. 一致性核对

- `doc/18-architecture-baseline.md` 未修改（`git diff main -- doc/18...` 为空；最后修改为历史 `f8388f0`）。
- 交付文档相对链接：0 missing（README/doc/23/27/28/08/本报告）。
- 脚本/命令路径：`build/env/recheck-full-locked.sh`、`scripts/deploy/target-smoke.sh`、`scripts/ops/tests/ops_scripts_test.sh`、`scripts/ci/check-workflows.sh`、`scripts/deploy/tests/target_rotate_test.sh` 等在 P14e 已实跑；本任务只复核路径。
- 秘密扫描：tracked 文件无真实私钥/云 token；仅 identity_crypto 的 PEM 头字符串常量。
- 口径检查：doc/27/28/README/doc/23 基线统一 `a1a33d1`；final gate 统一 148/148；P00n、P06h、P10m、P13g test-only、原生矩阵、P13 生产未退出、P05b/P12 待决策、Docker/跨主机/远端 CI/发布环境项在各文档一致。

## 6. 未做/限制

- 本任务只改文档，无 build/test 重跑（final gate 证据由 Lead 提供；本地未重跑 148 项）；文档校验为链接/路径/秘密/一致性静态检查。P13g 证据来自 `p13_joint_delete_e2e` test-only 标签与 P13g 报告，不把它当生产退出证据。
- 未执行 Docker/两主机/远端 CI/release，未接真实 QQ/Provider；不能以本报告替代这些环境验收。
