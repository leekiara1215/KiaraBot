# 需要你确认 / 手动操作的事项

> 生成时间：2026-10-31；对应 main `90e73d3`（十模块 W1–W4 全部合并后）。
> 本文件只列**我无法替你做**或**需要人类维护者裁定**的事项；代码侧已完成的内容见 [`execution/final-summary.md`](execution/final-summary.md)。

## 1. 必须由你手动执行（否则不能宣称"已验证"）

| # | 事项 | 原因 | 操作步骤 | 完成后的结果 |
| --- | --- | --- | --- | --- |
| 1 | **GitHub Actions 手动跑一次** | 自动触发自 2026-10-30 起被注释，只有 `workflow_dispatch`；我无 GitHub token，无法触发或读取 Actions 结果 | 打开仓库 Actions → `CI` → `Run workflow`，分支选 `main`；`base_ref` 填 `origin/main`（可选 `baseline_release` 填上一发布版 tag） | 若全绿，才可恢复 `pull_request`/`push` 触发；若失败，把失败 job 日志给我，我继续修 |
| 2 | **恢复自动触发** | 见 `doc/12-ci-cd.md`「恢复自动触发」；当前 main 已包含 full-build/矩阵/审计扩展，但从未在远端跑过 | 手动全绿后：取消 `ci.yml` 顶部 `pull_request`、`push: branches: [main]` 注释；删除 `inputs.base_ref` 与 `BASE_SHA` 里的 `\|\| inputs.base_ref` 兜底 | PR/push 重新自动跑 CI |
| 3 | **Docker / 部署实机验收（T11b）** | 本机没有 Docker；Dockerfile、compose、adapter/panel 镜像、host 网络、迁移/备份/回滚只能静态改，无法构建或起栈 | 安装 Docker Desktop（或 Linux + Docker Engine ≥ 带 Compose v2.24+），按 `doc/08-operations.md` 的 W4 章节执行：`docker compose -f deploy/compose.staging.yml config` → `up` → 迁移/健康/冒烟 → 备份恢复演练 | T11b 可勾选；否则只能写"脚本与配置已就绪、未实机验证" |
| 4 | **真实 QQ/NapCat/模型联调（T07b）** | 需要隔离 QQ/NapCat、模型服务与目标会话配置（你此前确认暂不准备） | 按 `tests/acceptance/real-environment.md` 准备凭据/环境后运行真实链路 | 真实收发/错误分类/限流行为才能验收；此前 all fake |
| 5 | **分支保护 / required checks** | 仓库文件无法表达 GitHub 分支保护 | 在仓库 Settings → Branches 给 `main` 勾选 CI 的必需 job（至少 build-test、contracts、full-build、format、clang-tidy、api-contracts、dependency-scan、secret-scan） | 不兼容变更无法绕过合并 |
| 6 | **清理临时工作树（可选）** | 为绕开会话写权限限制，W2/W3/W4 使用了 `/tmp/kb-*-w2/w3/w4`；主仓库 `build/` 也有本机构建产物 | `git -C /Users/leekiara/Workspace/KiaraBot worktree list` 后对 `/tmp/kb-*` 执行 `worktree remove`；`build/` 可删（已在 .gitignore） | 释放磁盘；不影响仓库 |

## 2. 需要人类维护者裁定（AI 只报告，不修改）

1. **`doc/18` 受保护基线与实现的差异**（按规则只报告，不编辑）：
   - 附录 A 仍写"仓库没有 `adapters/`、适配器暂存同级检出"，实际适配器已在 `adapters/nonebot/`；
   - 基线第 8–9 层（C++ 适配器、C ABI 插件、C++/TS SDK）仍为"未来"，用户已明确延期（T12）；
   - 基线第 7.1 节"控制面板至少需要管理 11 项"：W4 已补插件/会话/Action 管理，但机器人账号、模型 API Key、群白名单、用户角色写接口仍无 OpenAPI 端点（契约侧缺口），是否收窄基线或补契约由你决定。
2. **契约/架构决策**（会影响后续实现，当前按"不伪造能力"处理）：
   - `messages` 频道：保留并从契约删除/标记 deprecated，还是排期补 `message.read` 授予 + 投影 + 生产者（当前契约已标 `not_implemented`，任何角色订阅 4403）；
   - Action 7 类：只做纯文本 `send_message` 作为 v1 边界，还是排期 Mute/Kick/Approve 等（需同时补权限门禁与速率策略，见 `doc/17`）；
   - 会话 ID：W3 已统一为"平台原生数字 ID + type"，但 Core 的 session key 仅用 conversation_id，群/私聊数字 ID 撞号时会共享排序键；是否给 Core/契约增加会话类型维度；
   - 命令幂等：当前按"数量上限确定性淘汰"（core W2），时间型过期需要快照格式版本升级，是否排期；
   - 多插件/多适配器装配、完整 RBAC、`message.send` 速率/内容策略：本轮明确未做，是否作为下一轮 P1；
   - FileStorage：当前"已实现未接线"，是接线还是明确废弃；
   - ACK 超时默认 30s（`KIARABOT_ACK_TIMEOUT_MS`）是否接受（长回调插件会被重投一次）。
3. **W3 传输层语义登记**：control-api 新增了 405/OPTIONS/HEAD/CORS/429/trace header/WS 1013，contracts 侧尚未全部登记进 `api/openapi.yaml`/`api/asyncapi.yaml`；是否要求先登记再视为稳定契约。
4. **W4 部署模型**：为兼容 Core/控制面/适配器"只绑回环"的安全约束，compose 改为 Linux host 网络（Docker Desktop 默认不支持）；是否接受单机 host 网络作为 v1 部署形态，还是改为在 Core 增加受控监听地址（涉及 `doc/06`/`doc/08` 安全语义）。

## 3. 本机与 CI 环境差异（需要在 CI 上复跑确认）

| 项 | 本机 | CI | 影响 |
| --- | --- | --- | --- |
| Python | uv 0.13.5 无法下载 CPython **3.11.18**，实际用 **3.11.17** | workflow 钉 3.11.18 | SDK/adapter/control 的 Python 验收需 CI 复跑确认；本地结果不能替代 |
| clang-format | Homebrew **23.1.4** | **16** | 只保证改动文件在 23 下通过；CI format job 可能对既有文件/差异有不同结果 |
| protobuf / gRPC | Homebrew 36.2 / 1.84.1 | vcpkg **29.3 / 1.71.0** | 本机 proto 构建/CTest 通过，但不能替代 CI 固定版本 |
| clang-tidy | 未安装（brew llvm@16 因 Homebrew post-install bug 安装失败） | clang-tidy-16 | 本机未跑静态分析；CI 的 full-build job 会跑全模块 tidy |
| Docker | 无 | CI 无 docker job（W4 交接建议增加） | Dockerfile/compose 全部未实机验证 |
| GitHub Actions | 无法触发/查询 | 手动触发可用 | 所有"CI 已绿"结论都不成立，直到你手动 Run |

## 4. 各模块"未验证"清单（汇总）

- **contracts**：旧 reader 线兼容测试默认关闭（本机未建 SDK venv）；发布版 descriptor 基线不存在（已支持 `baseline_release` 但无 release 可测）。
- **toolchain**：完整 `scripts/check-toolchain.sh` 未跑（缺 clang-tidy）；`check-format` 未作为门禁（版本差异）。
- **core**：`KIARABOT_*` 6 个新环境变量只在单元/进程测试中验证，未在真实部署组合下验证；维护循环的性能影响未压测。
- **infrastructure**：崩溃恢复用 SIGKILL 模拟，不等价断电；容量上限未接监控告警；`migrate` 锁重试依赖时序。
- **adapter**：真实 NapCat/QQ 未联调（T07b）；`tests/acceptance/run.sh` 全量入口未跑；备份只恢复一侧的 `out_of_order` 问题未修复（仅文档约定）。
- **plugin-sdk**：示例插件未做"真实 Core + 新插件"端到端；插件发现/多实例、日志/健康通道、会话、模型流、权限查询仍是交接项。
- **control-api**：一次全量 pytest 出现 1 个时序偶发失败（单测复跑通过），未做长期稳定性验证；限流默认值未按生产负载校准；TLS 未交付。
- **control-panel**：live 测试无本机 C++/venv 前置时 skip（CI 恢复后应跑 19 项）；axe/i18n/服务端过滤未做；视觉未做设计走查。
- **operations**：docker 全链路未跑；host 网络仅 Linux；`EXPECTED_SCHEMA_VERSION` 需随 schema 变更手动同步；备份/恢复/回滚演练未做。
- **ci-cd**：自动触发暂停；full-build 从未在远端执行；本机只做了 YAML 解析与 actionlint。

## 5. 合并与推送现状

- 十个模块分支的工作均已合并到 `main`（最终 `90e73d3` 及后续文档提交），并已 push 到 `origin/main`。
- 各模块远端分支保留其提交；部分分支落后 main（main 含其他模块合并），可按需 `git merge main` 或重置。
- 合并过程中 `doc/16-development-tasks.md` 出现多次"两边都追加记录"的冲突，均已保留双方记录解决，未丢失内容。
