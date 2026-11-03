# P14n 最终交付状态与证据索引

- 日期：2026-11-03；worktree `.worktrees/p14n`，branch `codex/p14n`，基线 `ce7993a`（main；P14t/P05b-13；final HEAD 门禁 161/162 + `control_http_websocket_acceptance` 负载 flake 孤立通过；exact-HEAD `04f7044` 门禁 161/162 + p08 负载 flake 孤立通过）。
- 任务：只刷新最终证据矩阵/交付口径，不修改实现、契约或部署脚本；`doc/18` 冻结基线只读。
- 结论：**P13 生产退出 PASS（本验收范围）、P11 独立验收 PASS、P10k 真实 Scheduler 任务链 PASS、P05b B1 链完成（安全评审 0 Critical/High；Medium 全部处置：F-1/F-2 已修、F-3/F-4/F-5/F-6 已加固；F-5 host/core 边界文档化）。剩余未验收收敛为 8 项环境/契约项；P12 host operation/bootstrap_credential 按用户确认保持 not_implemented。previous checkpoint 161/161@7b8fb0f/1051.70s；final exact-HEAD gate @04f7044：**161/162 CTest passed，1025.87s**（`build/logs/recheck-ctest-162320.log`，2026-11-03 16:23:19→16:40:30）；唯一失败 `p08_managed_host_e2e` 35.77s（psql SSL negotiation invalid response，套件负载下时序问题）；孤立复跑 **2/2 Passed 58.64s/56.72s**（Lead 57.39s），判定负载时序 flake，不是断言/实现回归。此前 161/161@7b8fb0f/1051.70s。 不得把本报告当作“已发布/全部验收”声明。**

## 1. 最终门禁与修复

| 项 | 结果 |
|---|---|
| full-locked build + CTest | exact-HEAD gate @04f7044：**161/162 CTest passed，1025.87s**（`build/logs/recheck-ctest-162320.log`，2026-11-03 16:23:19→16:40:30）；唯一失败 `p08_managed_host_e2e` 35.77s（psql SSL negotiation invalid response，套件负载下时序问题）；孤立复跑 **2/2 Passed 58.64s/56.72s**（Lead 57.39s），判定负载时序 flake，不是断言/实现回归。此前 161/161@7b8fb0f/1051.70s。 |
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
- **边界**：fake Scheduler 内部建模 ListTasks；生产 participant 端口只有 cancel_tasks；Scheduler 私有库任务/租约断言未在本 harness。**P13 生产退出仍未通过**（用户已批限定 A，P13h-1/2/3 实施中；B 未批准），不得把本 test-only e2e 当生产授权证据。

## 1c. P14t 通过证据（2026-11-03）

- **P13 生产退出 PASS（本验收范围）**：`p13h3_joint_delete_production_e2e`（真 PG16.4 四库、真 Identity bootstrap、真 mTLS Runtime/Scheduler、真 `kiarabot-plugin-host`+受管 SDK 插件）：BeginDelete→真实 Host receipt+Scheduler ListTasks/CancelTask→participants 全 Done→DELETED；Host UNKNOWN/离线保持 BLOCKED；Scheduler denied 保持 BLOCKED；Restore 不重建 task/凭据/租约；再次 BeginDelete 重新调度成功；跨实例任务不受影响；account 字段冻结为空时不伪造归属。残余环境：真实 Host UNKNOWN 进程注入（当前 HostBridge 测试客户端）、Scheduler 完全离线主链引用既有证据、多主机/真实满盘/生产规模。
- **P11 独立验收 PASS**：`p11h_managed_model_e2e`（真实 managed Host+SDK plugin→PluginBridge→Runtime→Model→真实 HTTP Provider 正/负矩阵）、`p11h_model_chain_fail_closed`、`p11h_provider_fixture`；相关回归 37/37（p11|p05|p08|p06|p03_postgres）。真实付费外部 Provider/目标部署环境仍环境项。
- **P10k 真实 Scheduler 任务链 PASS**：`p05b_task_chain_e2e`（真 PG16.4 四库+真 Identity/Runtime/Scheduler+managed plugin）：PluginBridge task_create/get/cancel→Runtime→真 Scheduler；owner=instance/installation、account 空、幂等 replay、取消/重复取消、COMPLETED=cancel CONFLICT、跨实例 NOT_FOUND、Scheduler 离线 fail-closed、Host/插件生命周期共存。真实 QQ/平台触发仍环境项。
- **P05b B1 链完成**：P05b-2/5/6/7/8/9/4/12；P05b-11 安全评审 **0 Critical/High**；P05b-12/13 处置全部 Medium：F-1 cache installation、F-2 sealed TOCTOU 已修；F-3 refresh token 最大年龄/重认证停止、F-4 per-key single-flight、F-5 OPENSSL_cleanse+host/core 边界文档化、F-6 expected_subject 已加固。
- **未验收 8 项**：Docker Compose 六服务栈；两主机/断网；远端 CI/release；真实 QQ/NapCat；真实付费外部 Provider；生产 CA/密钥/备份离站；部署级错误 Runtime 叶证书；Model Update/RotateProviderSecret `scope_kinds` 契约缺口。

## 2. 阶段状态（详见 [doc/27](../../doc/27-phase-exit-evidence.md)）

| 阶段 | 状态 | 证据入口 |
|---|---|---|
| P00–P03 工具链/契约/PG | 前置基线通过 | `p01_*`、`p02_core_process`、`p03_postgres_database`、`sqlite_runtime_store`（P00n 竞态修复）；contract 门禁；`doc/11` |
| P04 Identity/Control 认证 | 退出通过（本机） | `p04_identity_*`、`p04_control_auth_https`、`doc/06` |
| P05 Runtime 资源/授权链 | **B1 链完成**（安全评审 0 Critical/High；Medium 全部处置：F-1/F-2 已修、F-3/F-4/F-5/F-6 已加固；F-5 host/core 边界文档化） | `p05b_task_chain_e2e`、`p05b_model_chain_e2e`、`p05b_model_chain_fail_closed_e2e`、`p05b_provider_fixture`、`p05_runtime_identity_e2e`、[P05 报告](architecture-p05-runtime.md) |
| P06 事件/投递/SDK/Adapter | 退出通过（本机/真实进程） | `p06_bridge_acceptance`、`p06_client_e2e`、P06g `p07_adapter_e2e`、P06h UNKNOWN/EXPIRED/reconnect、[P06 报告](architecture-p06-event-delivery.md) |
| P07 Action/配额/执行 | 退出通过（P07f-2） | `p07_action_rules`、`p07_postgres_action_records`、`p07_adapter_e2e`、[P07 报告](architecture-p07-action.md) |
| P08 Host/期望状态/配置 | 退出通过（P08f） | `p08_host_*`、`p08_host_bridge_e2e`、`p13_host_sdk_drain`、[P08 报告](architecture-p08-host.md) |
| P09 Namespace/Storage | 范围完成/部分环境项 | `p09_postgres_storage_records`、`p09a_runtime_storage_application`、[P09 报告](architecture-p09-storage.md) |
| P10 Scheduler/定时交接 | 退出通过（P10m 后） | `p10_scheduler_*`、`p10_scheduler_rest`、`p10_timer_e2e`、P10m 段 |
| P11 Model 网关 | **独立验收 PASS（本地真实依赖）** | `p11_model_*`、`p11_control_model_rest`、[P11 报告](architecture-p11-model.md) |
| P12 Control/Panel | **范围完成**；host operation/bootstrap_credential 保持 not_implemented（用户确认） | `p12_control_owner_gateway`、`p12l` principals、`p12j_panel_live`、[P12l](architecture-p12l-identity-principals.md)/[P12j](architecture-p12j-panel-live.md) |
| P13 生命周期/可观测 | A 已批限范围；P13h-1/2/3 实施中；生产未退出 | `p13_runtime_lifecycle`、`p13_host_sdk_drain`、**`p13_joint_delete_e2e`**（38.59s，test-only；BeginDelete→Blocked→Done→DELETED/Restore 不复活/Host 离线/UNKNOWN Blocked；HostCommand identity 修复）、[P13f 决策](architecture-p13f-exit-decision.md) |
| P14 部署/故障/交付 | 材料/原生矩阵完成；环境未验收 | final gate、P14f/i/j/k/l/m 报告、`doc/08`、`target-smoke`/`target-drill`/`check-workflows` |

## 3. 未验收清单（P14t 收敛）

| 类别 | 状态 | 退出条件/说明 |
|---|---|---|
| 单机 Docker Compose 六服务栈 | **未验收** | Linux Docker：六服务镜像 build、compose config/up、官方 PG16.4 初始化/迁移/healthcheck、非 root/只读/秘密挂载、备份恢复 |
| 两主机/断网演练 | **延期**（用户确认可选） | 两主机拓扑、网络分区、TLS/撤销/重连、协调恢复 |
| 远端 CI / release | **未运行/未就绪** | GitHub Actions workflow_dispatch 未运行；无 release SHA 全量归档、签名镜像、SBOM/cosign、release-gated Environment |
| 真实 QQ/NapCat | 待环境 | 平台长期稳定性/网络抖动/reaction API；P10k 真实平台触发 |
| 真实付费外部 Model Provider | 待环境 | T07b 付费联调/计费核对；P11h 使用本地真实 HTTP Provider fixture，不等于生产 Provider |
| 生产 CA/密钥托管、备份离站 | 待用户环境/决策 | CA/root key 托管、轮换/吊销/KMS；备份保留周期、离站位置/频率、生产数据量恢复演练 |
| 部署级错误 Runtime 叶证书 | **未验收** | P11h forged signature/错误 origin 在线拒绝已覆盖，但 fixture 仅登记一个 Runtime peer；替代 Runtime 叶证书的部署级错误证书未验证 |
| Model Update/RotateProviderSecret `scope_kinds` 契约缺口 | **已知缺口** | proto 缺 scope_kinds，真实 Control 管理 RPC 无法经 Identity CheckAuthorization；e2e 用 test-only seed helper 绕过，不扩契约 |

## 4. 用户决策与实施状态（2026-11-03）

登记文件：[user-decisions-2026-11-03.md](../analysis/user-decisions-2026-11-03.md)。

| 项 | 决策 | 实施/验收状态 | 说明 |
|---|---|---|---|
| P13 | 限定批准 A（Scheduler mTLS+白名单+显式授权，仅 ListTasks/CancelTask；instance/task 归属；真实 Host 回执+Scheduler cleanup→DELETED，UNKNOWN/离线 BLOCKED，Restore 不重建任务/租约） | **生产退出 PASS（本验收范围）**：`p13h3_joint_delete_production_e2e`；P13h-1/2 证据已合入 | 残余多主机/满盘/生产规模、真实 Host UNKNOWN 进程注入属环境项 |
| P05b / P11 / P10k | 批准 B1；P11 验收真实 managed plugin+真实 HTTP Provider fixture+负例；P10k 同批解锁 | **B1 链完成；P11 独立验收 PASS（37/37 相关回归）；P10k 真实 Scheduler 任务链 PASS**；安全评审 0 Critical/High，Medium 全部处置（F-1/F-2 已修，F-3/F-4/F-5/F-6 已加固；F-5 host/core 边界文档化） | 真实付费 Provider、目标部署环境、部署级错误 Runtime 叶证书、Model Update/RotateProviderSecret scope_kinds 仍为环境/契约项 |
| P12 | host operation/bootstrap_credential 保持 not_implemented（用户确认），只文档化、不扩契约 | 无新增实现任务；按 not_implemented | 不是完成 |
| P14 | 用户准备 Docker 与 gh 登录，之后继续 Compose/CI/release | 单机 Docker Compose、远端 CI、正式发布仍未运行；previous checkpoint @7b8fb0f 161/161/1051.70s；final @04f7044 161/162/1025.87s，p08 负载 flake 孤立 2/2 通过 | 不以文档替代验收 |

## 5. 一致性核对

- `doc/18-architecture-baseline.md` 未修改（`git diff main -- doc/18...` 为空；最后修改为历史 `f8388f0`）。
- 交付文档相对链接：0 missing（README/doc/23/27/28/08/本报告）。
- 脚本/命令路径：`build/env/recheck-full-locked.sh`、`scripts/deploy/target-smoke.sh`、`scripts/ops/tests/ops_scripts_test.sh`、`scripts/ci/check-workflows.sh`、`scripts/deploy/tests/target_rotate_test.sh` 等在 P14e 已实跑；本任务只复核路径。
- 秘密扫描：tracked 文件无真实私钥/云 token；仅 identity_crypto 的 PEM 头字符串常量。
- 口径检查：doc/27/28/README/doc/23 基线统一 `04f7044`；final 161/162@04f7044（p08 负载 flake 孤立通过）已回填；P00n、P06h、P10m、P13 生产退出 PASS、P11 独立验收 PASS、P10k 真实链 PASS、P05b B1 完成、P12 not_implemented、P14 环境项在各文档一致；此前 gate 148/148@1213790 与当前 checkpoint 161/161@7b8fb0f 均按实际日志记录。

## 6. 未做/限制

- 本任务只改文档，无 build/test 重跑（final gate 证据由 Lead 提供；本地未重跑 148 项）；文档校验为链接/路径/秘密/一致性静态检查。P13g 证据来自 `p13_joint_delete_e2e` test-only 标签与 P13g 报告，不把它当生产退出证据。
- 未执行 Docker/两主机/远端 CI/release，未接真实 QQ/Provider；不能以本报告替代这些环境验收。
