# P14e 交付包 QA + 证据可复现性抽查

- 日期：2026-11-02；worktree `.worktrees/p14e`，branch `codex/p14e`，基线 `439fb9e`。
- 环境：macOS 本机；`build/full-locked`（main build）真实 PG16.4 fixture；无 Docker。
- 结论：**抽样证据在重建后的 main build 上可复现；交付包总体一致，但存在若干过期状态口径（已在本任务修正）与明确的未验收环境项；交付包不能宣称正式发布就绪。**
- 期间 main 推进：QA 开始 `439fb9e` → 抽样/检查期间 `937acb8` → 结束时 `25e24ce`（P14f docs）。全量 recheck 在 `937acb8` 上执行；`25e24ce` 未重新跑全量，release 必须在最终 release SHA 重跑。

## 1. 证据可复现性抽样（11 条）

所有测试使用 main build `build/full-locked`，`ctest -j1`；首次运行前 main build 落后当前源码（p06/p07/p10 失败），执行一次 `cmake --build build/full-locked --parallel 3` 后全部通过。**说明文档证据必须绑定已构建 HEAD；陈旧 build 不能复现。**

| # | 文档声明（doc/27 等） | 命令/测试 | 实际结果 |
|---|---|---|---|
| 1 | P03 真 PG 持久化 | `p03_postgres_database` | Passed 17.9s |
| 2 | P04 Control 认证真实 TLS | `p04_control_auth_https` | Passed 14.8s |
| 3 | P05 Runtime↔Identity 真 TLS/PG | `p05_runtime_identity_e2e` | Passed 9.7s |
| 4 | P06 真实 adapter→Runtime→plugin 事件链 | `p06_client_e2e` | Passed 53.3s |
| 5 | P07 真实 NoneBot adapter 全链路 | `p07_adapter_e2e` | Passed 15.9s |
| 6 | P08 HostBridge 真实 TLS | `p08_host_bridge_e2e` | Passed 2.4s |
| 7 | P09 Storage PG | `p09_postgres_storage_records` | Passed 2.4s |
| 8 | P10 TimerTrigger 端到端（长测） | `p10_timer_e2e` | Passed 44.3s |
| 9 | P11 Control Model REST e2e | `p11_control_model_rest` | Passed 14.3s |
| 10 | P12 Control owner gateway | `p12_control_owner_gateway` | Passed 0.16s |
| 11 | P13 生命周期 | `p13_runtime_lifecycle` | Passed 0.04s |

汇总：`ctest -R 'p03_postgres_database|p04_control_auth_https|p05_runtime_identity_e2e|p06_client_e2e|p07_adapter_e2e|p08_host_bridge_e2e|p09_postgres_storage_records|p10_timer_e2e|p11_control_model_rest|p12_control_owner_gateway|p13_runtime_lifecycle' -j1` → **11/11 Passed**（总 291s 首次失败版；重建后总 175s，全部通过）。测试名、结果与 doc/27 矩阵引用一致；无“声称已实测但实际失败”的项。长测选取 p10_timer_e2e（44s）与 p06_client_e2e/p07_adapter_e2e（真实 adapter 链路）。

首次失败（陈旧 build 证据）：
- p10_timer_e2e Runtime/Scheduler/PG 二进制与源码 schema 不一致，fixture exit 1；
- p06_client_e2e/p07_adapter_e2e SDK plugin registration timeout（`operation=plugin_connect error_code=UPSTREAM_UNAVAILABLE/TIMEOUT`）。
重建后 3/3 通过；记录为“证据重跑前提”，不是测试断言缺陷。

契约/证据交叉：P11f 已记录 OpenAPI 9 个 Model REST operation 仍 inline `not_implemented`，但 P11 Control REST e2e 通过；本次将该项保留在交付 QA 不一致清单（不修改契约）。doc/27 P12 行“日志/审计 503”已过期，已修正为 P12c-6/P12d-4 接线。

## 2. 脚本可用性

| 脚本/命令 | 结果 |
|---|---|
| `/Users/betta/Workspace/KiaraBot/build/env/recheck-full-locked.sh`（本地 build helper，**非仓库跟踪文件**） | HEAD `937acb8`：build exit=0；`ctest` **144/144 passed**（本机 full build + 全部测试）。注：该路径只在 main build env，不在仓库 scripts；不属于可交付/可复现仓库脚本，需交付包补入口或文档注明本地 helper。 |
| `scripts/run-ci-local.sh --self-test`（裸命令） | exit=2（usage；`--self-test` 需要 mode，说明正确用法） |
| `scripts/run-ci-local.sh test --self-test` | exit=0；`run-ci-local: self-test PASS`（JSON writer/mode parsing OK） |
| `scripts/deploy/target-smoke.sh` | exit=0；离线结构/YAML/profile/脚本检查 PASS；官方 PG 镜像/Compose/Docker 运行 UNVERIFIED |
| `scripts/deploy/target-smoke.sh --require-docker` | exit=1（无 Docker，按预期失败并明确说明） |
| `scripts/ops/tests/ops_scripts_test.sh` | exit=0；`P14C OPS SCRIPT CHECKS PASS` |
| `scripts/ci/check-workflows.sh` | exit=0；`workflow checks passed (GitHub Actions were NOT triggered)`；`gh` 已安装但未认证，远端 CI 未触发 |
| `scripts/deploy/tests/target_rotate_test.sh` | exit=0；`P14A ROTATION SCRIPT CHECKS PASS` |

## 3. 交付包一致性检查

- **链接/路径**：README、doc/README、doc/23、doc/27、doc/28、deploy/README、doc/08 共 7 个文档的 Markdown 相对链接全部存在（0 missing）。
- **doc/18 冻结基线**：`git diff 439fb9e -- doc/18-architecture-baseline.md` 为空；最后修改提交为历史 `f8388f0`，本任务未改。
- **秘密扫描**：tracked 源码中仅 `infrastructure/identity_crypto` 的 PEM 头字符串常量（代码校验器），无真实私钥/云 token/密码；测试 fixture 的固定 token 未在交付材料中泄漏。`deploy/target/*` 与 target.env 仍由 `.gitignore` 排除。
- **状态口径不一致（已修正）**：
  1. doc/27 基线 `b4adf9c`、门禁 `134/134` 与当前 `439fb9e` / P14e 抽样/recheck 144/144 不一致 → 已更新为 QA 基线 + 实测结果。
  2. doc/27 P12 行仍称“日志/审计无权威来源（503）” → P12c-6/P12d-4 已接线，已修正。
  3. doc/28 第 7 节“当前结论 HEAD b4adf9c” → 已更新 QA 基线/main 推进说明。
  4. README、doc/23 的“当前基线 b4adf9c” → 已更新 439fb9e/QA 期间推进说明。
  5. doc/27 P08 行仍称 P08b-3 为部分范围 → P08f 残余矩阵已补齐且 P08 退出判定通过，已补证据/口径。
- **其他不一致/需 Lead/契约 owner 处理**：
  - `build/env/recheck-full-locked.sh` 不在仓库，任务把它列为脚本可用性项；交付包没有等价的可跟踪入口（应补 `scripts/` 入口或文档标注非交付 helper）。
  - OpenAPI Model 9 个 REST operation inline `not_implemented` 与 `p11_control_model_rest` e2e 证据不一致（P11f 已记录，本任务不改契约）。
  - `p01_target_roundtrip` 在 P11f 发现 ListAudit scope_kinds 与 target_spec 不一致（P12c-6 遗留）；交付 QA 未改 proto/target_spec，需 Lead/契约 owner 处理。
  - main 在 QA 期间连续推进（439fb9e→937acb8→25e24ce）；本报告 recheck 只到 937acb8，最新 25e24ce 未重跑 full gate，release 必须重跑。
  - P14a-2 doc/27 P14 行称“目标单主机 Compose 文件/脚本离线检查”已实测；Docker Compose/官方 PG 镜像仍未验证；与未验收分类一致。

## 4. 未验收分类（不得混写）

| 类别 | QA 结论 |
|---|---|
| 单机部署（默认目标） | **未验收**：Compose/脚本离线检查通过；本机无 Docker，官方 PG16.4 镜像拉取/初始化/迁移/恢复、Compose config/up、healthcheck 未运行 |
| 跨主机演练 | **延期/未验证**（用户确认可选）：网络分区、TLS/撤销、重连、协调恢复未演练 |
| 远端 CI | **未运行**：本地 workflow 静态检查通过；`gh` 未认证，GitHub Actions 未触发；无远端 artifact |
| 正式发布 | **未就绪**：无 release SHA 全量门禁（最新只到 937acb8 本机 144/144）、无推送/签名镜像、无 SBOM/cosign/release artifact、备份保留/离站/密钥托管未确认 |

## 5. 剩余待用户环境项

- Docker Linux 主机上的官方 `postgres:16.4-bookworm` 镜像与单主机 Compose 全流程验收（init/migrate/healthcheck/备份恢复）。
- GitHub Actions 手动 workflow_dispatch 首跑绿色 + `CI_AUTO_ENABLED`；release-gated Environment/tag 保护/GHCR 凭据/cosign。
- 真实外部 Provider 付费联调（T07b）与真实插件→Runtime→Model→Provider 单链路（P05b 委托语义）。
- 真实 QQ/NapCat 平台首轮/长期稳定性、两主机网络故障矩阵。
- 备份保留周期、离站复制、密钥托管（KMS/离线/分片）决策；目标 release SHA 上重跑全量 build/ctest/contracts/format/SDK/panel 并归档证据。

## 6. QA 判定

- 文档声明的抽样“已实测”项在重建后的 main build 上 **11/11 可复现**，脚本可跑、doc/18 未动、链接/秘密检查通过。
- 交付包**不能**标记正式发布已就绪；未验收项均为环境/发布门禁，不是本地测试失败。
- 已修正事实性过期口径（基线/门禁计数/P12 日志审计/P08 状态/doc28 结论/README/doc23 基线）。
