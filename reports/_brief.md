# KiaraBot 十模块覆盖分析 · 分析员简报（Lead 维护，只读复用）

## 0. 已由 Lead 核实的事实（不要重复核，直接引用）

- 仓库 HEAD：`8caecb1`；`main` 与 10 个 `kiarabot-*` 分支**完全一致**（`git rev-list --left-right --count main...<branch>` 均为 0/0）。
- 10 个模块分支各有独立 worktree：`/Users/leekiara/Workspace/KiaraBot.worktrees/<branch>`，均 clean、同一 commit。
- 结论：**分支名是模块范围标签，不是独立代码线**。本轮分析对象 = 该分支名对应目录/文件在 `main` 当前实现（HEAD `8caecb1`）。
- 本机工具：有 `python3.9`、`ruby`、`clang++`、`git`、`sqlite3`；**没有** `cmake`、`ninja`、`node`、`npm`、`uv`、`protoc`、`docker`、`clang-format`、`clang-tidy`。
  → 不能构建、不能跑 CTest/pytest/npm、不能跑 CI。**只做静态分析**；不要尝试安装依赖。
- `doc/18-architecture-baseline.md` 是受保护文档：**只读**，不得修改。

## 1. 模块 → 目录映射（本轮统一口径）

| 分支 | 模块 | 目录/文件范围 | 对应设计文档 |
| --- | --- | --- | --- |
| `kiarabot-contracts` | 协议契约 | `proto/`、`api/`、`proto/tests/` | `doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`proto/README.md` |
| `kiarabot-core-runtime` | Core 运行时 | `core/` | `doc/02-core.md` |
| `kiarabot-plugin-sdk` | 插件 SDK | `sdk/`、`plugins/` | `doc/03-plugin-sdk.md`、`sdk/PLUGIN_SDK_DEPENDENCIES.md` |
| `kiarabot-nonebot-adapter` | NoneBot 适配器 | `adapters/nonebot/`、`tests/acceptance/` | `doc/04-nonebot-adapter.md` |
| `kiarabot-infrastructure` | 基础设施 | `infrastructure/` | `doc/05-infrastructure.md` |
| `kiarabot-control-api` | 控制 API | `control/api/`、`api/`（REST/WS 部分） | `doc/06-control-api.md`、`api/openapi.yaml`、`api/asyncapi.yaml` |
| `kiarabot-control-panel` | 控制面板 | `panel/` | `doc/07-control-panel.md` |
| `kiarabot-operations` | 运维 | `deploy/`、`scripts/deploy/`、`Dockerfile`、`.dockerignore` | `doc/08-operations.md` |
| `kiarabot-toolchain` | 工具链 | `scripts/check-toolchain.sh`、`scripts/quality/`、`cmake/`、`CMakeLists.txt`、`CMakePresets.json`、`.clang-format` | `doc/11-toolchain.md`、`doc/12-cpp-coding-standard.md` |
| `kiarabot-ci-cd` | CI/CD | `.github/workflows/ci.yml`、`scripts/ci/` | `doc/12-ci-cd.md` |

## 2. 项目功能范围（用于判断“是否覆盖”的基准）

判断“这个模块缺什么”必须以项目声明的功能为基准，优先顺序：

1. `doc/18-architecture-baseline.md`（人类基线，九层架构，含未来 C++ 层）；
2. `README.md` 的「目标架构」「仓库组成」「当前状态」；
3. `doc/09-roadmap.md` 阶段 0–9；
4. `doc/16-development-tasks.md`（T01a–T12 台账，有状态与验收记录；注意它是"自述"，要用代码核实）；
5. 对应模块文档 `doc/02`–`doc/08`、`doc/01-*` 的自述职责与限制；
6. `proto/`、`api/` 契约中已声明但可能未实现的 RPC/字段/操作；
7. `doc/17-account-ban-risk.md`（封禁风险：对 Core/Action/适配器的强制约束需求）；
8. `doc/15-cross-layer-gaps.md`、`doc/14-implementation-assessment.md`、`doc/13-*.md`（历史快照，引用时必须标注"历史快照"）。

## 3. 判定口径：七类缺口 + 三级严重度

对每个功能点判为：**完整覆盖** / **部分覆盖** / **未覆盖**。

缺口类型（每条必须归类）：

- `未实现`：契约/文档承诺存在，代码不存在；
- `半实现`：存在但只覆盖子集，或有硬编码限制/只支持一种实现；
- `未接线`：代码存在，但进程启动、构建目标、调用链或配置没用上（找出断点）；
- `未验证`：实现存在但无测试，或测试未纳入 CI/验收脚本；
- `契约不一致`：proto/openapi/asyncapi 与实现或模块文档互相矛盾；
- `测试缺失`：行为存在但没有对应测试（正常/拒绝/超时/依赖故障）；
- `文档缺失/漂移`：文档未覆盖或与代码不符。

严重度：

- **P0**：阻断项目主流程、已声明验收项或安全底线；
- **P1**：功能域缺失/契约未实现，但主流程可跑；
- **P2**：体验、文档、增强项、未来选项。

## 4. 报告模板（严格遵守，写入 `reports/modules/<name>.md`）

```markdown
# <模块名>（<branch>）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/node/uv/protoc/docker）。

## 1. 范围与规模
- 目录/文件：
- 规模：N 文件 / M 行（测试 X 行）
- 关键入口与代码地图（file:line）
- 对应设计文档：

## 2. 模块职责（依据）
- 基线/doc 自述职责：
- 契约来源：

## 3. 已实现能力（逐条带证据）
| # | 能力 | 证据（file:line） | 对应测试 | 状态 |
| --- | --- | --- | --- | --- |

## 4. 缺口清单
| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |

## 5. 与文档/台账声明的不一致
| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |

## 6. 跨模块影响（只陈述，不替其他模块下结论）

## 7. 覆盖结论
- 一句话结论：
- 覆盖度估计：X/Y 个功能点（口径说明：功能点如何枚举）

## 8. 未核实事项
```

## 5. 证据与诚实性规则

- 每条缺口必须给 `path:line`（或 `path:START-END`）证据；**代码行为优先于文档声明**。
- “文档/台账说已实现” ≠ 已实现：必须找到调用链/配置/测试接线点，否则标 `未接线` 或 `未验证`。
- 不要把“未验证”写成“未实现”，也不要把“存在签名”写成“功能可用”。
- 无法确认的写进「未核实事项」，说明卡在哪里；不要编造文件或行为。
- 报告用中文；不贴大段代码，引用行号即可。
- 只写你负责的模块；跨模块结论放第 6 节且标注“待其他模块确认”。

## 6. 共享约束

- **只读仓库**。唯一允许的写操作：创建/覆盖你自己的报告文件 `reports/modules/<name>.md`。
- 禁止修改仓库其他任何文件；禁止 `git add`/`commit`/`checkout`/`stash`；禁止安装依赖或运行构建。
- 不修改 `doc/18`（受保护）；不改任何 `doc/` 文件。
- 行号以当前 HEAD 工作区为准。
