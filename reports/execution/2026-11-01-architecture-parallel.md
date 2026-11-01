# 新架构并行开发协调记录（2026-11-01）

## 授权与统一基点

用户要求复用 `/Users/leekiara/Workspace/KiaraBot.worktrees/`，新开多个对话并行开发，由当前主对话最后统一整理，每个开发独立提交，不能最后一次性提交全部改动。

- 主集成目录：`/Users/leekiara/Workspace/KiaraBot`
- 主集成分支：`codex/architecture-rollout`
- 已确认设计独立提交：`66d0996`（doc/19–22 与裁定清单）
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

此记录建立时对话尚待启动；创建成功后主对话记录真实 threadId、分支、基点和检查结果，不用预设状态冒充启动或完成。阶段状态依实际验证更新，尚不宣称 P00–P14 完成。
