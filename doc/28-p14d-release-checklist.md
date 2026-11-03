# KiaraBot P14d 发布清单（Release Checklist）

- 版本：v1.0（2026-11-02）
- 适用：单主机 Docker Compose 默认目标的正式发布准备；两主机/跨主机为可选延期项。
- 关联：[阶段退出证据矩阵](27-phase-exit-evidence.md)、[运维 runbook/P14c](08-operations.md)、[CI/CD/P14b-2](12-ci-cd.md)、[实施计划](23-implementation-plan.md)。
- 原则：**没有证据的项不得打勾**；本清单是发布门禁，不是“已发布”声明。`doc/18` 冻结基线只读。

## 1. 发布元数据（发布时填写）

| 字段 | 值（占位） | 证据/命令 |
|---|---|---|
| 版本 | `vX.Y.Z`（禁止 `latest`） | `release.yml` 输入校验 `^v[0-9]+\.[0-9]+\.[0-9]+...$` |
| Release commit SHA | `<release-sha>` | `git rev-parse HEAD` |
| 分支/tag | `main` / `vX.Y.Z` | tag 必须受保护；发布记录含批准人 |
| 发布负责人/批准 | `<name>` / `<approver>` | `release-gated` Environment 审批记录 |
| 日期（UTC） | `<YYYY-MM-DD>` | workflow run id |
| Schema 版本 | Identity `14`、Runtime `15`、Scheduler `9`、Model `8` | `infrastructure/postgres/schema/v1.hpp`；发布前重新核对 |
| Service config schema | `1`（`doc/08` 严格配置） | `schema_version=1`；Control `control_api` 端点/秘密路径以发布 env 为准 |
| Compose/镜像版本变量 | `KIARABOT_VERSION=vX.Y.Z` | `deploy/target.env`；`target-smoke.sh --release` 校验非 dev 版本与 digest |
| PostgreSQL 镜像 | `postgres:16.4-bookworm@sha256:<digest>` | release 时 PG16 当前修订 + digest；`target-smoke --release` 强制 |
| 镜像 digest | `ghcr.io/leekiara1215/kiarabot-<image>@sha256:<digest>` × 9 | release artifact `digests.txt`；禁止使用 tag 代替 digest 归档 |
| SBOM | `sbom/<image>.spdx.json` + sha256 | release artifact `release-evidence-<version>-<sha>` |
| 签名 | cosign keyless（或受保护密钥）对每个 digest 的签名 | `cosign verify` 记录；签名失败/缺失时**不得发布** |
| 测试证据 | full-locked ctest/contracts JSON artifact + 本清单命令日志 | `ci-local-evidence-<sha>` / `ci-evidence-<sha>` |

镜像列表（必须全部覆盖，不允许只发部分）：`kiarabot-identity`、`kiarabot-runtime`、`kiarabot-scheduler`、`kiarabot-model`、`kiarabot-plugin-host`、`kiarabot-control`、`kiarabot-tools`、`kiarabot-panel`、`kiarabot-adapter`。

## 2. 发布前置门禁（缺一停）

- [ ] release SHA 上 full-locked 构建通过：`cmake --build --preset full-locked --parallel 3`。
- [ ] release SHA 上 **全量** CTest 通过（无 `-E`/label 过滤）：`ctest --test-dir build/full-locked --output-on-failure`；记录总数与失败清单。
- [ ] contracts 门禁：`proto/tests/check_contracts.sh`、`python3 proto/tests/check_implementation_status.py`、OpenAPI/AsyncAPI 校验通过；target 状态翻转均有证据。
- [ ] format/boundaries/toolchain：`scripts/quality/check-format.sh`、`check-boundaries.py`、`check-toolchain`/`scripts/ci/check-workflows.sh` 通过。
- [ ] Python SDK/adapter/panel 门禁：`sdk/python`、`adapters/nonebot` pytest；panel `npm run check:contracts/lint/test/build`。
- [ ] 阶段退出矩阵已评审：未验收项/缺口已由发布负责人明确接受并向用户说明（特别是 P05b、P14 Docker/两主机/远端 CI）。
- [ ] `deploy/target.env` 使用受保护的真实秘密路径，无秘密写入 Git/日志；root key/证书/凭据为只读挂载且 0600。
- [ ] 备份策略已执行：发布前 P14c 离线备份完成并校验 manifest/SHA-256，保留最近完整集；离站/KMS 方案按用户确认。
- [ ] 回滚预案已确认：上一版本镜像 digest、备份集、schema 版本、回滚负责人；数据库迁移遵守 expand/contract，不自动回滚/迁移。

## 3. 测试证据命令（release SHA 上重跑并归档）

```bash
# 1) full-locked 构建与全量测试（必须无过滤）
VCPKG_ROOT="$PWD/build/vcpkg" cmake --preset full-locked
cmake --build --preset full-locked --parallel 3
ctest --test-dir build/full-locked --output-on-failure

# 2) 统一 JSON 证据（本机/CI 同一入口）
scripts/run-ci-local.sh test --json build/ci-local/test.json
scripts/run-ci-local.sh contracts --json build/ci-local/contracts.json

# 3) 契约/质量门禁
proto/tests/check_contracts.sh
python3 proto/tests/check_implementation_status.py
scripts/quality/check-format.sh
python3 scripts/quality/check-boundaries.py
bash scripts/ci/check-workflows.sh

# 4) 运维离线检查（Docker 环境另执行 compose 验收）
scripts/deploy/target-smoke.sh
scripts/ops/tests/ops_scripts_test.sh
```

归档要求：`ci-local-*.json`、full ctest 日志、contract/format/boundaries 输出、release `release-evidence.json`、SBOM、`digests.txt`、cosign 验证输出。缺少证据的项在发布说明中列为“未验证”，不得用历史 run 替代 release SHA。

## 4. 发布流水线（P14b-2 骨架顺序）

1. **手动 CI 首跑**：GitHub Actions 选择 `CI` → `Run workflow`（`base_ref` 传上一提交或 tag，避免 descriptor 自比），确认 `full-build` 的 full-locked 构建 + 全量 ctest 绿色；检查 `ci-evidence-<sha>` artifact。
2. **启用自动 CI**（用户决定：先手动跑绿）：仓库 Settings → Variables 设 `CI_AUTO_ENABLED=true`。确认 PR/push 首次自动运行正常，无 job 被误 skip。
3. **Release dry-run**：`gh workflow run release.yml -f version=vX.Y.Z -f push=false`；确认 build/SBOM 成功，`release-evidence.json` 中 `pushed=false` 且 blockers 记录 dry-run/变量原因。
4. **配置受保护发布**：仓库 Settings 创建 **`release-gated`** Environment（required reviewers、允许的 tag/branch、environment secrets），变量 `RELEASE_PUSH_ENABLED=true`、`RELEASE_COSIGN_KEYLESS=true`；确认 tag 保护规则与发布审批人。
5. **受保护发布**：`gh workflow run release.yml -f version=vX.Y.Z -f push=true` 或推送受保护 `vX.Y.Z` tag；等待 Environment 审批；确认每个镜像 push + cosign 签名 + SBOM 均成功，artifact `release-evidence-<version>-<sha>` 完整。
6. **事后校验**：`cosign verify ghcr.io/leekiara1215/kiarabot-<image>@sha256:<digest>`；核对 SBOM 与 digest；记录 release notes 与 GHCR 可见性；通知 Panel/部署负责人更新镜像 digest。

## 5. 发布后验证与回滚（引用 P14c）

- 单机 Compose 启动：PG → 初始化/迁移（显式，不自动）→ Identity → Runtime/Scheduler/Model → Host → Adapter → Control；每步 readiness 不通过即停止，不跳过。
- 健康：`/health/live` 进程存活；匿名 `/health/ready` 达到接流量条件；受权 `/api/v1/health` 四依赖聚合；Identity/凭据/撤销异常必须 UNHEALTHY，Runtime/Scheduler/Model 单点故障为 DEGRADED 且对应功能 503，不整体摘流。
- 数据层：执行一次 P14c 备份/恢复演练（本地脚本已验证 PASS，生产数据量未验证），确认四库 `schema_versions`、SQLite integrity、材料 0600。
- 回滚：停止新流量并按 P14c 顺序停机 → 使用最近完整备份 `target-restore.sh --confirm --expect identity=14 --expect runtime=15 --expect scheduler=9 --expect model=8` → 恢复上一版本镜像 digest 与配置 → 按启动顺序恢复 → 观察窗口。禁止只恢复单库后启动、禁止服务自动迁移、禁止用新 CA/root key 顶替旧材料。
- 若发布后无法在约定窗口内恢复，保持停机并升级人工处理；不得为缩短停机删除 journal/outbox/未决记录。详见 `doc/08`「P14c 本地真实备份/恢复与故障 runbook」。

## 6. 待确认项（发布前由用户/管理员确认）

- [ ] GHCR 凭据：使用 `GITHUB_TOKEN`（packages:write + Environment 审批）还是独立 PAT？registry owner 固定 `leekiara1215`。
- [ ] cosign：keyless OIDC 的可验证身份策略（workflow/ref/environment）或固定密钥的托管/轮换方案；无签名凭据不得发布。
- [ ] Trivy 是否从 report-only 提升为阻断门禁；阈值与例外审批。
- [ ] 备份保留周期、离站复制位置/频率、密钥托管（KMS/离线/分片）。
- [ ] 单主机 Docker Compose 验收环境（Linux + Docker）、官方 PG16 digest 固定策略。
- [ ] 两主机/跨主机演练是否在本次发布范围外；若在外，发布说明必须明确“未验证”。
- [ ] `release-gated` Environment required reviewers 名单与 tag 保护规则。

## 7. 当前结论（P14t 最终刷新：2026-11-03，基线 HEAD 04f7044；最终门禁 161/162 + p08 负载 flake 孤立通过）

| 交付类别 | 状态 |
|---|---|
| 单机部署（默认目标） | **部分已验收**：官方 PG16.4 的容器健康、TLS、四库初始化及备份恢复已实测；六服务容器共同启动与业务健康检查待验证 |
| 跨主机演练 | **延期/未验证**（用户确认可选） |
| 远端 CI | **未运行**：workflow/本地静态检查已合入，`gh` 未认证，未触发 GitHub Actions |
| 正式发布 | **未就绪**：无推送/签名镜像、无 release artifact，需先完成本清单第 2–4 节 |

最终证据：P13/P11/P10k/P05b 通过证据已合入；P14t checkpoint @7b8fb0f：**161/161 CTest passed @7b8fb0f**（`build/logs/recheck-ctest-155131.log`，1051.70s，2026-11-03 15:51:31→16:09:06；本轮一次全绿、无失败；此前 `1213790` gate 148/148（687.02s）的 `p02_core_process`/`p08_managed_host_e2e` 负载时序 flake 未复现）。首跑 `sqlite_runtime_store` flake 已由 P00n 根因修复（双连接写锁竞争；final gate sqlite 0.27s；此前孤立 14/14、全量重跑 148/148）。P06h 补齐真实 adapter 链 UNKNOWN/EXPIRED/重连证据；原生五进程+PG16.4 矩阵最终 **9 success + 1 stable-ok + 0 known-gap**。P10m 修复优雅停机 SIGSEGV（OpenSSL atexit 竞态）并完成 p10 抽验。P13 生产退出**PASS（本验收范围，`p13h3_joint_delete_production_e2e`）**；P11 独立验收**PASS（P11h，37/37 相关回归）**；P10k 真实 Scheduler 任务链**PASS（`p05b_task_chain_e2e`）**；P05b B1 链完成；安全评审 0 Critical/High，Medium 全部处置（F-1/F-2 已修，F-3/F-4/F-5/F-6 已加固；F-5 host/core 边界文档化）。最终 exact-HEAD gate @04f7044：**161/162 CTest passed，1025.87s**（`build/logs/recheck-ctest-162320.log`，2026-11-03 16:23:19→16:40:30）；唯一失败 `p08_managed_host_e2e` 35.77s（psql SSL negotiation invalid response，套件负载下时序问题）；孤立复跑 **2/2 Passed 58.64s/56.72s**（Lead 57.39s），判定负载时序 flake，不是断言/实现回归。此前 161/161@7b8fb0f/1051.70s。
用户 2026-11-03 决策：P13 限定 A 已批准并**生产退出通过（本验收范围）**；P05b B1 已批准且**链完成**；P11 独立验收通过、P10k 真实 Scheduler 任务链通过；P12 host operation/bootstrap_credential 保持 not_implemented（用户确认，不扩契约）；P14 本机 Docker 已就绪，gh 登录暂缓。
**剩余未验收收敛为**：Docker Compose 六服务栈、两主机/断网、远端 CI/release、真实 QQ/NapCat、真实付费外部 Provider、生产 CA/密钥/备份离站、部署级错误 Runtime 叶证书；两主机延期。决策登记见 [用户决策注册](../reports/analysis/user-decisions-2026-11-03.md)，详见 [P14q 交付状态](../reports/modules/architecture-delivery-status.md) 与 [阶段矩阵](27-phase-exit-evidence.md)。

本清单不能由 AI/文档提交替代真实环境验收；任何一项未完成时发布说明必须保留对应“未验证/延期/缺口”。
