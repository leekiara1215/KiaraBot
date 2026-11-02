# KiaraBot P04–P14 阶段退出证据矩阵（P14d）

- 版本：v1.0（2026-11-02）
- 基线提交：`b4adf9c`（main）
- 用途：发布前统一回答“哪些已经实现、哪些已实测、哪些仍待验收/缺口/用户确认”，**不替代**各阶段原始报告与测试记录，也不把提交/文档存在当作阶段完成。
- 相关文档：[发布清单](28-p14d-release-checklist.md)、[实施计划](23-implementation-plan.md)、[运维 runbook](08-operations.md)、[CI/CD](12-ci-cd.md)、[契约状态](01-contracts.md)。
- 证据口径：`已实测` 必须能给出可复现命令 + 测试名 + 日期/HEAD；本矩阵记录的“最近全量门禁”来自 P14b-2 交付时 Lead 确认的 134/134，**本 P14d 文档任务未重跑全量构建/CTest**，正式发布前必须在 release SHA 上重跑（见发布清单第 3 节）。

## 1. 状态定义

| 状态 | 含义 |
|---|---|
| 已实现 | 代码/契约/文档已合入 main；未配置依赖时按契约显式失败，无 stub/假成功 |
| 已实测 | 在真实依赖（PG16.4/SQLite/TLS/进程等）或明确 loopback 假服务上按测试命令跑过，并有报告或 ctest 记录 |
| 待验收 | 已有实现但验收条件（真实环境/跨主机/远端/人工确认）尚未满足 |
| 已知缺口 | 明确未实现或仅部分实现，发布说明必须保留 |
| 用户确认项 | 需要用户/产品确认后才能继续的语义、策略或环境项 |

`已实现` 不代表 `已实测`；`已实测（本地）` 不代表单主机 Compose、跨主机、远端 CI 或正式发布已验证。

## 2. P04–P14 矩阵

| 阶段 | 已实现 | 已实测（命令/测试名/日期/HEAD） | 待验收 | 已知缺口 | 用户确认项/证据 |
|---|---|---|---|---|---|
| P04 Identity/Control 认证 | Identity domain/application/PG/RPC、登录/刷新/会话撤销、委托/交换、assignment/fingerprint、证书与时钟安全、Target Control HTTPS 认证映射 | `cmake --preset full-locked && cmake --build --preset full-locked && ctest --test-dir build/full-locked -R 'p04_'`；代表测试 `p04_identity_application`、`p04_identity_rpc`、`p04_control_auth_https`、`p04_identity_daemon_tls_expiry`；2026-11-02 前序门禁 | 生产 CA/密钥托管、真实 QQ 登录态复验在用户环境补做 | 真实 deployed CA/secret manager 未接；admin bootstrap 仅受控 fixture | 无新增；证据：[architecture-p04-control-evidence.md](../reports/modules/architecture-p04-control-evidence.md)、`doc/06` |
| P05 Runtime 资源/授权链 | 账号/适配器绑定、安装/实例/desired/config、operation、Runtime→Identity 在线授权与 peer 校验 | `ctest -R 'p05_'`；`p05_runtime_resource_application`、`p05_runtime_identity_e2e`、`p05_runtime_identity_client` | 真实多实例/多安装规模压测（仅模拟适配器/fixture） | **P05b 深链路未完成**：Runtime 获取 Model 等下游实例委托的生产来源未接线，P11c production delegation provider 仍 fail-closed；跨服务实时委托刷新未验收 | **需用户确认委托语义**：Runtime 应以何种 parent proof/Exchange 获取 audience=model 实例委托；证据 `doc/02`/`doc/22`、[architecture-p05-runtime.md](../reports/modules/architecture-p05-runtime.md) |
| P06 事件/投递/SDK/Adapter | Runtime 事件/水位/目标/尝试、PluginBridge/AdapterBridge 外部 TLS 监听、SDK 注册/ACK/续租、NoneBot adapter、真实闭环 acceptance | `ctest -R 'p06_'`；`p06_bridge_acceptance`、`p06_client_e2e`、`p06_postgres_runtime_events`；2026-11-02 真实 TLS/PG 验收报告 | 真实 QQ/NapCat 长期稳定性与真实网络抖动属用户环境验收 | 真实平台（QQ）链路仍需人工；fixture 只证明协议链 | 无；[architecture-p06-event-delivery.md](../reports/modules/architecture-p06-event-delivery.md)、`doc/03`/`04` |
| P07 Action/配额/执行 | Action 受理/dispatch/result/evidence/receipt、幂等/配额/策略/取消/EXPIRED、SDK/Adapter/Core 闭环、Control Action 查询 REST；P07g 证据矩阵（action_rules/PG16.4/bridge P07/真实 adapter e2e/SDK action pytest/p12 gateway） | `ctest -R 'p07_action_rules|p07_postgres_action_records|p07_adapter_e2e|p06_bridge_acceptance|p12_control_owner_gateway'` 5/5；SDK test_actions 28 passed；P07g 报告矩阵 | 真实平台动作（Mute/Kick/撤回/审批）与 reaction API、平台延迟/EXPIRED 故障注入、UNKNOWN 崩溃补报、跨主机 | 高风险动作真实副作用不可自动重放；真实 QQ 仅手工首轮，未形成自动矩阵 | [architecture-p07-action.md](../reports/modules/architecture-p07-action.md)、`doc/17`（只读风险说明） |
| P08 Host/期望状态/配置 | Host process/bridge/SQLite journal、HostService 7 方法、desired/outbox/observations、SDK config ACK、目标单主机 Compose 组件 | `ctest -R 'p08_'`；`p08_host_*`、`p08_postgres_host_registration`；真实 TLS HostBridge 验收；P14a 部署文件/离线检查（未启动 Docker） | 真实受管进程与 Docker/Compose 生命周期、Host 配置应用证据端到端 | P08b-3 host_commands/outbox/observations 仍为部分范围；Host/插件 ACK 与 Runtime 期望/Control 展示的三类事实尚未在真实 Compose 中验收 | 无；[architecture-p08-host.md](../reports/modules/architecture-p08-host.md)、`doc/08` |
| P09 Namespace/Storage | 私有/共享 namespace、CAS、TTL、配额、receipt、SDK 方法与 Runtime 申请/REST 转发部分 | `ctest -R 'p09'`；`p09_postgres_storage_records`、`p09a_runtime_storage_application` | Control/Panel 共享 namespace 管理入口的 live 验收 | 部分 REST/Panel 管理入口未接入真实 Control；P09b/C 范围按 doc/23 分批 | 无；[architecture-p09-storage.md](../reports/modules/architecture-p09-storage.md)、`doc/02` |
| P10 Scheduler/定时交接 | 任务查询/取消、occurrence、TimerTriggered 交接、时区、配额、Runtime 接收、Control REST 任务查询/取消 | `ctest -R 'p10'`；`p10_postgres_scheduler`、`p10_scheduler_rpc`、`p10_timer_e2e`；2026-11-02 残余矩阵验收 | 跨主机定时交接/网络分区演练（两主机延期） | 跨主机触发只在本机多进程验证；真实时钟/时区环境有限 | 无；`doc/23` P10 记录、`doc/02` |
| P11 Model 网关 | Model 两次事务/PG、HTTP Provider/AEAD、调用者 Identity 校验、Runtime→Model client、SDK model.chat、PluginBridge model_chat、P11e 真实 TLS bridge fixture、Model 管理 REST（P11d） | `ctest -R 'p11'`；`p11_model_*` 系列测试（`p11_model_gateway_application`、`p11_model_http_provider`、`p11_model_identity_client`、`p11_runtime_model_client`、`p11_model_bridge`、`p11_control_model_rest`）；2026-11-02 前序门禁 | 单一真实链路（插件→Runtime→Model→Provider）端到端 | **P05b 委托来源缺失**导致 P11e 只到 fake RuntimeModelPort；真实付费 Provider 联调需环境/授权（T07b/P14）；streaming 明确不做 | 需用户确认是否接受分层证据退出 P11；[architecture-p11-model.md](../reports/modules/architecture-p11-model.md)、`doc/03` |
| P12 Control/Panel | Control REST owner gateway（Action/绑定/安装实例/策略/任务/主体/健康/日志审计 503/operation 快照/实例 plugin-state/config）、health.read、P12a 契约、Panel 骨架与 P12b mock | `ctest -R 'p12|control|p04_control'`；`p12_control_owner_gateway`、`p04_control_auth_https`（真实 TLS）；P12d live 覆盖率见 Panel 测试 | 真实 Panel live 全批准入口/拒绝/版本冲突/断流恢复；真实四依赖健康环境 | **target Control 无 WS**（P12a-2 决定延期，REST operation 快照为 resync 源）；日志/审计仍无权威来源（503）；部分 owner 端点依赖 P05b/Model REST | 用户已确认 Q1–Q8 与 P12a-2；证据 `doc/06`、[control-api.md](../reports/modules/control-api.md)、[control-panel.md](../reports/modules/control-panel.md) |
| P13 生命周期/可观测 | Runtime delete/restore/tombstone/retention、Host drain、SDK drain、清理批次；P13d maintenance 结构化日志（blocked/cleanup_failed 计数）+ GetOperation participants + ListAudit/ListLogs 排障步骤 | `ctest -R 'p13|p12|p08|p05|p03_postgres'`；`p13_runtime_lifecycle`（含 cleanup 故障注入）、`p13_host_sdk_drain`；P13d 报告矩阵 | 真实满盘/长时间保留/跨主机协调恢复、真实 Host/Scheduler 参与者完成、生产规模积压 | 指标/告警通道、集中日志、health.updated WS 仍缺；满盘仅代码路径故障注入，非真实磁盘 | [architecture-p13-lifecycle.md](../reports/modules/architecture-p13-lifecycle.md)、[architecture-p13a-runtime-lifecycle.md](../reports/modules/architecture-p13a-runtime-lifecycle.md)、[architecture-p13b-host-drain.md](../reports/modules/architecture-p13b-host-drain.md) |
| P14 部署/故障/交付 | `deploy/` 单主机 Compose 文件与脚本、`target-smoke` 离线检查、P14b 本地 CI/工具链、P14b-2 GitHub Actions（门控 + 证据 artifact）、GHCR release 骨架（SBOM+cosign 门槛）、P14c 备份/恢复/故障 runbook 与本地真 PG16.4 演练 | `scripts/deploy/target-smoke.sh`、`target-smoke --release`（缺 digest 失败）、`scripts/ops/target-drill.sh`（PASS，RPO=0s，RTO≈4.6–12.1s 小数据量，2026-11-02）、`bash scripts/ci/check-workflows.sh`（本地通过，未触发远端） | **单机 Docker Compose 未验证**（本机无 Docker）；**两主机/跨主机断网延期**；**远端 CI 未运行**；**正式发布未执行** | Docker 构建/Compose/healthcheck/官方 PG 镜像初始化/迁移/恢复未跑；GHCR 未推送/未签名（无凭据）；官方 PG digest、备份保留/离站、KMS/密钥托管、release tag/Environment 审批待确认；两主机演练延期 | **待用户确认**：备份保留周期/离站位置与频率、密钥托管方式、release GHCR 凭据/签名 OIDC/Trivy 阈值/tag 保护；参见 `doc/08` P14c/P14a-2、`doc/12` P14b-2 |

> P00–P03（工具链、目标契约、PG/UoW/初始化）作为前置基线已由 `p03_postgres_database`、`p02_core_process`、`p01_*` 等覆盖，详细证据见 [architecture-p02-acceptance.md](../reports/modules/architecture-p02-acceptance.md)、[architecture-p03-postgres.md](../reports/modules/architecture-p03-postgres.md)、`doc/11`。

## 3. 跨阶段证据缺口（发布前必须处理或明确接受）

1. **P05b 委托深链路**：Runtime→Model/其他下游的实例委托生产来源未实现，导致 P11 无法完成单链路 e2e；P11e/P11c 证据是分层 fixture。需用户确认委托语义或排期 P05b。
2. **P14 Docker/Compose 链**：本机无 Docker，P14a-2 只做文件/脚本/离线检查；Compose 启动、官方 PG 镜像 digest、初始化/迁移/恢复、healthcheck 生命周期均未运行。
3. **两主机/跨主机故障演练**：用户确认默认单主机 Compose，两主机延期并单独记录；未验证网络分区、TLS/撤销/重连与协调恢复。
4. **远端 CI**：P14b-2 workflow 已合入但未在 GitHub Actions 运行；`gh` 未认证、未 push 分支；需手动 `workflow_dispatch` 跑绿后再启用 `CI_AUTO_ENABLED`。
5. **正式发布**：未推送/未签名镜像，未配置 `release-gated` Environment 与 `RELEASE_PUSH_ENABLED`/`RELEASE_COSIGN_KEYLESS`；SBOM/Syft 与 Trivy 结果、cosign 验签未有实际 artifact。
6. **真实平台/模型**：真实 QQ/NapCat 仅历史首轮手工验收；真实模型 Provider 未联调（T07b/P14 环境）；不能以 loopback/fake 替代。
7. **可观测性**：指标/告警、集中日志、health.updated WS 事件与真实容量/满盘演练缺位。
8. **日志/审计来源**：Control `/logs`、`/audit` 当前按契约返回 503 UNAVAILABLE；无权威聚合来源前不得在发布说明中描述为已实现查询。
9. **文档一致性**：P14c runbook 示例中的 `--expect runtime=14` 已落后当前 Runtime schema 15，本文档同时修正（见 `doc/08`）；发布前需按 release SHA 重新核对四服务 schema/config 版本。
10. **证据重跑**：矩阵引用的门禁是前序 HEAD 运行结果；正式发布必须在 release SHA 上重跑全量 full-locked build/CTest、contracts、format/boundaries、SDK/panel 门禁并归档 JSON artifact。

## 4. 交付分类（不得混写）

| 类别 | 当前状态 | 说明 |
|---|---|---|
| 单机部署（默认目标） | **未验收**：Compose 文件/脚本离线检查通过，Docker 未运行；本机历史单机 reference MVP 已验收，但那是不同运行轨 | 需要在有 Docker 的 Linux 主机执行 `docker compose config/up`、初始化/迁移、healthcheck、备份恢复演练后才可宣称 |
| 跨主机演练 | **延期**（用户确认可选） | 两主机拓扑、网络分区、TLS/撤销/重连、协调恢复均未验证 |
| 远端 CI | **未运行**：workflow 与本地静态检查已就绪，`gh` 未认证/未触发 | 需用户在 GitHub 仓库先 workflow_dispatch full-build 绿色，再设 `CI_AUTO_ENABLED` |
| 正式发布 | **未就绪**：pipeline 骨架/gating 已合入，未推送签名镜像，无 release artifact | 依赖 Docker 验收、release Environment/变量/凭据、SBOM/cosign 证据与发布清单签署 |

## 5. 维护规则

- 每次阶段完成或发布准备时更新本矩阵；只把有命令/测试记录/报告支撑的项标为“已实测”。
- 用户确认项变化时同步 `doc/08`、`doc/12`、`doc/23` 与发布清单；`doc/18` 为冻结基线，只读不改。
- 证据引用优先指向 `reports/modules/` 与 `ctest` 名称；测试改名/删除时必须在同一变更更新本文件与发布清单。
