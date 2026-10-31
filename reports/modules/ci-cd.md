# CI/CD（kiarabot-ci-cd）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未运行 CI（本机无 GitHub Actions/网络远端）。remote CI 历史结果只能引用仓库自述，标注为未核实。

## 1. 范围与规模

- 目录/文件：`.github/workflows/ci.yml`（260 行，唯一 workflow）、`scripts/ci/check-descriptor.sh`(27)、`scripts/ci/test-descriptor-gate.sh`(47)、`scripts/ci/validate-asyncapi.cjs`(23)、`scripts/ci/package.json`(9)+`package-lock.json`、`doc/12-ci-cd.md`(343)
- 规模：7 文件 / 709 行（含文档）
- 关键入口：`ci.yml:7-17`（触发条件）、`:26-259`（8 个 job）
- 对应设计文档：`doc/12-ci-cd.md`、`doc/11-toolchain.md`、`README.md:45-53`

## 2. 模块职责（依据）

- `doc/12-ci-cd.md:7-23`：CI/CD 负责"编译/测试/协议兼容 → 可追踪产物 → 镜像发布 GHCR → staging 自动部署 → 生产审批 → 迁移/健康检查/冒烟/失败回滚"。
- `doc/12-ci-cd.md:55-66`：CI 必须至少覆盖 C++ 编译（GCC+Clang）、Core 测试、协议生成、descriptor、格式与静态检查、OpenAPI、AsyncAPI、镜像扫描（secret/漏洞/SBOM）。
- `doc/12-ci-cd.md:72-84`：分支模型（main 禁 push、PR 必须过 CI）、语义化版本、按 SHA 打镜像标签。
- `doc/12-ci-cd.md:320-331`：验收标准 10 条。

## 3. 已实现能力（逐条带证据）

| # | 能力 | 证据 | 状态 |
| --- | --- | --- | --- |
| 1 | 手动触发与只读权限、并发取消 | `ci.yml:7-24` | 完整 |
| 2 | dev 构建与全部 CTest，GCC13/Clang16 双编译器矩阵 | `ci.yml:27-56` | 部分（仅 dev 预设，见缺口 3） |
| 3 | proto+infrastructure 构建、CTest、跨语言闭环 acceptance（SDK/控制 API/适配器）、面板四连检、descriptor 基线与负例门禁 | `ci.yml:58-132` | 基本完整（结论依赖一次从未验证的全绿运行） |
| 4 | workflow YAML 解析、actionlint、全部跟踪 `.sh` 的 `bash -n` | `ci.yml:134-162` | 完整 |
| 5 | clang-format 门禁（跟踪的手写 C++） | `ci.yml:164-171` | 完整（文件集小于本地脚本，见 toolchain 报告） |
| 6 | clang-tidy（`clang-analyzer-*`，warning 即失败） | `ci.yml:173-196` | 部分（仅 core TU，见缺口 4） |
| 7 | OpenAPI Redocly lint + AsyncAPI parser 校验（锁定 `scripts/ci`） | `ci.yml:198-224` | 完整 |
| 8 | 依赖审计（仅 `scripts/ci` npm 锁文件） | `ci.yml:226-235` | 部分（见缺口 5） |
| 9 | Gitleaks 全历史密钥扫描，失败上传报告 | `ci.yml:237-259`、`.gitleaks.toml:1-16` | 完整 |
| 10 | descriptor 工件上传（SHA 命名，14 天） | `ci.yml:126-132` | 完整（仅此一种工件） |

## 4. 缺口清单

| # | 缺口 | 类型 | 严重度 | 证据 | 影响 |
| --- | --- | --- | --- | --- | --- |
| 1 | **自动触发被关闭**：`pull_request`、`push: branches:[main]` 全部注释，只剩 `workflow_dispatch` | 未接线 | **P0** | `ci.yml:7-17`；`doc/12:95-101,322` | 任何提交/PR 都不会自动编译和测试；doc/12 验收标准第 1 条不成立；回归只能靠人工手动点 Run workflow |
| 2 | **HEAD 的"修复"从未在远端验证**：暂停自动触发前 5 次运行未通过（仓库自述），当前 HEAD 未记录任何全绿运行 | 未验证 | **P0** | `README.md:52`；`ci.yml:3-6`；`doc/12:103` | 主分支质量状态未知；恢复自动触发前无证据表明 CI 可用 |
| 3 | **CD 完全未实现**：无镜像构建/GHCR 推送/SBOM/镜像漏洞扫描/staging 自动部署/production 审批/部署回滚 | 未实现 | P1（doc/12:144 已声明"待实施"） | `ci.yml` 全文无 docker/buildx/GHCR/environment；`doc/12:144,152-279` | `doc/12:7-23` 声明的 CD 责任 6 条中 0 条落地；`doc/12:320-331` 验收标准后 8 条不成立 |
| 4 | **双编译器承诺只覆盖 Core**：build-test 只跑 dev 预设（不编译 control/api、infrastructure）；proto 任务只用 GCC13 | 未验证 | P1 | `ci.yml:52`（dev）、`:87-97`（proto，GCC13）；`CMakeLists.txt:4,21,90,120-122` | Clang16 从未编译 proto/control/infrastructure；`doc/12:59,323` 的"GCC 与 Clang 都通过"对两个模块不成立 |
| 5 | **CI 静态分析只覆盖 core**：clang-tidy 用 dev 编译库 | 未验证 | P1 | `ci.yml:183,194`；`doc/11:33` 已承认 | control/api 与 infrastructure 的 analyzer 缺陷无法被 CI 发现 |
| 6 | **依赖审计范围窄**：只审计 `scripts/ci`；面板依赖在 contracts job 用 `npm ci --no-audit` 安装；Python `uv.lock`、vcpkg C++ 依赖无审计 | 半实现 | P1 | `ci.yml:115,235`；`doc/12:120` 声明 vcpkg 未覆盖但未声明面板/Python 未覆盖 | 面板与 Python 依赖漏洞不会触发门禁；"vulnerability scan"仅名义存在 |
| 7 | **无依赖/构建缓存**：每次从源码编译 pinned vcpkg 的 grpc/protobuf/curl/sqlite3/boost/libsodium | 半实现 | P1 | `ci.yml:70-86`（无 actions/cache）、`:61`（90 分钟超时） | contracts job 是唯一真正的集成门禁，却最慢最易超时/失败；与"从未全绿"的自述互为因果 |
| 8 | 平台覆盖缺口：无 macOS/Windows job，但 README 承诺 AppleClang 15+ | 未实现 | P2 | `ci.yml` 全部 `runs-on: ubuntu-24.04`；`README.md:59` | macOS 开发者的构建差异只有本地 CTest 暴露 |
| 9 | 语言门禁缺口：Python 只有 pytest、无 lint/类型检查；TS 无 ESLint；Shell 只有 `bash -n`；无 CMake lint；无覆盖率门禁 | 未实现 | P2 | `ci.yml` 无 ruff/black/pyright/eslint/shellcheck/coverage | 与 toolchain 报告缺口 9 同源 |
| 10 | workflow-lint 的 python3-yaml 来自 apt，未锁版本；Go 工具（actionlint/gitleaks）按版本安装属可复现 | 半实现 | P2 | `ci.yml:143-156,159-162,249-252` | runner 升级可能改变 YAML 解析行为 |
| 11 | 手工触发时 descriptor 基线依赖 `inputs.base_ref` 默认 `origin/main`；`github.event.before` 为空，expression 落到 inputs | 半实现 | P2 | `ci.yml:9-14,120-123`；`doc/12:100` 要求恢复自动触发时删除兜底 | 在 main 尚未推送/浅克隆环境下手动运行可能找不到基线 |
| 12 | 缺少失败日志/测试报告工件；除 descriptor 与 gitleaks 报告外无 artifacts；大多数 job 无 `timeout-minutes` | 半实现 | P2 | `ci.yml:26-259`（仅 contracts 有 timeout、仅两处 upload） | 失败定位依赖日志滚动；挂起 job 可长时间占用 |
| 13 | `contracts` job 安装 `ninja-build`，但所有预设都是 Unix Makefiles | 半实现 | P2 | `ci.yml:80`；`CMakePresets.json:6` | 无用依赖，暗示预设/CI 曾计划切换但未同步 |
| 14 | 无定时/定时依赖重扫（cron），无 SBOM/供应链溯源 | 未实现 | P2 | `ci.yml:7-17` | 新增 CVE 不会触发检查 |
| 15 | `npm audit --audit-level=high` 只拦 high/critical，moderate 不拦 | 半实现 | P2 | `ci.yml:235` | 中危漏洞长期累积 |

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/12:3` "Python 适配器当前暂存于同级检出……迁入 `adapters/` 后改为仓库内验收入口" | 适配器已在仓库内（merge `f8388f0`），CI 已用仓库内 `adapters/nonebot` | `ci.yml:105-109`；`adapters/nonebot/` 存在 |
| `doc/12:66` 镜像扫描要求"secret scan、漏洞扫描、SBOM" | 只有 gitleaks secret scan；无镜像构建、无漏洞扫描、无 SBOM | `ci.yml` 全文；`doc/12:152-173` 为待实施 |
| `doc/12:117` "GCC 13、Clang 16 分别配置和构建 dev 预设……协议任务配置、构建 proto 并运行全部 CTest" | 与实现一致；但协议任务只有 GCC13，dev 不含 control/infra | `ci.yml:34-40,92` |
| `doc/12:320-331` 验收标准："任意 PR 自动编译测试 / 合并 main 生成镜像 / staging 自动部署 / production 审批 / 回滚 / 可追溯" | 全部不成立或未实现（触发关闭、无 CD） | `ci.yml:7-17`；无 docker/buildx/GHCR |
| `doc/11:31` "CI 另对跟踪的 `.sh` 执行 `bash -n`" | 一致 | `ci.yml:158` |
| `README.md:52` "自 2026-10-30 起暂停自动触发" | 一致 | `ci.yml:3-17` |
| `README.md:52` "HEAD e70de83 仍有 4 个 job 失败"；当前 HEAD 有修复提交 | **未核实**（无法访问远端 Actions 历史） | `git log`：`16191cb` 暂停、`8caecb1` 修复后从未记录全绿运行 |

## 6. 跨模块影响（只陈述）

- 触发关闭（P0）意味着其余 7 个 job 的能力对项目实际零保护；这是"十模块是否覆盖功能"问题里唯一一个**覆盖了但没生效**的模块。
- CD 未实现与 operations 模块（deploy/compose/备份/回滚脚本）重叠：脚本已存在但没有任何流水线调用它们（见 operations 报告）。
- 缺口 4/5 与 toolchain 报告缺口 3/5 是同一事实的两面：CI 的 C++ 矩阵与静态分析未覆盖 control/api、infrastructure。

## 7. 覆盖结论

- **一句话结论**：CI 的 8 个 job 设计上覆盖了构建/协议/格式/静态/API/密钥/依赖七类门禁，但**自动触发被关闭、CD 整章未实现、双编译器与静态分析只覆盖 Core**，因此当前状态的 CI/CD 只具备"手动、部分覆盖"的能力。
- 覆盖度估计：`doc/12:55-66` 要求的 8 类 CI 检查中 5 类完整、3 类部分（双编译器、静态分析、镜像扫描）；`doc/12:7-23` 的 6 条 CD 责任 0 条落地；按 `doc/12:320-331` 的 10 条验收标准，2 条部分成立、8 条不成立（口径：上文逐条判定）。

## 8. 未核实事项

- 远端 GitHub Actions 的实际运行历史（README 说 5 次均未通过、4 个 job 失败）无法离线核实；`8caecb1` 是否全绿未知。
- 分支保护/required checks 是否配置无法从仓库文件判断（`doc/12:122` 要求管理员配置）。
- vcpkg 全量构建在该 runner 上的实际耗时与 90 分钟超时余量未实测。
- 面板 `npm test` 中的 live fixture 是否需要额外端口/环境在 CI 上稳定，未实测。
- `actions/checkout`、`setup-node`、`setup-go`、`upload-artifact` 的 SHA 固定是否对应其注释版本，未逐个核对上游。
