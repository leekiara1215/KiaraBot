# 分支独立修复：提示词模板 + 10 份可直接使用的分支提示词

> 用途：在已有的 10 个模块 worktree 中，各开一个会话/代码代理，按本文件逐分支执行，避免互相踩文件、重复计数和契约冲突。
> 依据：`reports/module-coverage-analysis.md`（总评估）+ `reports/modules/*.md`（每个模块的缺口证据与未核实项）。

---

## 0. 使用方式（先看这一节，能省掉 80% 的冲突）

### 0.1 工作目录

每个分支已有独立 worktree，**必须在对应 worktree 里工作**，不要在主仓库 `KiaraBot` 里切换分支：

```text
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-contracts
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-core-runtime
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-plugin-sdk
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-nonebot-adapter
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-infrastructure
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-control-api
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-control-panel
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-operations
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-ci-cd
```

开始每个会话前先确认：

```bash
git -C /Users/leekiara/Workspace/KiaraBot.worktrees/<branch> status --short --branch
# 应为 clean，且 branch 名与目录一致
```

### 0.2 波次顺序（有依赖，不要 10 个同时开工）

```text
第 1 波（解锁其它分支）：contracts、toolchain、ci-cd
第 2 波（核心与存储）：core-runtime、infrastructure
第 3 波（接入与展示）：nonebot-adapter、plugin-sdk、control-api
第 4 波（界面与部署）：control-panel、operations
```

- contracts 先定"会话 ID、权限词表、Action/事件子集、频道状态"这些**跨模块语义**，后面的分支才有依据。
- toolchain/ci-cd 先动，是为了让后续 8 个分支有可用的门禁与（恢复后的）CI 反馈。
- 允许同波并行，但同一波内**共享文件独占**（见 0.3）。
- 每个分支合并前先 `git rebase main`（或合并最新 main），再跑本模块验证。

### 0.3 共享文件独占表（10 个分支并行时最容易冲突的地方）

| 文件/目录 | 唯一允许修改的分支 | 其他分支怎么办 |
| --- | --- | --- |
| `proto/`、`api/` | contracts | 需要契约变更 → 写"交接项"，不改 |
| `.github/workflows/`、`scripts/ci/` | ci-cd | 只写交接项 |
| `scripts/quality/`、`scripts/check-toolchain.sh`、`cmake/`、根 `CMakeLists.txt`、`CMakePresets.json`、`.clang-format` | toolchain | 只写交接项 |
| `tests/acceptance/` | nonebot-adapter | ci-cd 只能改 CI 里的调用参数，不改 run.sh |
| `Dockerfile`、`deploy/`、`scripts/deploy/` | operations | 只写交接项 |
| `README.md`、`doc/16-development-tasks.md`、`doc/README.md` | **集成分支**（最后合并时统一改） | 分支内不改；在交付说明列"需要的文档变更" |
| `doc/18-architecture-baseline.md` | 人类维护者 | **任何分支都不得修改**（AGENTS.md 受保护文档） |
| 各模块 `doc/0X-*.md` | 对应模块分支 | 其他分支不改 |
| 其他模块目录（`core/`、`adapters/`、`panel/` …） | 对应模块分支 | 只写交接项，不直接改 |

### 0.4 三条铁律（写进每个提示词）

1. **契约先行，不越界**：需要改 `proto/`、`api/` 或别的模块目录时，不改，写"交接项"（目标分支/原因/建议改法/本分支的临时行为）。
2. **不谎报验证**：本机没有 cmake/node/uv/protoc/docker。跑不了的检查必须写"未运行 + 原因 + 风险 + 应在哪跑"，**不得写成已通过**。
3. **不顺手改**：不做无关格式化、不重构无关代码、不提交生成物/构建产物、不改 `doc/18`、不 `push`/不 `force push`（是否 commit 由你决定，默认建议先改文件、你审阅后再提交）。

### 0.5 每个分支的交付物（统一格式）

```text
1. 目标与范围：改了什么、为什么
2. 修改文件清单（逐文件一句话）
3. 行为/契约变化：输入输出、错误语义、配置、超时、幂等
4. 文档影响判断：已更新哪些、无需更新哪些及理由（AGENTS.md 强制）
5. 实际运行的检查与结果；未运行项及原因、建议命令
6. 跨模块交接项（目标分支/原因/建议改法/我方临时行为）
7. 未完成、未核实、已知风险
```

---

## 1. 通用提示词模板（填占位符即可）

```text
你是 KiaraBot 仓库 <branch> 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/<branch>（git 分支 <branch>，HEAD 8caecb1）。
不要在主仓库 /Users/leekiara/Workspace/KiaraBot 或其他 worktree 中修改文件。

【第一步：只读准备（先用一轮完成，不改代码）】
1. 读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md；
2. 读 <对应模块文档 doc/0X-*.md>；doc/18-architecture-baseline.md 只读，禁止修改；
3. 读 reports/modules/<name>.md 的 §4 缺口清单、§5 文档不一致、§8 未核实（本任务的问题来源与证据）；
4. git status 确认工作区干净、确认当前分支；
5. 输出一份"执行前影响判断"：要改的文件、会影响的公共接口/错误语义/配置、需要同步的文档、需要其他分支配合的前置。

【范围】
允许修改：<dirs/files>
禁止修改：其他模块目录、proto/ 与 api/（除非本分支是 contracts）、doc/18、生成物与 build/、
          .github/ 与 scripts/ci/（除非本分支是 ci-cd）、根构建与 scripts/quality/（除非本分支是 toolchain）。
若确实需要越界：不要改，改为"交接项"（目标分支、原因、建议改法、本分支的临时兼容/失败行为）。

【本分支待办（按优先级，逐条给证据与验收）】
P0：
- <任务>（证据：<file:line>；验收：<如何证明>）
P1：
- ...

【验证】
必须运行（能跑则跑）：<commands>
当前环境缺 <tools>，若无法运行：在交付中列"未运行检查 + 原因 + 风险 + 应执行命令"。
禁止把未运行写成通过。

【交付】
按 §0.5 的 7 项格式输出。
```

---

## 2. 十份分支提示词（可直接复制）

### 2.1 `kiarabot-contracts`（第 1 波 · 契约先行）

```text
你是 KiaraBot contracts 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-contracts（分支 kiarabot-contracts，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/01-contracts.md、
doc/01-runtime-semantics.md、proto/README.md、api/openapi.yaml、api/asyncapi.yaml；
读 reports/modules/contracts.md（尤其 §4.1/§4.5/§4.6/§4.7 与 §8）和 reports/module-coverage-analysis.md 的 §0.3。
doc/18 只读，禁止修改。

【范围】
允许：proto/、proto/tests/、api/、doc/01-*.md、proto/README.md、doc/16 中 contracts 相关条目。
禁止：任何实现代码（core/、adapters/、sdk/、control/、infrastructure/、panel/、deploy/、.github/、scripts/ci/）。
需要实现配合的，写成交接项。

【待办（按优先级）】
P0-1 让兼容门禁"可执行且有判别力"，不要只空转：
  - check_enum_consistency.py 覆盖全部 9 个枚举（现只 4 个）且比较数值，不只名称；
  - 接线目前无人调用的 proto/tests/descriptor_compatibility_test.sh（6 个负例）和 proto/tests/action_contract_test.py（旧 reader）；
  - 在 proto/tests 增加"契约声明 → 实现状态"清单检查（对每个 payload/字段/频道标注 implemented / not_implemented / deprecated，
    未实现的不得被文档暗示为可用）；清单本身要能被 CI 校验，不需要访问实现代码。
P0-2 收敛"契约宣称 ≥ 实现"的失真（只改契约/测试/文档，不改实现）：
  - api/openapi.yaml 的 PluginManifest runtime/isolation 枚举 vs core 实现只支持 python-rpc/process；
  - api/asyncapi.yaml 的 messages 频道（无 message.read 授予路径、无投影）→ 补授权模型设计或标 deprecated；
  - admin '*' 与 "admin 也不默认 message.read" 的矛盾；
  - REST Action 查询的保留期/EXPIRED 语义与 ActionRouter::lookup 的差异，写进契约测试。
P1：补契约缺项：权限/能力词表、reserved/deprecated 策略、优雅停机/drain 语义、ID 语法统一（长度与字符集）、
    Action deadline 上限、gRPC 错误映射与 in-band Error 的实际差异。

【验证】
可以做：python3 proto/tests/check_enum_consistency.py、python3 scripts/quality/check-boundaries.py、
  ruby proto/tests/api_contract_test.rb（需要 build/proto 与 Core 策略产物，跑不了就说明）。
不能做：protoc 生成/descriptor diff、redocly、asyncapi parser（本机无工具）→ 写明未运行与风险。

【交付】按通用格式 7 项。额外必须给"跨模块语义决定清单"：会话 ID 方案、权限词表、频道启停、Action/事件子集，
供 core/adapter/plugin-sdk/control-api/panel 分支照此实现。
```

### 2.2 `kiarabot-core-runtime`（第 2 波）

```text
你是 KiaraBot core-runtime 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-core-runtime（分支 kiarabot-core-runtime，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/02-core.md、
doc/01-runtime-semantics.md；读 reports/modules/core-runtime.md 的 §3/§4/§4.1/§5/§8。
doc/18 只读。先确认 contracts 分支是否已给出会话 ID/权限词表决定；若没有，本分支不得自行改契约。

【范围】
允许：core/（include/src/app/tests）、doc/02-core.md、doc/16 中 core 相关条目。
禁止：proto/、api/、adapters/、sdk/、panel/、control/、infrastructure/、部署与 CI 文件。

【待办（按优先级）】
P0 运行期无界增长导致写路径死亡（必须先做）：
  - 生产装配周期调用 purge_expired / 审计裁剪 / 命令幂等清理（现在只有测试调用）；
  - 把 Action/审计/任务/receipts/namespace/会话等硬编码上限变成可配置（RuntimeOptions/配置读取），
    并让"达到上限"的拒绝语义稳定、可观测；
  - 补测试：长期运行、容量打满、清理后恢复、64 MiB 快照与 10 万审计边界的失败语义。
P1 并发与生命周期：
  - 同一 RuntimeStore 被 6 个服务共享，仅私有锁；要么加全局串行写（单写者/串行装饰器），
    要么把冲突重试策略统一（ACK/Action 结果/拒绝帧路径也要重试，不只 next）；
  - 插件 ACK 超时（挂起不 ACK 不得永久阻塞该插件，需定义期限与失败/重投语义）；
  - 把 recover_inflight_events / dispatchable_events / expire_actions / update_plugin_recovery /
    dead-letter 重放接到生产装配（现在 --check 之外无人调用）；
  - 组合根支持多适配器/多插件（配置文件或重复 env，而不是单组 KIARABOT_PLUGIN_*）；
  - 权限模型扩展到资源范围 + 出站速率/配额（doc/17 的 P0 需求），危险 Action 默认拒绝；
  - 修 control/ 侧用到的 stream_snapshot 未判空 find_if（如属 control/ 目录则写交接）；
  - /groups 有策略无实现：实现或明确返回错误码并写交接给 contracts/control-api。

【验证】
cmake --preset dev && cmake --build --preset dev && ctest --preset dev --no-tests=error；
cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON && cmake --build --preset proto && ctest --preset proto --no-tests=error。
本机缺 cmake/protoc → 明确写未运行。禁止声称通过。

【交付】按通用格式 7 项；额外给出"契约/装配前置交接"（多插件配置格式、权限词表、ACK 超时契约）。
```

### 2.3 `kiarabot-plugin-sdk`（第 3 波）

```text
你是 KiaraBot plugin-sdk 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-plugin-sdk（分支 kiarabot-plugin-sdk，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/03-plugin-sdk.md、
sdk/PLUGIN_SDK_DEPENDENCIES.md、sdk/python/README.md、tests/acceptance/core.env.example；
读 reports/modules/plugin-sdk.md 的 §3/§4/§5/§8。doc/18 只读。

【范围】
允许：sdk/、plugins/、doc/03-plugin-sdk.md、sdk 内 README/依赖说明、doc/16 中 plugin 相关条目。
禁止：core/、proto/、api/、adapters/、panel/。需要 Core/契约改动的写交接。

【待办（按优先级）】
P1-1 补齐两个基线示例插件（只用现有能力，不新增协议）：
  - plugins/group_admin：群管理命令（基于现有 event.message_received + send_message 纯文本 + 权限），
    明确它不实现 Mute/Kick/Approve（等 Core Action 扩展）；
  - plugins/reminder：基于现有 task.create/get/cancel + TimerTriggered 事件；写清单/manifest + 测试 + 配置样例。
P1-2 修文档漂移（本分支负责）：doc/03 的清单示例（非空 config_schema + 未支持 capability 当前无法注册）、
  权限词表与 Core 实际不一致、FakeEvent/FakeMessageService 等承诺未实现、sdk/PLUGIN_SDK_DEPENDENCIES.md
  仍写 T06b/T06c 待接线（已实现）。
P1-3 SDK 侧可独立完成的健壮性：
  - 本地 manifest 校验与 Core 对齐（ordered/capabilities/version/entrypoint），错误信息包含稳定码；
  - 运行时参数可配置（hook_timeout/io_timeout/heartbeat/retry，环境变量或 CLI），README 的"回调超时应大于模型调用"才可执行；
  - 补测试：cancel_action、storage expires_at、task local_time/DST、非法 manifest、心跳超时、队列满 RATE_LIMITED、
    query_action 双选/均缺、FakeHost 覆盖。
P1-4 需要跨模块的（写交接，不改）：插件发现/多实例运行时（core）、SDK 日志帧与健康通道（proto+core）、
  会话管理 ConversationService（proto）、模型流（proto）、权限查询（proto）。

【验证】
uv sync --project sdk/python --locked --python 3.11.18（无 uv → 未运行）；
sdk/python/.venv/bin/python -m pytest -q sdk/python/tests（无环境 → 未运行）；
示例插件用 sdk/python/tests 或 tests/acceptance 的 fake host 验证。禁止声称通过。

【交付】按通用格式 7 项；额外给"插件能力矩阵"：现有协议下能做到/做不到，以及需要哪些交接。
```

### 2.4 `kiarabot-nonebot-adapter`（第 3 波 · 依赖 contracts 的 ID 决定）

```text
你是 KiaraBot nonebot-adapter 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-nonebot-adapter（分支 kiarabot-nonebot-adapter，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/04-nonebot-adapter.md、
doc/01-runtime-semantics.md 的背压/ACK 段；读 reports/modules/nonebot-adapter.md 的 §3/§4/§5/§6/§8；
读 reports/modules/contracts.md 中会话 ID 与 RATE_LIMITED 结论，以及 reports/verification.md 的修正。
doc/18 只读。

【范围】
允许：adapters/nonebot/、tests/acceptance/、doc/04-nonebot-adapter.md、adapters/nonebot/README.md。
禁止：proto/、api/、core/、sdk/、panel/、deploy/。需要 Core/契约/运维配合的写交接。

【待办（按优先级）】
P0-1 统一会话 ID（先按 contracts 分支的决定实现适配器侧）：
  - 事件 mapper 与 Action 发送使用同一套 ID 方案（推荐：ConversationRef.id = 平台原生数字 ID，用 type 区分群/私聊；
    若 contracts 决定保留前缀，则适配器发送侧做等价映射）；
  - 修 actions.py 的纯数字/长度/字符集校验与 target.conversation_id 一致性；
  - 加真实形态回归：群/私聊、事件 → 插件 → Action 回发，用与事件完全相同的 conversation 值（覆盖 ai-reply 用法）。
P0-2 背压不丢事件：
  - 对 RATE_LIMITED/可重试错误保留 pending 并退避重投（不标 failed）；
  - 与 Core 的 retryable 语义对齐（Core 修复属交接；适配器侧先按"RATE_LIMITED 一律可重试"处理）；
  - 测试：队列满、断线重投、重复事件、内容冲突。
P0-3 健康与状态：
  - 本地 HTTP 健康服务（8081 /health/live、/health/ready，compose 会用）；
  - 上报 AdapterStatusChanged（连接/断开/登录态失效/能力），并处理注册响应里的 heartbeat_interval_seconds；
  - 提供可被运维/控制面消费的状态输出（日志或本地接口），写清与 compose/panel 的字段约定。
P1：notice/request/meta 事件映射；更多消息段（image-file/face/voice/video/file/location/forward）与未知段降级；
  outbox 容量与 7 天保留冲突（已确认行不应占满 10000）、failed 清理、错误分类（限频/验证/群不存在/被踢）、
  发送侧限速；把 T07b 的人工 runbook 变成可执行准备脚本（不要求真实环境）。
需要 Core 的：Action 其余 6 类（等 core 放开）、事件五类生产者（部分属本分支可加，另一些属 Core）。

【验证】
无 uv/venv（Python 3.9）→ pytest 未运行，写明；adaptr 测试需 CORE_TEST_BINARY 的用例单独列出；
tests/acceptance/run.sh 需要 sdk/control 环境与 proto 构建，未运行要说明。

【交付】按通用格式 7 项；额外给"适配器 ↔ Core/契约/运维 交接清单"（ID 方案、retryable、健康字段、compose 变量）。
```

### 2.5 `kiarabot-infrastructure`（第 2 波）

```text
你是 KiaraBot infrastructure 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-infrastructure（分支 kiarabot-infrastructure，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/05-infrastructure.md、
doc/08-operations.md 的备份/迁移段；读 reports/modules/infrastructure.md 的 §4/§4 补充/§5/§8。
doc/18 只读。

【范围】
允许：infrastructure/（include/src/tests）、doc/05-infrastructure.md、doc/16 中 infra 相关条目。
禁止：core/（含 core/ports，属 core 分支）、proto/、api/、deploy/。端口增删必须写交接给 core 分支。

【待办（按优先级，本模块无 P0，从 P1 开始）】
P1-1 端口层失真处置（只在本分支能做的部分）：
  - 对 IUnitOfWork/INamespaceStore/IIdempotencyStore/IAuditLogger 等死接口：在 doc/05 标注"未实现/不使用"，
    或补最小实现 + 接线；删除/改接口需 core 分支批准（交接）；
  - FileStorage：要么接入某个真实使用方（core 交接），要么在 doc/05 明确"未接线，不承诺能力"；
  - 修 file_storage 的键长（1–256 vs NAME_MAX 255）、目录校验、get 大小上限、临时文件残留。
P1-2 可靠性证据：
  - 64 MiB / 1,000,000 项上限：补测试并定义超限错误语义与告警字段；
  - 崩溃/半写恢复：加 kill -9 / 断电模拟测试（SQLite integrity_check、重开行为）；
  - migrate CLI 进程级测试（重复迁移、锁冲突恢复、坏 schema 退出码、并发实例）。
P1-3 模型/时区/日志：
  - http_model_provider_test 要能在有工具链的环境跑（当前从未执行）；补 TLS/连接中断/429 retryable 断言；
  - 时区：启动预检（zoneinfo 缺失时 fail fast 且错误码区别于参数错误）、可选缓存；
  - 日志 sink：字段与 doc/08 通用规范对齐（timestamp/event_id/action_id/message/error_code），
    明确轮转/持久化归属（本模块或运维，写交接）。
不需要做（已声明延期/由 Core 承担）：Postgres/Redis/CronScheduler。

【验证】
cmake -S infrastructure -B build/infrastructure && cmake --build build/infrastructure &&
ctest --test-dir build/infrastructure --output-on-failure。
本机缺 cmake/protoc → 未运行要写明。

【交付】按通用格式 7 项；额外给"端口 ↔ 实现 ↔ 使用方"对照表（哪些可用、哪些只是声明）。
```

### 2.6 `kiarabot-control-api`（第 3 波）

```text
你是 KiaraBot control-api 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-control-api（分支 kiarabot-control-api，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/06-control-api.md、
control/api/README.md、api/openapi.yaml、api/asyncapi.yaml；
读 reports/modules/control-api.md 的 §3/§4/§5/§8 和 reports/modules/contracts.md 的频道/权限结论。
doc/18 只读。

【范围】
允许：control/api/（include/src/tests）、doc/06-control-api.md、control/api/README.md、doc/16 中 control 相关条目。
禁止：core/、proto/、api/、panel/、deploy/。messages/health 事件、Action 查询数据属 core/contracts，写交接。

【待办（按优先级）】
P1-1 生产可用性面（本分支可独立完成）：
  - 通用限流（非仅登录）、每用户连接/流配额、CORS/OPTIONS、HEAD/405 语义、keep-alive；
  - 优雅关闭：停止接受新连接、排空在途请求、WS 发 close 帧（1001/1012/1013 且把 1013 写进契约交接）；
  - 访问日志与入站 trace 传播（接受 traceparent/X-Request-ID，成功响应回传 request_id）。
P1-2 事件流放大：把 100ms 全量轮询改为事件驱动或至少复用快照/断开即停，补负载测试
  （合法低权账号建满 1024 流不应拖垮 REST 与 Core）。
P1-3 测试补齐：health/messages 事件、504/503/容量型 429、1013/握手超时/权限变化关闭、CORS/OPTIONS/405、
  优雅关闭、并发同键写、64 KiB 边界。
P1-4 跨模块（写交接，不改）：messages 频道授权与投影（contracts/core）、health.updated 事件（core）、
  Action 查询生产者与保留期（复用 ActionRouter::lookup）、/groups 数据源（core）、/logs 与审计解耦（core/infra）。
P2：`/api/v1/health` 每次写事务改为只读探针或降低频率；插件 manifest 做 schema 校验；credentials CLI 不回显密码。

【验证】
uv sync --project control/api/tests --locked --python 3.11.18 && control/api/tests/.venv/bin/python -m pytest -q control/api/tests；
需要 build/proto 的 C++ 测试（CMake 目标）单独列。本机无 uv/cmake → 未运行要写明。

【交付】按通用格式 7 项；额外给"REST/WS 逐 path/频道覆盖表"（可用/部分/未接线/未实现）与本分支修复后的变化。
```

### 2.7 `kiarabot-control-panel`（第 4 波）

```text
你是 KiaraBot control-panel 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-control-panel（分支 kiarabot-control-panel，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/07-control-panel.md、
doc/06-control-api.md、api/openapi.yaml、api/asyncapi.yaml；
读 reports/modules/control-panel.md 的 §3/§4/§5/§8。doc/18 只读（其"至少需要管理"11 项是需求基准，但不得改该文件）。

【范围】
允许：panel/、doc/07-control-panel.md、doc/16 中 panel 相关条目。
禁止：core/、control/、proto/、api/、deploy/。需要 API 新增/修复的写交接（包 API 已就绪但恒 404 的情况要先确认 control-api 分支）。

【待办（按优先级）】
P1-1 把 API 已就绪的 7 个能力接进面板（只加 UI/客户端，不改服务端）：
  插件 validate / enable / disable / config GET / config PUT（expected_version）/ health、
  会话列表与撤销、Action 查询（Action 查询需 control-api 修生产者后才可用，先做 UI + 明确错误态）。
  每个操作要有加载/成功/失败/权限不足态，不引入新的业务规则。
P1-2 契约漂移检测真做：从生成的 `paths` 派生端点/方法表，或生成常量并在 CI 校验；
  已知事件白名单与 asyncapi 的 6 个 `*.updated` 对齐检查。
P1-3 测试可复现：tests/live.test.ts 的前置（build/proto 二进制 + control/api/tests/.venv）要么文档化，
  要么缺失时 skip 并给原因；mock 补分页 next_page_token、资源事件、4401/4403、429/5xx。
P2：修 4403 文案（实际不重试）、RESYNC 状态被覆盖、streamReady 断线不复位、错误分类与 request_id 展示、
  ErrorBoundary、page_size、日志页流式追加、i18n/a11y（可选，若超范围写"未完成"）。

【验证】
npm ci && npm run check:contracts && npm run lint && npm test && npm run build。
本机无 node/npm → 未运行要写明。

【交付】按通用格式 7 项；额外给"逐页管理动作前后对照表"（只读 → 可操作/仍只读）。
```

### 2.8 `kiarabot-operations`（第 4 波 · 依赖前几波的接口决定）

```text
你是 KiaraBot operations 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-operations（分支 kiarabot-operations，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/08-operations.md、
doc/12-ci-cd.md 部署段；读 reports/modules/operations.md 的 §3/§4/§5/§8 与
reports/verification.md 关于回环绑定的复核。doc/18 只读。

【范围】
允许：Dockerfile、.dockerignore、deploy/、scripts/deploy/、doc/08-operations.md、doc/16 中 ops 相关条目。
禁止：core/、control/、adapters/、panel/、proto/、api/、.github/、scripts/ci/。
Core 监听地址、适配器/面板健康接口、环境变量名属别的分支 → 写交接，并让 compose 使用"已约定的名字"。

【待办（按优先级）】
P0 让部署骨架从"不可运行"到"可解释地接近可运行"：
  1) Dockerfile：改用 proto + infrastructure 构建（KIARABOT_GENERATE_PROTO=ON、KIARABOT_ENABLE_INFRASTRUCTURE=ON），
     构建并复制 kiarabot-migrate，运行层装 curl/tzdata，非 root；确保 /health/live|ready 存在；
  2) compose：core 加 --grpc-listen、KIARABOT_RUNTIME_DB、控制配置挂载（0600）、对齐 HEALTH_PORT；
     迁移服务给出唯一权威 MIGRATION_COMMAND（kiarabot-migrate），失败即阻断；
  3) 网络：与 Core 分支约定的监听方案落地（受控监听地址 / host 网络 / 共享网络命名空间三选一），
     在此之前明确标注"跨容器不可用"；
  4) adapter/panel 镜像：要么本分支提供 Dockerfile（adapter 需要 console script 与 outbox 卷、panel 需要静态站点与反代），
     要么在 doc/08 明确"未交付"并从 compose 移除依赖，二选一并写清理由；
  5) 环境变量名对齐（CORE_GRPC_ENDPOINT/CORE_AUTH_TOKEN/ADAPTER_ID/... 与 VITE_* 构建期），不对齐的写交接。
P1：TLS 反代、日志 driver/轮转/集中收集、备份与 DB 分卷 + 保留/异地 + integrity_check + restore 守卫、
  部署记录（commit/image digest/schema/config 版本）、rollback 前 schema 兼容检查、升级窗口与 trap、
  systemd 加固（User/Restart/ProtectSystem）、部署后 live/ready/smoke。
验收不能只靠脚本单测：必须给出在隔离环境可执行的命令与预期输出；本机无 docker → 明确未验证。

【验证】
python3 scripts/deploy/tests/*.py（deployment/healthcheck 可在本机跑，先跑）；
ruby -e 'require "yaml"; YAML.load_file(...)' 校验 compose YAML；
docker build / docker compose config / up 本机不可用 → 未运行要写明。

【交付】按通用格式 7 项；额外给"compose 四服务依赖链：现在哪一步必失败 → 修复后预期"表。
```

### 2.9 `kiarabot-toolchain`（第 1 波）

```text
你是 KiaraBot toolchain 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain（分支 kiarabot-toolchain，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、doc/11-toolchain.md；
读 reports/modules/toolchain.md 的 §3/§4/§5/§8 和 reports/modules/ci-cd.md 的矩阵/tidy 结论。
doc/18 只读。

【范围】
允许：scripts/check-toolchain.sh、scripts/quality/（含 tests/）、cmake/、根 CMakeLists.txt、CMakePresets.json、
  .clang-format、doc/11-toolchain.md。
禁止：.github/、scripts/ci/（ci-cd 分支）、proto/、api/、各模块实现目录。CI 侧改动写交接。

【待办（按优先级）】
P1-1 依赖门禁前置化：把 rg（check-format 硬依赖）、python3、ruby 加入 require_command/版本表/文档；
  缺工具要在最前面失败，而不是跑完两次构建后才失败。
P1-2 统一"本地 vs CI"的文件集：format/tidy 的覆盖必须一致（CI 用 dev 编译库只含 core，本地用 proto 库含
  control/api+infrastructure）；建议提供两个显式模式（dev 快速 / full 全模块），CI 用 full。
P1-3 边界检查加覆盖：除 Core 直接 include 外，检查 Python（sdk/adapters）导入边界、面板不导入 Core、
  CMake target 链接方向、传递依赖（至少对违规 include 链做一跳）。
P1-4 其他：check-toolchain.sh 的 ctest 加 --no-tests=error；bash -n 覆盖 proto/tests 与 tests/acceptance；
  两套 validate-asyncapi.cjs 合并为一份；Python/TS/Shell 的 lint 接入（或明确不接入并写风险）；
  dev 预设是否默认编译 control/api 与 infrastructure（二选一并在文档说明）。
P2：CMake 依赖检查前移；离线/受限网络提示；单模块最小启动命令缺口说明。

【验证】
bash -n 全部脚本；python3 scripts/quality/tests/boundaries_test.py；python3 scripts/quality/check-boundaries.py；
check-format/tidy 需要 clang-format/tidy（本机无 → 未运行写明）。

【交付】按通用格式 7 项；额外给"检查项 × 覆盖模块"矩阵（现有 vs 修复后），以及需要 ci-cd 分支同步的条目。
```

### 2.10 `kiarabot-ci-cd`（第 1 波 · 先恢复反馈能力）

```text
你是 KiaraBot ci-cd 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-ci-cd（分支 kiarabot-ci-cd，HEAD 8caecb1）。

【只读准备】
读 AGENTS.md、doc/12-ci-cd.md、doc/11-toolchain.md、README.md 的「当前状态」、
reports/modules/ci-cd.md 的 §3/§4/§5/§8、reports/modules/contracts.md 的 G1/§4.5、reports/verification.md。
doc/18 只读。

【范围】
允许：.github/workflows/ci.yml、.github/workflows/ 下新增文件、scripts/ci/、doc/12-ci-cd.md、doc/16 中 ci 相关条目。
禁止：scripts/quality/、根 CMakeLists、proto/、api/、各模块实现。需要 toolchain 配合的写交接。

【待办（按优先级）】
P0-1 恢复自动触发（先保证手动全绿再恢复）：
  - 先修使 job 失败的具体问题；在交付里明确"手动运行 → 全绿 → 取消注释 pull_request/push"的步骤；
  - 恢复后删除 inputs.base_ref 与 BASE_SHA 的 fallback，PR 用真实 base.sha；
  - 若无法在本地验证 GitHub Actions，必须写明"未在远端验证"，不得声称已恢复。
P0-2 descriptor 基线可执行：上传当前 descriptor 作为后续基线（artifact 或 release），
  check-descriptor 从"上一版基线"取而不是 main/HEAD；保留 test-descriptor-gate 的负例。
P1-1 构建矩阵补全：Clang 16 也要构建 proto + infrastructure（现在只有 GCC13）；
  静态分析覆盖 control/api 与 infrastructure（用 proto 编译库或独立 job），与 toolchain 分支约定一致。
P1-2 依赖与缓存：vcpkg/uv/npm 缓存；依赖审计扩展到 panel、Python（uv.lock，可用 pip-audit 或 osv-scanner）；
  SBOM 若纳入范围则产物化。
P1-3 工程化：失败时上传测试日志/报告；job timeout；node/python 版本显式锁定；
  acceptance 失败时保留分阶段日志；避免把面板检查全部塞进 contracts job（拆独立 job）。
P2：macOS job（若承诺 AppleClang 支持）；定时依赖重扫；镜像构建/发布流水线（明确是否本分支范围，若否则写交接）。

【验证】
actionlint（本机可能无 go → 未运行）、python3 yaml.safe_load 校验 workflow；
npm ci --prefix scripts/ci --ignore-scripts（无 node → 未运行）；descriptor 脚本需要 protoc/构建产物。
禁止声称 CI 已绿。

【交付】按通用格式 7 项；额外给"job × 检查 × 覆盖模块"表、恢复自动触发的 checklist、需要 toolchain 分支配合的清单。
```

---

## 3. 合并与冲突处理

1. **按波次合并**：`contracts` → `core-runtime`/`infrastructure` → `adapter`/`plugin-sdk`/`control-api` → `panel`/`operations`。每波合并后跑一次本机可跑的验证。
2. **共享文档最后统一改**：`README.md`、`doc/16-development-tasks.md`、`doc/README.md` 由集成分支统一更新（各分支只改自己的模块文档与 doc/16 相关条目，减少冲突）。
3. **契约变更**：只从 contracts 分支出；其他分支若在等待契约，先按"临时兼容 + 交接项"实现，合并前 rebase 到含新契约的 main。
4. **合并前检查**：`git diff --stat` 确认只动自己范围；`git diff` 确认没有顺手格式化；`git status` 确认无生成物、无 `.venv`/`build/`；提交说明写清 P0/P1 编号。
5. **最终集成验证**（有工具链时）：
   ```bash
   ./scripts/check-toolchain.sh
   bash tests/acceptance/run.sh
   cd panel && npm ci && npm run check:contracts && npm run lint && npm test && npm run build
   ```

## 4. 常见坑（写提示词时反复强调）

- **不要 10 个分支同时改 `api/openapi.yaml`**：契约文件只有一个所有者（contracts），否则合并时必然冲突。
- **不要把"未运行"写成"通过"**：本机无工具链，这是最容易产生虚假结论的地方。
- **不要用"顺手修一下"跨模块**：所有跨模块需求都写成交接项，由对应分支在它自己的提示词里处理。
- **不要改 `doc/18`**：受保护文档，只报告差异，由人类维护者裁定。
- **不要以"文档说已实现"为准**：先找调用链/接线点；找不到就按 `未接线`/`未验证` 处理。
- **做完 P0 再碰 P1**：每个分支的 P0 都是会阻断主线或产生数据丢失/安全后果的问题。
- **不要扩大范围**：10 个分支各自收敛，比一个分支试图修全部更容易合并与验收。
