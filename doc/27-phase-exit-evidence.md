# KiaraBot P04–P14 阶段退出证据矩阵（P14t 最终刷新）

- 版本：v1.3（2026-11-03，P14t 最终刷新）
- 基线提交：`7b8fb0f`（main；P14t 最终刷新；final gate 见下）
- 用途：发布前统一回答“哪些已经实现、哪些已实测、哪些仍待验收/缺口/用户确认”，**不替代**各阶段原始报告与测试记录，也不把提交/文档存在当作阶段完成。
- 相关文档：[发布清单](28-p14d-release-checklist.md)、[实施计划](23-implementation-plan.md)、[运维 runbook](08-operations.md)、[CI/CD](12-ci-cd.md)、[契约状态](01-contracts.md)、[最终交付状态](../reports/modules/architecture-delivery-status.md)。
- 最终门禁：此前 exact-HEAD gate **148/148@1213790**（`build/logs/recheck-ctest-064402.log`，687.02s；首跑 146/148 两项负载时序 flake 孤立 5/5、3/3 通过；`a1a33d1` 此前干净门禁 148/148/708.53s）。**P14t checkpoint `7b8fb0f` 由 env 运行中（日志 `build/logs/recheck-ctest-155131.log`，已注册 161 tests）；完成后回填准确 HEAD/总数/耗时/失败/孤立复验，本文件暂不写新数字。** P00n SQLite 写锁竞态已修（final gate sqlite 0.27s）；正式发布仍须在 release SHA 全量重跑并归档。
- 原生单机矩阵（P14m 后）：**success 9 + stable-ok 1 + known-gap 0**；`p14_native_single_host`/`p12j_panel_live` 在真实五进程 + PG16.4 + mTLS 栈上通过。

## 1. 状态定义

| 状态 | 含义 |
|---|---|
| 退出通过/范围完成 | 阶段验收项已有真实依赖或明确 loopback 证据，证据测试与报告可复现；残余环境/后续项已列明且不阻塞本阶段目标 |
| 已实现 | 代码/契约/文档已合入 main；未配置依赖时按契约显式失败，无 stub/假成功 |
| 已实测 | 在真实依赖（PG16.4/SQLite/TLS/进程等）或明确 loopback 假服务上跑过，有命令/测试名/日期/HEAD |
| 待验收 | 实现存在，但验收环境/链路（Docker/跨主机/远端/真实平台/Provider）未满足 |
| 已知缺口 | 明确未实现或仅部分实现，发布说明必须保留 |
| 待用户决策 | 需要用户/产品拍板的语义、策略、范围或环境项 |

`已实现` 不代表 `已实测`；`已实测（本机）` 不代表单主机 Compose、跨主机、远端 CI 或正式发布已验证。

## 2. P04–P14 矩阵

| 阶段 | 状态 | 关键已实测证据（命令/测试/HEAD） | 待验收/已知缺口 | 待用户决策/环境 |
|---|---|---|---|---|
| P04 Identity/Control 认证 | 退出通过（本机） | `ctest -R 'p04_'`；`p04_identity_application`、`p04_identity_rpc`、`p04_control_auth_https`、`p04_identity_daemon_tls_expiry`；P04f/P05f 历史证据后保持绿；final gate 148/148@1213790 | 生产 CA/密钥托管、真实 QQ 登录态复验属环境项 | CA/secret manager、生产证书轮换 |
| P05 Runtime 资源/授权链 | **B1 链完成**（P05b-2/5/6/7/8/9/4/12）；安全评审 0 Critical/High，F-1/F-2 已修、F-3 加固中 | `p05b_task_chain_e2e`、`p05b_model_chain_e2e`、`p05b_model_chain_fail_closed_e2e`、`p05b_provider_fixture`、`p05_runtime_identity_e2e`；`reports/modules/architecture-p05b*` | 真实多实例/多安装规模压测；F-3/F-4/F-5/F-6 加固（P05b-13 in_progress）；生产部署环境 | **P05b B1 已批准并完成本机链**；P11 真实 Provider 验收范围已批；跨主机/生产 CA/密钥 |
| P06 事件/投递/SDK/Adapter | 退出通过（本机/真实进程） | `p06_bridge_acceptance`、`p06_client_e2e`、`p06_postgres_runtime_events`、`p07_adapter_e2e` P06g 全总线（真实 adapter+Runtime+受管 SDK plugin） | 真实 QQ/NapCat 长期稳定性/网络抖动；真实平台链路人工验收 | QQ/NapCat 环境；P05b 相关下游委托 |
| P07 Action/配额/执行 | **退出通过（P07f-2）** | `p07_action_rules`、`p07_postgres_action_records`、`p07_adapter_e2e`、`p06_bridge_acceptance`、`p12_control_owner_gateway`；SDK action pytest | 真实平台 reaction API、Control `listActionMetadata` REST 未接线（环境/后续能力，不阻塞 target 契约退出） | 真实平台动作（Mute/Kick/撤回/审批）环境项 |
| P08 Host/期望状态/配置 | **退出通过（P08f）** | `p08_host_*`、`p08_postgres_host_registration`、`p08_host_bridge_e2e`、`p13_host_sdk_drain`；真实 TLS HostBridge | 真实受管进程/Docker Compose 生命周期；Host/插件 ACK 与 Runtime/Control 三类事实的 Compose 端到端 | Docker Compose 环境；跨主机 |
| P09 Namespace/Storage | 范围完成（本机），部分待验收 | `p09_postgres_storage_records`、`p09a_runtime_storage_application`、`p06_bridge_acceptance` 共享成员/namespace 删除隔离 | Control/Panel 共享 namespace 管理入口 live；P09b/C 分批范围 | 真实 Control/Panel live、生产规模 |
| P10 Scheduler/定时交接 | **退出通过（P10m）；P10k 真实 Scheduler 任务链通过** | `p10_postgres_scheduler`、`p10_scheduler_rpc`、`p10_scheduler_rest`、`p10_sdk_tasks_e2e`、`p10_timer_e2e`、**`p05b_task_chain_e2e`**（真实 managed SDK plugin Bridge task_create/get/cancel→Runtime→真 Scheduler；owner=instance/installation、幂等/取消/COMPLETED、跨实例 NOT_FOUND、离线 fail-closed）；P10m 修复 | 真实 7 天 EXPIRED/多 installation 容量；真实 QQ/平台触发；生产规模 | 真实时钟/时区/平台、跨主机；真实 QQ 环境 |
| P11 Model 网关 | **独立验收通过（本地真实依赖）** | **P11h**：`p11h_managed_model_e2e`（真实 managed Host+SDK plugin→PluginBridge→Runtime→Model→真实 HTTP Provider 正/负矩阵）、`p11h_model_chain_fail_closed`、`p11h_provider_fixture`；相关回归 **37/37 passed**（p11|p05|p08|p06|p03_postgres）；p11 9/9、p03 真 PG 2/2、contract gate PASS | 真实付费外部 Provider（T07b，计费/配额/外部网络）；目标部署环境；streaming 明确不做 | **P11 独立验收通过**；真实付费 Provider/目标部署环境待环境；Model Update/RotateProviderSecret proto scope_kinds 契约缺口保留 |
| P12 Control/Panel | **范围完成**（P12l/P12j/P14g-i）；host operation/bootstrap_credential 保持 not_implemented | `p12_control_owner_gateway`、`p04_control_auth_https`、`p12l` principals、`p12j_panel_live` 严格模式 PASS；原生 Control `/api/v1/health` 200 HEALTHY；P14g occurrence/readiness、P14l/P14m audit/log 修复 | 真实 Panel 浏览器 live 全批准入口；真实四依赖健康环境；host operation/bootstrap_credential 字段级证据待决策 | **P12 host operation / bootstrap_credential 保持 not_implemented（用户确认，不扩契约）**；Docker/跨主机 |
| P13 生命周期/可观测 | **生产退出通过（本验收范围）** | **`p13h3_joint_delete_production_e2e`**：真 PG16.4 四库+真 Identity bootstrap+真 mTLS Runtime/Scheduler+真 plugin-host+受管 SDK 插件；BeginDelete→真实 Host receipt+Scheduler ListTasks/CancelTask→全 Done→DELETED、Host UNKNOWN/离线 BLOCKED、Scheduler denied BLOCKED、Restore 不重建 task/凭据/租约、再次 BeginDelete 重新调度、跨实例任务不受影响；P13h-1 `p10_timer_e2e` platform-peer 矩阵、P13h-2 `p13_scheduler_lifecycle`/`p10_scheduler_task_client` | 真实 Host UNKNOWN 注入（当前 HostBridge 测试客户端）、Scheduler 完全离线主链引用既有生产证据、P14 多主机/真实满盘/生产规模 | **P13 生产退出通过（本验收范围）**；B 未批准；多主机/满盘/生产规模待 P14 环境 |
| P14 部署/故障/交付 | 交付材料/原生矩阵完成；checkpoint 门禁进行中 | 此前 exact-HEAD gate **148/148@1213790**（687.02s）；P14f/i/j/k/l/m 原生栈矩阵 9 success+1 stable-ok+0 known-gap；P13/P11/P10k/P05b 通过证据见各行；checkpoint `7b8fb0f` env 运行中（结果回填） | 单机 Docker Compose 六服务栈、两主机/断网、远端 CI/release；真实 QQ/NapCat、真实付费 Provider、生产 CA/密钥/备份离站；部署级错误 Runtime 叶证书；Model Update/RotateProviderSecret proto scope_kinds 契约缺口 | Docker/两主机/远端 CI/Release 环境（用户准备 Docker/gh 中）；QQ/Provider/CA/密钥/备份离站 |

> P00–P03（工具链、契约、PG/UoW/初始化）作为前置基线已由 `p03_postgres_database`、`p02_core_process`、`p01_*`、contract 门禁覆盖，证据见 [architecture-p02-acceptance.md](../reports/modules/architecture-p02-acceptance.md)、[architecture-p03-postgres.md](../reports/modules/architecture-p03-postgres.md)、`doc/11`。

## 3. P10m 修复前后对比（final gate 的前置）

- **修复前**：`p10_timer_e2e` 在 8192 tasks/expiry churn 后 SIGTERM Runtime 偶发失败约 1/3（17–18s；受控诊断 4 次循环中 1 次），退出码 -11，backtrace 为 OpenSSL `EVP_RAND_CTX_free`/`init_thread_stop` atexit 竞争。
- **根因**：OpenSSL 3 `OPENSSL_cleanup` 在 atexit 释放 RAND 状态，gRPC/service worker 线程在进程 teardown 后段退出，TLS 析构访问已释放状态。修复：共用 `run_service` 早期 `OPENSSL_init_crypto(OPENSSL_INIT_NO_ATEXIT)`，不注册退出期全局清理；停机状态机/信号/RPC/DB 语义不变。
- **修复后**：`p10_timer_e2e` 新增 10 次“8192 filler 插入/删除→SIGTERM→Runtime 重启”循环；连续 5 次完整 e2e（50 次停机）全 exit 0，另 12 次完整 e2e 无崩溃；final gate **148/148@1213790** 包含 `p10_timer_e2e`/p10 抽验通过。提交：`6bff2c9` fix、`532b1d3` test、`e83a988` docs。
- **P00n SQLite 竞态修复**：首跑 `sqlite_runtime_store` 偶发双 Conflict（两个独立 SQLite 连接读同一 revision 后竞争 UPDATE，无 busy timeout 导致双双 `SQLITE_BUSY`）；修复为对 peer writer 最多等待 50ms + 显式 ready latch，WHERE revision 仍决定唯一 winner。final gate `sqlite_runtime_store` Passed 0.27s；正式 release SHA 仍需重跑全量并归档。
- **P06h 真实链补齐**：`p07_adapter_e2e` 扩展 fake OneBot 平台延迟、PG deadline 过期→EXPIRED、迟到 ActionResult 仅写 evidence、SIGKILL adapter 后 recover 生成 UNKNOWN evidence 补报；重复平台帧不新增事件、乱序事件不重复 target。真实 QQ/NapCat 仍需环境。

## 3a. P06h 真实链补齐（2026-11-03）

`tests/architecture/p07_adapter_e2e.py`（真实 adapter/fake OneBot/Runtime/真 PG）扩展：fake OneBot `delay_seconds` 制造平台延迟，Action 派发后把 PG deadline 推进到过去→Runtime maintenance 转 EXPIRED；平台响应迟到时只写 `adapter_result_evidence`，动作终态保持 EXPIRED、plugin 查询不出现 known-success、平台副作用仅一次。SIGKILL adapter during platform call 后重启同一 OUTBOX_PATH，adapter `recover()` 生成 UNKNOWN evidence report 并重连补报；Runtime Action 保持 ACCEPTED/非 known-success、evidence 行存在、平台调用不重复。重复平台 frame 不新增 event；乱序两事件各投递一次、无重复 target。final gate `p07_adapter_e2e` Passed 29.19s。真实 QQ/NapCat 与生产 adapter 重启窗口仍需环境。

## 4. 原生单机目标栈矩阵（P14m 后，final）

`p14_native_single_host`（真实 Identity/Runtime/Scheduler/Model/Control 五进程 + owned PG16.4 + 临时 PKI + Control HTTPS）最终 strict 矩阵：

- **success 9**：health（200 HEALTHY 四真实组件）、principals（P12l 真实页）、logs（200 + identity 日志行）、audit（200 + identity 审计行/分页）、tasks_list（200 空页）、models（200 空页）、action_query（404 NOT_FOUND owner 语义）、delete（404 NOT_FOUND）、restore（409 CONFLICT）。
- **stable-ok 1**：action_policy GET（404 NOT_FOUND，fixture 未播种 account policy，稳定 owner 业务语义）。
- **known-gap 0**。
- 故障/重启/fail-closed（bad CA/unauthenticated DB）矩阵保持通过；`p12j_panel_live` 严格模式在同一栈上通过（健康 200、principals capability/known-gap 口径已随后收口）。

## 5. 统一未验收清单（P14t 收敛）

以下为剩余未验收项；其余阶段/链路按本矩阵对应行已有通过证据，不再泛列。

| 类别 | 状态 | 说明/退出条件 |
|---|---|---|
| 单机 Docker Compose 六服务栈 | **未验收** | 本机无 Docker；`target-smoke` 仅离线检查。需 Linux Docker 主机执行六服务镜像 build、`docker compose config/up`、官方 PG16.4 初始化/迁移、healthcheck、备份恢复 |
| 两主机/断网演练 | **延期（用户确认可选）** | 两主机拓扑、网络分区、TLS/撤销/重连、协调恢复未验证 |
| 远端 CI / release | **未运行/未就绪** | `gh` 未认证、GitHub Actions 未触发；正式发布无 release SHA 全量归档、无推送/签名镜像、无 SBOM/cosign/release-gated Environment |
| 真实 QQ/NapCat | 待环境 | 历史首轮手工验收；长期稳定性/网络抖动/平台 reaction 未自动矩阵；P10k 真实平台触发仍环境项 |
| 真实付费外部 Model Provider | 待环境 | T07b 付费联调/计费核对未执行；P11h 使用本地真实 HTTP Provider fixture，不等于生产 Provider |
| 生产 CA/密钥托管、备份离站 | 待用户环境/决策 | CA/root key 托管、轮换/吊销、KMS/离线/分片；备份保留周期、离站位置/频率、生产数据量恢复演练未定 |
| 部署级错误 Runtime 叶证书 | **未验收** | P11h 以 forged signature/错误 origin 覆盖在线拒绝路径，但 fixture 只登记一个 Runtime peer；替代 Runtime 叶证书的部署级错误证书场景未验证 |
| Model Update/RotateProviderSecret `scope_kinds` 契约缺口 | **已知缺口** | proto 缺 `scope_kinds`，真实 Control 管理 RPC 无法经 Identity CheckAuthorization；e2e 以 test-only seed helper 绕过；不扩契约，保留为后续契约任务 |

## 6. 用户决策与实施状态（2026-11-03，P14t）

用户决策登记于 `reports/analysis/user-decisions-2026-11-03.md`；**已批准不等于全环境完成**。

| 项 | 决策 | 实施/验收状态 | 说明 |
|---|---|---|---|
| P13 | 限定批准 A：Scheduler mTLS 平台身份+白名单+显式授权，仅 ListTasks/CancelTask；ListTasks 限目标实例、CancelTask 核对归属、跨实例/跨安装拒绝；生产验收等真实 Host 回执+Scheduler cleanup→DELETED，UNKNOWN/离线 BLOCKED，Restore 不重建任务/租约 | **生产退出 PASS（本验收范围）**：`p13h3_joint_delete_production_e2e`；P13h-1/2 证据已合入 | 残余：真实 Host UNKNOWN 注入、Scheduler 完全离线主链引用既有证据、多主机/满盘/生产规模属 P14 环境 |
| P05b / P11 / P10k | 批准 B1（Control 管理员触发实例主体父委托）；P11 验收真实 managed plugin+真实 HTTP Provider fixture（非 fake port）+撤销/过期/跨实例负例；P10k 同批解锁 | **B1 链完成；P11 独立验收 PASS（37/37 相关回归）；P10k 真实 Scheduler 任务链 PASS**；安全评审 0 Critical/High，Medium F-1/F-2 已修，F-3/F-4/F-5/F-6 加固中（P05b-13） | 真实付费外部 Provider、目标部署环境、部署级错误 Runtime 叶证书、Model Update/RotateProviderSecret scope_kinds 仍为环境/契约项 |
| P12 | host operation/bootstrap_credential 字段级缺口保持 `not_implemented`（用户确认），只文档化、不扩契约 | 无新增实现任务；契约/文档保持 not_implemented | 不是完成；后续需用户另批契约 |
| P14 | 用户准备 Docker 与 `gh` 登录，之后继续 Compose/CI/release 验收 | 单机 Docker Compose、远端 CI、正式发布仍未运行 | 环境项，不以文档替代验收 |
| 两主机/跨主机 | 用户确认可选、延期 | 未做 | 延期 |

## 7. 维护规则

- 每次阶段完成或发布准备时更新本矩阵；只把有命令/测试记录/报告支撑的项标为“已实测”。
- 用户确认项变化时同步 `doc/08`、`doc/12`、`doc/23` 与发布清单；`doc/18` 为冻结基线，只读不改。
- 证据引用优先指向 `reports/modules/` 与 `ctest` 名称；测试改名/删除时必须同一变更更新本文件与发布清单。
