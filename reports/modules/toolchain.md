# 工具链（kiarabot-toolchain）覆盖与缺口分析

> 分析对象：main@8caecb1；静态分析，未构建/未运行测试（本机无 cmake/protoc/node/clang-format/clang-tidy，有 python3.9/ruby/git）。

## 1. 范围与规模

- 目录/文件：
  - `scripts/check-toolchain.sh`（119 行，统一检查入口）
  - `scripts/quality/check-format.sh`(8)、`check-tidy.sh`(54)、`check-openapi.sh`(6)、`check-asyncapi.sh`(10)、`check-boundaries.py`(44)、`validate-asyncapi.cjs`(22)、`tests/boundaries_test.py`(51)
  - `cmake/verify_file.cmake`(3)、根 `CMakeLists.txt`(148)、`CMakePresets.json`(33)、`.clang-format`(26)
- 规模：21 文件 / 524 行（本模块自身脚本与构建配置）
- 关键入口：
  - 统一检查：`scripts/check-toolchain.sh:35-118`（工具门禁→C++20/protoc 冒烟→脚本语法→边界→dev/proto/独立 infra 三套构建+CTest→契约/格式/tidy/API lint）
  - 构建骨架：`CMakeLists.txt:4`（`KIARABOT_GENERATE_PROTO`）、`:120`（`KIARABOT_ENABLE_INFRASTRUCTURE`）、`:21/:90/:122`（三个 add_subdirectory）
  - 预设：`CMakePresets.json:6-23`（Unix Makefiles；dev/proto 两预设）
- 对应设计文档：`doc/11-toolchain.md`、`doc/00-boundaries.md`、`doc/12-cpp-coding-standard.md`

## 2. 模块职责（依据）

- `doc/00-boundaries.md:104-112`：任何任务须给出构建/测试命令与验收条件；模块须有独立测试和最小启动命令。
- `doc/11-toolchain.md:11-27`：工具版本矩阵与检查方式；`:35-64`：统一检查脚本应覆盖的 12 类检查。
- `doc/12-cpp-coding-standard.md`：C++ 命名/格式/现代用法/测试/提交前检查（格式与 tidy 为强制门禁）。
- `doc/11-toolchain.md:76`：CI 不直接调用 `check-toolchain.sh`，CI 以工作流为准。

## 3. 已实现能力（逐条带证据）

| # | 能力 | 证据 | 对应测试 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 工具存在性与版本下限门禁（cmake/make/cxx/protoc/grpc plugin/node/npm/npx/clang-format/tidy） | `scripts/check-toolchain.sh:7-76` | 无自测，运行时校验 | 完整 |
| 2 | 编译器识别（AppleClang15/Clang16/GCC13）与 C++20＋protobuf/gRPC 实际代码生成冒烟 | `check-toolchain.sh:46-99` | 冒烟编译+生成文件非空 | 完整 |
| 3 | 脚本语法检查 | `check-toolchain.sh:101-103`（仅 `scripts/`） | `bash -n` | 部分（见缺口 7） |
| 4 | 架构边界门禁（Core 业务代码禁止直接 include 外层/基础设施/控制面） | `scripts/quality/check-boundaries.py:9-37` | `scripts/quality/tests/boundaries_test.py`（4 类用例，注册于 `core/CMakeLists.txt:47-48`） | 部分（见缺口 4/5） |
| 5 | dev / proto / 独立 infrastructure 三套构建 + CTest | `check-toolchain.sh:105-113` | CTest（含 deploy 与 boundary 测试，见 `core/CMakeLists.txt:45-57`） | 完整（本机工具齐全时） |
| 6 | 协议契约检查（oneof 判别、enum 一致性、openapi/asyncapi 关键字段、descriptor 生成） | `check-toolchain.sh:114` → `proto/tests/check_contracts.sh:10-61` | 脚本自身即检查 | 完整（无基线时不证跨版本兼容，见 `doc/11:69`） |
| 7 | 格式门禁（clang-format --dry-run --Werror，排除 pb 生成物） | `scripts/quality/check-format.sh:5-8` | 无自测 | 完整 |
| 8 | 静态分析门禁（clang-analyzer-*，warning 即错误，先校验编译库覆盖） | `scripts/quality/check-tidy.sh:6-54` | 无自测 | 完整（本地路径） |
| 9 | OpenAPI lint（Redocly 2.72.1） | `scripts/quality/check-openapi.sh:6` | 无自测 | 完整（依赖网络） |
| 10 | AsyncAPI 校验（parser 3.6.6，Error 级失败） | `scripts/quality/check-asyncapi.sh:9-10` | 无自测 | 完整（依赖网络） |
| 11 | descriptor 存在性校验 | `cmake/verify_file.cmake:1-3`、`CMakeLists.txt:114-117` | `proto_descriptor_exists` | 最小（仅非空） |
| 12 | 单一 C++ 格式配置（LLVM 基线、C++20、include 分组） | `.clang-format:1-26` | — | 完整 |

## 4. 缺口清单

| # | 缺口 | 类型 | 严重度 | 证据 | 影响 |
| --- | --- | --- | --- | --- | --- |
| 1 | **统一检查不覆盖 4 条模块验证链**：面板（npm check:contracts/lint/test/build）、控制 API pytest 独立入口、Python SDK/适配器 pytest、`tests/acceptance/run.sh` 跨语言闭环均不在 `check-toolchain.sh` 中 | 未接线 | P1 | `check-toolchain.sh:101-118` 无 npm/pytest/acceptance；`doc/11:79` 只说"不覆盖适配器/SDK 实现"；`doc/11:93` 把 acceptance 当独立入口 | `./scripts/check-toolchain.sh` 全绿不代表控制面 HTTP/WS、面板、SDK、适配器可用；README「统一检查」易被理解为全量验收 |
| 2 | **未声明 `rg`/`python3`/`ruby` 依赖**：格式脚本硬依赖 `rg`，但工具门禁清单与 `doc/11` 要求表都没有 ripgrep；`python3` 被 CMake `REQUIRED` 与边界/CTest 使用也不在清单；`ruby` 仅 README 提及 | 文档缺失/未接线 | P1 | `scripts/quality/check-format.sh:7`；`check-toolchain.sh:35`；`doc/11:11-27`；`core/CMakeLists.txt:44`；`README.md:65`；`tests/acceptance/run.sh:22` | 在缺 rg 的机器上，统一检查要跑完两次完整构建后才在格式步骤 127 失败；新环境按 doc/11 装不齐依赖 |
| 3 | **本地与 CI 的 clang-tidy 覆盖不一致**：本地用 proto 编译库覆盖 core/proto/tests/control/api/infrastructure；CI 用 dev 编译库只含 core | 未验证 | P1 | `check-tidy.sh:12`；`.github/workflows/ci.yml:183,194`；`CMakeLists.txt:21,90,122`（control/api、infrastructure 需各自 option） | CI 永远不会静态分析 control/api 与 infrastructure 的 C++；这两模块的 analyzer 缺陷只在人工本地检查时可见 |
| 4 | **边界检查只覆盖 Core 直接 include**：只扫 `core/src`、`core/include`；不查传递依赖（A 头文件间接引入外层）、不查 infrastructure→control/panel、不查 adapters/plugins（Python 导入）、不查 CMake target 链接关系 | 半实现 | P1 | `scripts/quality/check-boundaries.py:16`（目录）、`:31-36`（只查两处 `../core/src` 字面路径）；`doc/00:23` 自己声明"检查直接 include，不构成完整传递依赖审计" | `doc/00-boundaries.md` 的禁止依赖只有一小部分被机器强制；跨层耦合可长期存在而不被发现 |
| 5 | **编译器/构建矩阵盲区**：dev 预设不编译 control/api 与 infrastructure；CI build-test 仅跑 dev（GCC13/Clang16），proto 仅 GCC13（vcpkg job） | 未验证 | P1 | `CMakeLists.txt:4,120-122`；`CMakePresets.json:14`；`ci.yml:52,92` | Clang16 从未编译 proto/control/infrastructure；GCC13+Clang16 的"双编译器"承诺（`doc/12-ci-cd.md:59`）只对 Core 成立 |
| 6 | **重复且行为不同的 AsyncAPI 校验器**：`scripts/ci/validate-asyncapi.cjs` 与 `scripts/quality/validate-asyncapi.cjs` CLI、诊断输出、parser 加载方式均不同；本地走 `npm install` 临时装，CI 走 lockfile | 契约不一致 | P2 | 两文件 diff（本报告依据 `diff -u`，见 `scripts/quality/validate-asyncapi.cjs:1-22` 与 `scripts/ci/validate-asyncapi.cjs:1-23`）；`check-asyncapi.sh:9`；`ci.yml:206,224` | 本地与远端可能一个过一个失败；维护两份逻辑易漂移 |
| 7 | `bash -n` 只扫 `scripts/`，漏 `proto/tests/check_contracts.sh`、`tests/acceptance/run.sh`（CI 反而覆盖全部 tracked `*.sh`） | 半实现 | P2 | `check-toolchain.sh:101-103`；`ci.yml:158` | 本地检查与 CI 结果不一致；本地漏检两个关键脚本 |
| 8 | CTest 未加 `--no-tests=error`，测试被静默跳过不会失败 | 半实现 | P2 | `check-toolchain.sh:107,113`；README `README.md:116` 却建议该参数 | 例如缺少 `control/api/tests/.venv` 时 `control_http_websocket_acceptance` 被条件跳过（`control/api/CMakeLists.txt:13`），统一检查不报错 |
| 9 | **非 C++ 语言无质量门禁**：Python 无 ruff/black/pyright；TS 的 `npm run lint` 实为 `tsc --noEmit`（无 ESLint）；Shell 仅有语法检查（脚本里 `shellcheck source=` 注释无人执行）；无 CMake lint、无覆盖率 | 未实现 | P2 | `panel/package.json`（`"lint": "tsc --noEmit"`）；`scripts/deploy/*.sh:5`（shellcheck 指令）；`check-toolchain.sh:101-103` | 占仓库语言一半的 Python/TS/Shell 只有编译或语法级检查 |
| 10 | API/格式工具依赖网络且无离线策略：Redocly 用 `npx --yes` 每次拉取，AsyncAPI 每次 `npm install`；JSON 头文件在 CMake configure 时从 GitHub 下载 | 半实现 | P2 | `check-openapi.sh:6`；`check-asyncapi.sh:9`；`core/cmake/bridge.cmake:6-11` | 离线/受限网络环境无法复现检查；下载失败发生在构建阶段而非门禁阶段 |
| 11 | 协议 runtime 版本下限只在 proto 预设检查；dev 预设不检查 Protobuf/gRPC 存在性 | 半实现 | P2 | `CMakeLists.txt:23-33`（位于 `if(KIARABOT_GENERATE_PROTO)` 内） | dev 构建"不需要协议"是设计，但无法提前发现协议依赖版本问题 |
| 12 | 最小启动命令覆盖不均：控制 API 无独立可执行入口（仅在 Core 进程内，`control/api/CMakeLists.txt:2-9`），不满足 `doc/00:97`"每个模块可单独启动"的字面要求 | 未实现（设计取舍） | P2 | `control/api/CMakeLists.txt:2-9`；对照 `doc/00-boundaries.md:99` | 控制面无法脱离 Core 单独 smoke，排障粒度粗 |

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/11:43` 称统一检查依次覆盖"工具版本、C++20 编译、protoc/gRPC 生成、脚本语法、dev/proto/infra 构建+CTest、契约、格式、tidy、OpenAPI、AsyncAPI" | 与实现一致；但未声明不含面板/SDK/适配器/acceptance | `check-toolchain.sh:101-118` |
| `doc/11:45` 称格式检查"扫描仓库手写 C++ 头文件和源文件" | 实现依赖 `rg`，且 `rg` 未被列为要求工具 | `check-format.sh:7`；`doc/11:11-27` |
| `doc/11:33` 称 CI 的 tidy 用 dev 数据库、control/infrastructure 由本地脚本覆盖 | 与实现一致（属已声明的能力限制，而非漂移） | `ci.yml:183-196`；`check-tidy.sh:12` |
| `doc/11:76` 称 CI 执行"依赖审计"，配合 `doc/12:120` 说明只覆盖 scripts/ci | 一致，但 `doc/12:66` 要求"漏洞扫描 + SBOM"未实现 | `ci.yml:226-235` |
| `README.md:113-116` 暗示 `check-toolchain.sh` 是"统一检查" | 实际省略 4 条模块验证链（缺口 1） | `check-toolchain.sh:101-118` |

## 6. 跨模块影响（只陈述）

- 缺口 3/5 使 CI 对 control-api、infrastructure 的编译与静态分析无强制，回归可能通过这些模块进入 main；需 control-api/infrastructure 模块报告确认其自测是否在 acceptance 中。
- 缺口 4 使 `doc/00` 的依赖方向对 Python/TS 层没有强制执行，panel/adapter/plugin 的越界需各自模块自证。
- 缺口 1 与 operations 模块的部署脚本验证（`scripts/deploy/tests/*`）不同：部署测试已注册进 dev CTest（`core/CMakeLists.txt:49-53`），因此统一检查通过 ctest 间接覆盖，但真实 Compose/Docker 路径仍无验证（见 operations 报告）。

## 7. 覆盖结论

- **一句话结论**：工具链模块把"本机 C++ 质量门禁"做完整了，但它不是全仓库验证入口：控制面/面板/SDK/适配器/跨语言闭环都不在其覆盖内，且本地与 CI 的静态分析和格式文件集不一致。
- 覆盖度估计：工具链自身声明的 12 类检查中 11 类有实现（第 12 类"依赖审计"不在本模块）、但全仓库 10 个模块中仅 C++ 侧 4 个（core/contracts/control-api/infrastructure）被本机 tidy 覆盖、CI 侧仅 core 全覆盖；按"模块×检查"口径约 8/14 条主链路被强制门禁覆盖（口径：核心 10 模块 + 面板/运维/CI 三条横向链路中，能由脚本自动判定的链路数）。

## 8. 未核实事项

- 未在本机运行 `check-toolchain.sh`（无 cmake/protoc/clang-format/clang-tidy），无法确认脚本在干净环境中是否按 doc/11 顺序全部通过。
- `check-tidy.sh` 对头文件的覆盖依赖 clang-tidy 的 `--header-filter` 与 TU 包含关系，未实测哪些头文件实际未被任何 TU 触达。
- CI 中 vcpkg 构建耗时是否接近 90 分钟超时无法本地验证。
- `scripts/quality/validate-asyncapi.cjs` 与 `scripts/ci/validate-asyncapi.cjs` 的语义等价性仅通过源码 diff 判断，未运行两者对比输出。
