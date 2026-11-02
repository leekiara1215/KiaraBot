# KiaraBot P04–P14 阶段退出证据矩阵（P14n 最终刷新）

- 版本：v1.1（2026-11-03，P14n 交付收口）
- 基线提交：`e83a988`（main；final gate 147/147）
- 用途：发布前统一回答“哪些已经实现、哪些已实测、哪些仍待验收/缺口/用户确认”，**不替代**各阶段原始报告与测试记录，也不把提交/文档存在当作阶段完成。
- 相关文档：[发布清单](28-p14d-release-checklist.md)、[实施计划](23-implementation-plan.md)、[运维 runbook](08-operations.md)、[CI/CD](12-ci-cd.md)、[契约状态](01-contracts.md)、[最终交付状态](../reports/modules/architecture-delivery-status.md)。
- 最终门禁：`build/env/recheck-full-locked.sh` 在 `e83a988` 真实跑 full-locked build + **147/147 CTest passed**（日志 `build/logs/recheck-ctest-035852.log`，2026-11-03）。正式发布仍须在 release SHA 上重跑并归档。
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
| P04 Identity/Control 认证 | 退出通过（本机） | `ctest -R 'p04_'`；`p04_identity_application`、`p04_identity_rpc`、`p04_control_auth_https`、`p04_identity_daemon_tls_expiry`；P04f/P05f 历史证据后保持绿；final gate 147/147@e83a988 | 生产 CA/密钥托管、真实 QQ 登录态复验属环境项 | CA/secret manager、生产证书轮换 |
| P05 Runtime 资源/授权链 | 范围完成；P05b 待决策 | `ctest -R 'p05_'`；`p05_runtime_resource_application`、`p05_runtime_identity_e2e`、`p05_runtime_identity_client`；P14n 回归 147/147 | 真实多实例/多安装规模压测；P05b Runtime→下游实例委托生产来源未接线（P11 单链路受阻） | **待用户决策 P05b 委托语义**（parent proof/Exchange/audience model）；跨主机 |
| P06 事件/投递/SDK/Adapter | 退出通过（本机/真实进程） | `p06_bridge_acceptance`、`p06_client_e2e`、`p06_postgres_runtime_events`、`p07_adapter_e2e` P06g 全总线（真实 adapter+Runtime+受管 SDK plugin） | 真实 QQ/NapCat 长期稳定性/网络抖动；真实平台链路人工验收 | QQ/NapCat 环境；P05b 相关下游委托 |
| P07 Action/配额/执行 | **退出通过（P07f-2）** | `p07_action_rules`、`p07_postgres_action_records`、`p07_adapter_e2e`、`p06_bridge_acceptance`、`p12_control_owner_gateway`；SDK action pytest | 真实平台 reaction API、Control `listActionMetadata` REST 未接线（环境/后续能力，不阻塞 target 契约退出） | 真实平台动作（Mute/Kick/撤回/审批）环境项 |
| P08 Host/期望状态/配置 | **退出通过（P08f）** | `p08_host_*`、`p08_postgres_host_registration`、`p08_host_bridge_e2e`、`p13_host_sdk_drain`；真实 TLS HostBridge | 真实受管进程/Docker Compose 生命周期；Host/插件 ACK 与 Runtime/Control 三类事实的 Compose 端到端 | Docker Compose 环境；跨主机 |
| P09 Namespace/Storage | 范围完成（本机），部分待验收 | `p09_postgres_storage_records`、`p09a_runtime_storage_application`、`p06_bridge_acceptance` 共享成员/namespace 删除隔离 | Control/Panel 共享 namespace 管理入口 live；P09b/C 分批范围 | 真实 Control/Panel live、生产规模 |
| P10 Scheduler/定时交接 | **退出通过（P10m 后）** | `p10_postgres_scheduler`、`p10_scheduler_rpc`、`p10_scheduler_rest`、`p10_sdk_tasks_e2e`、`p10_timer_e2e`；P10m 修复 + 147/147 final gate | 真实 7 天 EXPIRED/多 installation 容量/真实平台触发；Runtime→Scheduler 生产委托来源（与 P05b 同类） | 真实时钟/时区/平台、跨主机；P05b 决策 |
| P11 Model 网关 | 分层证据退出（待确认单链） | `ctest -R 'p11'` 9/9；`p11_model_*`、`p11_control_model_rest`、p03 真 PG 2/2、SDK 141 passed/23 skipped；contract gate PASS | 真实插件→Runtime→Model→Provider 单链路（P05b）；真实外部 Provider 付费联调（T07b）；streaming 明确不做 | **P05b 委托语义**；外部 Provider 环境/授权 |
| P12 Control/Panel | **范围完成**（P12l/P12j/P14g-i） | `p12_control_owner_gateway`、`p04_control_auth_https`、`p12l` principals、`p12j_panel_live` 严格模式 PASS；原生 Control `/api/v1/health` 200 HEALTHY；P14g occurrence/readiness、P14l/P14m audit/log 修复 | 真实 Panel 浏览器 live 全批准入口；真实四依赖健康环境；host operation/bootstrap_credential 字段级证据待决策 | **待用户决策 P12 host operation / bootstrap_credential 字段级**；Docker/跨主机 |
| P13 生命周期/可观测 | **未通过（决策包 P13f）** | `p13_runtime_lifecycle`、`p13_host_sdk_drain`、`p13c` 证据；P13a–e 本机矩阵 | Scheduler participant 生产 ListTasks/CancelTask 委托未确认；BeginDelete/Restore→真实 Host receipt 单链缺失；真实满盘/跨主机/生产规模 | **待用户决策 P13 方案 A/C**（A 平台 peer 豁免需确认；C 仅 test-only 联合证据） |
| P14 部署/故障/交付 | 交付材料/原生矩阵完成；环境未验收 | `build/env/recheck-full-locked.sh` 147/147@e83a988；`target-smoke`、`target-drill`、`check-workflows`、ops/rotate 脚本；P14f/i/j/k/l/m 原生栈最终矩阵 9 success+1 stable-ok+0 known-gap | 单机 Docker Compose、跨主机演练、远端 CI、正式发布未执行；官方 PG digest/镜像/SBOM/cosign 未产出 | Docker/两主机/远端 CI/Release 环境；备份保留/离站/密钥托管、QQ/Provider |

> P00–P03（工具链、契约、PG/UoW/初始化）作为前置基线已由 `p03_postgres_database`、`p02_core_process`、`p01_*`、contract 门禁覆盖，证据见 [architecture-p02-acceptance.md](../reports/modules/architecture-p02-acceptance.md)、[architecture-p03-postgres.md](../reports/modules/architecture-p03-postgres.md)、`doc/11`。

## 3. P10m 修复前后对比（final gate 的前置）

- **修复前**：`p10_timer_e2e` 在 8192 tasks/expiry churn 后 SIGTERM Runtime 偶发失败约 1/3（17–18s；受控诊断 4 次循环中 1 次），退出码 -11，backtrace 为 OpenSSL `EVP_RAND_CTX_free`/`init_thread_stop` atexit 竞争。
- **根因**：OpenSSL 3 `OPENSSL_cleanup` 在 atexit 释放 RAND 状态，gRPC/service worker 线程在进程 teardown 后段退出，TLS 析构访问已释放状态。修复：共用 `run_service` 早期 `OPENSSL_init_crypto(OPENSSL_INIT_NO_ATEXIT)`，不注册退出期全局清理；停机状态机/信号/RPC/DB 语义不变。
- **修复后**：`p10_timer_e2e` 新增 10 次“8192 filler 插入/删除→SIGTERM→Runtime 重启”循环；连续 5 次完整 e2e（50 次停机）全 exit 0，另 12 次完整 e2e 无崩溃；final gate 147/147@e83a988。提交：`6bff2c9` fix、`532b1d3` test、`e83a988` docs。

## 4. 原生单机目标栈矩阵（P14m 后，final）

`p14_native_single_host`（真实 Identity/Runtime/Scheduler/Model/Control 五进程 + owned PG16.4 + 临时 PKI + Control HTTPS）最终 strict 矩阵：

- **success 9**：health（200 HEALTHY 四真实组件）、principals（P12l 真实页）、logs（200 + identity 日志行）、audit（200 + identity 审计行/分页）、tasks_list（200 空页）、models（200 空页）、action_query（404 NOT_FOUND owner 语义）、delete（404 NOT_FOUND）、restore（409 CONFLICT）。
- **stable-ok 1**：action_policy GET（404 NOT_FOUND，fixture 未播种 account policy，稳定 owner 业务语义）。
- **known-gap 0**。
- 故障/重启/fail-closed（bad CA/unauthenticated DB）矩阵保持通过；`p12j_panel_live` 严格模式在同一栈上通过（健康 200、principals capability/known-gap 口径已随后收口）。

## 5. 统一未验收清单

| 类别 | 状态 | 说明/退出条件 |
|---|---|---|
| 单机 Docker Compose | **未验收** | 本机无 Docker；`target-smoke` 仅离线检查。需在 Linux Docker 主机执行镜像 build、`docker compose config/up`、官方 PG16.4 初始化/迁移、healthcheck、备份恢复 |
| 两主机/跨主机演练 | **延期（用户确认可选）** | 两主机拓扑、网络分区、TLS/撤销/重连、协调恢复未验证 |
| 远端 CI | **未运行** | workflow 与本地静态检查通过，`gh` 未认证、未触发 GitHub Actions；需手动 workflow_dispatch 绿后启用 `CI_AUTO_ENABLED` |
| 正式发布 | **未就绪** | 无 release SHA 全量归档、无推送/签名镜像、无 SBOM/cosign/release artifact；release-gated Environment/变量/凭据未配置 |
| 真实 QQ/NapCat | 待环境 | 历史首轮手工验收；长期稳定性/网络抖动/平台 reaction 未自动矩阵 |
| 真实外部 Model Provider | 待环境 | T07b 付费联调/计费核对未执行；fixture provider 不等于生产 |
| 生产 CA/密钥托管 | 待用户决策 | CA/root key 托管方式、轮换/吊销、KMS/离线/分片未定 |
| 备份保留/离站 | 待用户决策 | 保留周期、离站位置/频率、恢复演练生产数据量未定 |

## 6. 待用户决策清单（不替用户拍板）

1. **P13 方案 A/C**：A = 平台 peer 豁免扩展到 Scheduler ListTasks/CancelTask（需确认；仅 Runtime 实例生命周期/插件任务，严格 owner/instance scope）；C = 确认前仅 test-only 联合 e2e 证据（P13 仍标生产未通过）。B 不推荐。
2. **P05b 委托语义**：Runtime 应以何种 parent proof/Exchange 获取 audience=model（及其他下游）实例委托；生产 delegation provider 当前 fail-closed；决定 P11 单链路与 P10 真实委托。
3. **P12 host operation / bootstrap_credential**：Host operation 未知/未接线错误码契约（`semantic.host.unknown_operation_error` 当前 INVALID_ARGUMENT vs 契约 CAPABILITY_NOT_SUPPORTED）与 `bootstrap_credential` 字段级（Adapter/外部插件）target 翻转/消费闭环；需确认是否作为发布前门槛。

## 7. 维护规则

- 每次阶段完成或发布准备时更新本矩阵；只把有命令/测试记录/报告支撑的项标为“已实测”。
- 用户确认项变化时同步 `doc/08`、`doc/12`、`doc/23` 与发布清单；`doc/18` 为冻结基线，只读不改。
- 证据引用优先指向 `reports/modules/` 与 `ctest` 名称；测试改名/删除时必须同一变更更新本文件与发布清单。
