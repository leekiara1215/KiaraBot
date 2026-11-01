# 新架构并行开发协调记录（2026-11-01）

## 授权与统一基点

用户要求复用 `/Users/leekiara/Workspace/KiaraBot.worktrees/`，新开多个对话并行开发，由当前主对话最后统一整理，每个开发独立提交，不能最后一次性提交全部改动。

- 主集成目录：`/Users/leekiara/Workspace/KiaraBot`
- 主集成分支：`codex/architecture-rollout`
- 已确认设计独立提交：`66d0996`（doc/19–22 与裁定清单）
- 实施计划/并行规则独立提交：`6d64b2a`（所有首波分支共同基点）
- 实施依据：[P00–P14 计划](../../doc/23-implementation-plan.md)
- 首版 doc/18 只读；无旧协议兼容/数据导入/双写，不执行清库。

现有十个模块 worktree 初始检查均 clean，但各自停在旧模块分支；不 reset/rebase/删除旧分支，改在原目录建立 `codex/architecture-*` 新分支，共用本轮已提交设计与计划作为开发基点。每次切换前重新检查 clean；如果有新改动则停止并报告，不覆盖。

## 首波开发分工

| 对话任务 | worktree | 新分支 | 首轮交付 | 主写范围 |
| --- | --- | --- | --- | --- |
| 契约 | kiarabot-contracts | codex/architecture-contracts | P00 契约规格及 P01a 目标 proto/API/fixtures，按可验收能力分提交 | proto/、api/、doc/01*、doc/06 的契约说明 |
| 工具链 | kiarabot-toolchain | codex/architecture-toolchain | P00 依赖精确锁、可重复检查、分层目标/阶段门禁规格 | 工具锁、scripts/quality、scripts/check-toolchain、doc/11、根/模块构建规划 |
| Core 内层 | kiarabot-core-runtime | codex/architecture-core | P00 内层规格及 P01b 独立内部模型/端口、纯领域不变量测试 | core/domain、application/ports 的独立值类型与单元测试，不写传输/SQL |

先完成各自 P00 规格并检查已提交基点；已有明确设计部分可独立推进 P01，涉及未落定共同接口只报告主对话，不跨越到其他线程写入域。六进程 CMake/入口和部署变更先不由 Core/契约线程同时改写；P02 在首波规格与契约确认后由主对话分配。

其他已有 worktree 暂不启动依赖尚未落地的业务实现；后续按计划前置开启基础设施、Identity、Adapter/SDK、Control/Panel、运维/CI 等工作。

## 提交与集成规则

1. 每个线程只在指定 worktree 执行，检查实际 cwd、分支与基点；禁止在主目录或他人 worktree 写入。
2. 每完成一个独立、可审查、已做相关验证的能力即提交，使用精确文件列表，不 `git add .`；多个能力不得积成最终一个大提交。
3. 只提交本任务文件，禁止生成物、构建/venv、秘密和机器配置；每次报告 commit SHA、目的、文件、已运行/未运行验证及依赖。
4. 不自行 merge/cherry-pick 他人分支，不改保护文档，不推送远端。需要共同契约更新由主对话协调并给出确认提交。
5. 主对话读各线程状态、按依赖逐 commit 审查/集成和相关验证，保留独立提交，不 squash 成全仓一次提交。
6. 若提交与当前契约不一致、含越界变更或验收缺失，先反馈修复，不直接以“已提交”当作阶段完成。

## 运行状态

首波三个对话已通过 Codex 创建工具启动。已核对三个工作区从干净旧分支切到本轮新分支，HEAD 共同为 `6d64b2a`；旧分支保留。主目录当前无代码改动，设计与计划分两个独立提交保存。

| 对话任务 | threadId | hostId | 运行状态 |
| --- | --- | --- | --- |
| KiaraBot 新架构：契约与 API | 01a1951a-79a9-7130-b932-3699427f75d2 | local | 七个提交已集成、独立契约验收通过；最后一项 lint 表达修正等待工具授权提交 |
| KiaraBot 新架构：工具链与构建基线 | 01a1951a-894c-7bf2-840b-7ef0191b7d9e | local | 首轮两个提交已审查、集成；完整锁定环境与六入口尚未验收 |
| KiaraBot 新架构：Core 内层模型与端口 | 01a1951a-93d3-7ea3-b840-cf35713eb720 | local | 首轮四个提交已审查、集成，独立构建与测试通过；未接入生产路径 |

这些是独立对话，工具默认项目目录为主检出，因此任务提示明确要求每次命令使用指定模块 worktree。主对话核对实际 git 状态，避免默认 cwd 导致串改。阶段状态依提交与实际验证更新，尚不宣称 P00–P14 完成。

主对话已审查模块文件边界；Core 与契约的 fencing 组合、Runtime 专属 UoW 以及直接 Adapter/Plugin 调用者身份已反馈并以独立提交修正。各对话的命令授权由用户在原对话处理，主对话不代批，也不通过替代工具绕过。

## 已集成的独立提交

cherry-pick 保留每个开发提交的能力边界；由于集成分支已有其他提交，集成 SHA 与原分支 SHA 不同。不 squash，也不把未提交内容复制到主工作区。

| 原提交 | 集成提交 | 能力 |
| --- | --- | --- |
| `112ec27` | `338d1bc` | 契约方法规格 |
| `0520cde` | `ab0ddb8` | Core 强类型身份及失败基础 |
| `2ae3b4c` | `4f36e50` | Core 上下文与本地事务端口 |
| `eb32ae4` | `be80bf0` | fence、版本与有界租约不变量 |
| `7d05d2c` | `9671ad2` | 直接调用者类型与可信 fence 映射约束 |
| `5f43135` | `f1f7348` | 依赖精确锁与来源校验 |
| `5088ed5` | `eb8df25` | 编译覆盖/内层依赖/目标/阶段门禁 |
| `e4983db` | `dab2daa` | 公共协议与注册、投递续租帧 |
| `51be69b` | `8026dc9` | 五服务类型化方法及跨语言验证入口 |
| `baa48bb` | `d3934dd` | 状态变更与普通幂等收据分离、方法集合双向核对 |
| `402e42e` | `cb63a3a` | REST/WS 管理契约、严格字段/uint64 schema 与验证 |
| `07e1e8c` | `23fc332` | 契约验证记录与末尾空白修正 |
| `3243aff` | `bc24430` | 受管/外部自管实例的条件 Host 约束 |

以上合计十三个开发提交，均独立保留。契约对话最后的 `api/openapi.yaml` 分支判别字段
显式 required 修正已在该工作区通过检查，但在本记录更新时尚待环境工具授权后提交，
因此没有复制或合入其未提交内容。当前已集成 schema 的行为回归通过；该收尾修正
用于使分支约束表达更清楚，并消除两条组合规则警告。后续只需审查其独立提交再集成，
不能把本记录当作该提交已完成。

## 主集成复验

- Core：`cmake -S core/tests/architecture -B /tmp/kiarabot-rollout-integrated-core`、对应 build 与 CTest 通过（2/2）；直接调用者修正后再次 build/CTest 通过。八个新增/修改的手写 C++ 文件 `clang-format --dry-run --Werror` 通过。
- 工具链：依赖锁离线一致性通过；`python3 -m unittest discover -s scripts/quality/tests -p '*_test.py'` 通过（13 项）；四组 `cmake --list-presets=all` 解析及改动 shell 的 `bash -n` 通过。
- 协议：实现状态登记、枚举一致性、既有语义规则检查通过；主集成目录运行 `KIARABOT_TARGET_PYTHON=/tmp/kiarabot-contracts-venv/bin/python proto/tests/check_target.sh` 通过：77 个 RPC、269 种消息 Python→C++→Python 往返。实际生成/编译了 C++/Python/gRPC；使用本机两端各自匹配的生成器/runtime，非最终锁定环境。生成源码及本机 Abseil 产生 nullability 扩展警告、链接重复库提示，不能宣称无警告 full 构建。
- 主集成在幂等分类和方法双向核对修正后重跑独立协议入口，通过同样的 77 RPC/269 消息；不是仅沿用修正前结果。
- API：`ruby proto/tests/target_api_test.rb` 通过 33 个 HTTP 操作及全部引用/错误映射；将当前 OpenAPI 转临时 JSON 后执行 `target_schema_test.cjs`（临时固定 lint 安装的 Ajv）通过，包含必填、空 ID、零代次、uint64 最大值/溢出、registered/Blocked 条件，以及受管/外部自管实例正反例。
- 正式 Redocly 2.72.1 lint 和 AsyncAPI parser 3.6.6 在主目录通过；已集成版本 Redocly 有四条 warning（两个组合判别字段、两个未用组件，其中 cursor 由 WS 跨文件引用），AsyncAPI 有版本/未用组件等 information，无 error。最后两条组合 warning 的修正仍按上文待提交状态记录。
- `./scripts/check-toolchain.sh` 在前置检查明确失败：缺少 `clang-tidy`。不据此宣称 full、固定依赖环境、真实 PG/JWT 或生产链路通过。
- `git diff --check` 通过；受保护 `doc/18-architecture-baseline.md` 无改动。协调与进度文档的链接检查随最终记录运行。

当前只交付首波 P00/P01 基础。首轮接口与协议/API 独立验收已通过，新增 target 方法/字段/频道仍登记未实现。P00 全环境验收与 P01 完整门禁未关闭；六进程构建、数据库及后续业务阶段未实施。下一波先补齐固定依赖环境、完整质量门禁，再推进 P02 六入口/分层构建，基于已集成提交下发；不能仅因协议可生成就关闭阶段。

文档影响：主协调更新本记录、`doc/23-implementation-plan.md` 的实际进度；按 P00 要求给 `doc/00-boundaries.md`、`doc/12-ci-cd.md` 追加目标实施约束，并在 `doc/16-development-tasks.md` 仅登记新计划，保留旧执行历史。模块自身已在对应独立提交同步文档。主协调不改变协议、运行行为或已确认架构，无需额外改写其他模块文档；保护基线只读。链接与 diff 检查不替代模块测试。
