# 分支提示词（纯复制版）

> 每个代码块就是一份完整提示词，直接整块复制给你的分支代理。
> 使用前只改一件事：把代码块里的 worktree 路径/分支名与你实际环境核对（应与文件名一致）。
> 建议顺序：contracts / toolchain / ci-cd → core-runtime / infrastructure → nonebot-adapter / plugin-sdk / control-api → control-panel / operations。

---

## 1. contracts

```text
你是 KiaraBot contracts 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-contracts
（git 分支 kiarabot-contracts，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/01-contracts.md、doc/01-runtime-semantics.md、proto/README.md；读
reports/modules/contracts.md 的 §4/§4.5/§4.6/§4.7/§8；doc/18 只读，禁止修改。
先输出执行前影响判断：要改的契约文件、跨模块语义变化、需要同步的文档、其他分支要跟进的点。

范围：允许修改 proto/、proto/tests/、api/、doc/01-*.md、proto/README.md、doc/16 中 contracts 条目。
禁止修改任何实现代码（core/、adapters/、sdk/、control/、infrastructure/、panel/、deploy/、.github/、scripts/ci/）。
需要实现配合的，不要改，写"交接项"（目标分支/原因/建议改法/我方临时行为）。

待办（P0 先做，证据在 reports/modules/contracts.md）：
P0-1 让契约门禁有判别力，而不是空转：
  - proto/tests/check_enum_consistency.py 覆盖全部枚举（当前只 4/9）并比较数值，不只名称；
  - 把无人调用的 proto/tests/descriptor_compatibility_test.sh（6 个负例）和
    proto/tests/action_contract_test.py（旧 reader）接入可执行入口；
  - 在 proto/tests 增加"契约声明 → 实现状态"清单检查：每个 payload/字段/频道标注
    implemented / not_implemented / deprecated，未实现的不得被文档暗示为可用。
P0-2 收敛"契约宣称 ≥ 实现"的失真（只改契约/测试/文档）：
  - api/openapi.yaml 的 PluginManifest runtime/isolation 枚举与实际只支持 python-rpc/process 不符；
  - api/asyncapi.yaml 的 messages 频道：补授权模型设计，或在契约中明确标记不可用/deprecated；
  - admin '*' 与"admin 也不默认 message.read"的矛盾；
  - REST Action 查询的 7 天保留/EXPIRED 语义与 ActionRouter::lookup 的差异，写进契约测试断言。
P1：补契约缺项：权限/能力词表、reserved/deprecated 规则、优雅停机/drain、ID 语法统一
  （长度与字符集）、Action deadline 上限、gRPC 错误映射与 in-band Error 的实际差异。

验证：必跑 python3 proto/tests/check_enum_consistency.py、python3 scripts/quality/check-boundaries.py；
ruby proto/tests/api_contract_test.rb 需要 build/proto 产物，跑不了要写明。protoc/redocly/asyncapi parser
本机不可用，必须列"未运行 + 原因 + 风险"。禁止把未运行写成通过。
禁止顺手格式化、重构无关代码、提交生成物、修改 doc/18。

交付：1) 修改文件与理由；2) 契约/行为变化（输入输出、错误语义、兼容性）；
3) 文档影响判断（已更新/无需更新及理由）；4) 实际运行的检查与未运行项；
5) 跨模块交接项；6) "跨模块语义决定清单"（会话 ID 方案、权限词表、频道启停、Action/事件子集），
供 core/adapter/plugin-sdk/control-api/panel 分支照此实现；7) 未完成/未核实风险。
```

## 2. core-runtime

```text
你是 KiaraBot core-runtime 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-core-runtime
（git 分支 kiarabot-core-runtime，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/02-core.md、doc/01-runtime-semantics.md；读 reports/modules/core-runtime.md 的 §3/§4/§4.1/§5/§8；
doc/18 只读。先确认 contracts 分支是否已给出会话 ID/权限词表决定；没有则本分支不得自行改契约。
先输出执行前影响判断。

范围：允许修改 core/（include/src/app/tests）、doc/02-core.md、doc/16 中 core 条目。
禁止修改 proto/、api/、adapters/、sdk/、panel/、control/、infrastructure/、部署与 CI 文件；越界写交接项。

待办：
P0-1 运行期无界增长导致写路径死亡（必须先修）：
  - 生产装配周期调用 purge_expired、裁剪审计、清理命令幂等记录（现在只有测试调用）；
  - 把 Action/审计/任务/receipts/namespace/会话等硬编码上限改为可配置，并让达到上限的拒绝语义稳定可观测；
  - 补测试：长期运行、容量打满、清理后恢复、64 MiB 快照与 10 万审计边界的失败语义。
P1-1 并发与一致性：同一 RuntimeStore 被 6 个服务共享且只有私有锁；加全局串行写（单写者/串行装饰器），
  或统一冲突重试策略（ACK、Action 结果、拒绝帧路径都要重试，不只 next）。
P1-2 插件 ACK 超时：插件挂起不 ACK 不得永久阻塞该插件，定义期限与失败/重投语义并测试。
P1-3 生命周期接线：把 recover_inflight_events / dispatchable_events / expire_actions /
  update_plugin_recovery / dead-letter 重放接到生产装配（现在除 --check 外无人调用）。
P1-4 装配能力：组合根支持多适配器/多插件（配置文件或重复环境变量），不再是单组 KIARABOT_PLUGIN_*。
P1-5 权限与风控：权限扩展到资源范围 + 出站速率/配额（doc/17 需求），危险 Action 默认拒绝并审计。
P1-6 /groups 有策略无实现：实现，或返回明确错误码并写交接给 contracts/control-api；
  另修 stream_snapshot 未判空 find_if 的潜在越界（若在 control/ 目录则写交接）。

验证：cmake --preset dev && cmake --build --preset dev && ctest --preset dev --no-tests=error；
cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON && cmake --build --preset proto &&
ctest --preset proto --no-tests=error。本机缺 cmake/protoc 则写明未运行，禁止声称通过。

交付：1) 修改文件与理由；2) 行为/错误语义变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) 跨模块交接（权限词表、ACK 超时契约、多插件配置格式、健康事件）；6) 未完成/未核实风险。
```

## 3. plugin-sdk

```text
你是 KiaraBot plugin-sdk 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-plugin-sdk
（git 分支 kiarabot-plugin-sdk，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/03-plugin-sdk.md、sdk/PLUGIN_SDK_DEPENDENCIES.md、sdk/python/README.md、
tests/acceptance/core.env.example；读 reports/modules/plugin-sdk.md 的 §3/§4/§5/§8；doc/18 只读。
先输出执行前影响判断。

范围：允许修改 sdk/、plugins/、doc/03-plugin-sdk.md、sdk 内 README/依赖说明、doc/16 中 plugin 条目。
禁止修改 core/、proto/、api/、adapters/、panel/；需要 Core/契约改动的写交接项。

待办：
P1-1 补齐两个基线示例插件（只用现有能力，不新增协议）：
  - plugins/group_admin：群管理命令（message_received + send_message 纯文本 + 现有权限），
    明确不实现 Mute/Kick/Approve（等 Core Action 扩展）；
  - plugins/reminder：基于 task.create/get/cancel + TimerTriggered；含 manifest、配置样例、测试。
P1-2 修本模块文档漂移：doc/03 的清单示例（非空 config_schema + 未支持 capability 当前无法注册）、
  权限词表与 Core 实际不一致、FakeEvent/FakeMessageService 等承诺未实现、
  sdk/PLUGIN_SDK_DEPENDENCIES.md 仍写 T06b/T06c 待接线（实际已实现）。
P1-3 SDK 侧健壮性：本地 manifest 校验与 Core 对齐（ordered/capabilities/version/entrypoint，错误含稳定码）；
  运行时参数可配置（hook_timeout/io_timeout/heartbeat/retry，环境变量或 CLI），
  让 README 的"回调超时应大于模型调用"可执行；补测试（cancel_action、storage expires_at、
  task local_time/DST、非法 manifest、心跳超时、队列满 RATE_LIMITED、query_action 双选/均缺、FakeHost 覆盖）。
P1-4 跨模块（写交接项，不改）：插件发现/多实例（core）、SDK 日志帧与健康通道（proto+core）、
  会话管理 ConversationService（proto）、模型流（proto）、权限查询（proto）。

验证：uv sync --project sdk/python --locked --python 3.11.18；
sdk/python/.venv/bin/python -m pytest -q sdk/python/tests；示例插件用 fake host 验证。
本机无 uv/venv 则写明未运行，禁止声称通过。

交付：1) 修改文件与理由；2) 行为/错误语义变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) 交接项；6) "插件能力矩阵"（现有协议下能做到/做不到、需哪些交接）；7) 未完成/未核实风险。
```

## 4. nonebot-adapter

```text
你是 KiaraBot nonebot-adapter 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-nonebot-adapter
（git 分支 kiarabot-nonebot-adapter，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/04-nonebot-adapter.md、doc/01-runtime-semantics.md 的背压/ACK 段；
读 reports/modules/nonebot-adapter.md 的 §3/§4/§5/§6/§8、reports/modules/contracts.md 的会话 ID/背压结论、
reports/verification.md 的修正；doc/18 只读。先输出执行前影响判断。

范围：允许修改 adapters/nonebot/、tests/acceptance/、doc/04-nonebot-adapter.md、adapters/nonebot/README.md。
禁止修改 proto/、api/、core/、sdk/、panel/、deploy/；需要 Core/契约/运维配合的写交接项。

待办：
P0-1 统一会话 ID（先按 contracts 分支决定实现适配器侧）：事件 mapper 与 Action 发送使用同一套 ID 方案
  （推荐 ConversationRef.id = 平台原生数字 ID，用 type 区分群/私聊；若契约保留前缀则发送侧做等价映射）；
  修 actions.py 的纯数字/长度/字符集校验与 target.conversation_id 一致性；
  加真实形态回归：群/私聊、事件→插件→Action 回发，使用与事件完全相同的 conversation 值。
P0-2 背压不丢事件：对 RATE_LIMITED/可重试错误保留 pending 并退避重投（不标 failed）；
  与 Core 的 retryable 语义对齐（Core 修复写交接，适配器侧先按"RATE_LIMITED 一律可重试"处理）；
  测试队列满、断线重投、重复事件、内容冲突。
P0-3 健康与状态：提供本地 HTTP 健康服务（8081 /health/live、/health/ready）；
  上报 AdapterStatusChanged（连接/断开/登录态失效/能力），处理注册响应的 heartbeat_interval_seconds；
  输出可被运维/控制面消费的状态，写清与 compose/panel 的字段约定。
P1：notice/request/meta 事件映射；更多消息段（image-file/face/voice/video/file/location/forward）与未知段降级；
  outbox 容量与 7 天保留冲突（已确认行不应占满 10000）、failed 清理、错误分类（限频/验证/群不存在/被踢）、
  发送侧限速；把 T07b 的人工 runbook 变成可执行的准备脚本（不要求真实环境）。

验证：本机无 uv/venv（Python 3.9）→ pytest 未运行要写明；需 CORE_TEST_BINARY 的用例单独列出；
tests/acceptance/run.sh 需要 sdk/control 环境与 proto 构建，未运行要说明。

交付：1) 修改文件与理由；2) 行为/错误语义变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) "适配器 ↔ Core/契约/运维 交接清单"（ID 方案、retryable、健康字段、compose 变量）；
6) 未完成/未核实风险。
```

## 5. infrastructure

```text
你是 KiaraBot infrastructure 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-infrastructure
（git 分支 kiarabot-infrastructure，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/05-infrastructure.md、doc/08-operations.md 的备份/迁移段；
读 reports/modules/infrastructure.md 的 §4/补充/§5/§8；doc/18 只读。先输出执行前影响判断。

范围：允许修改 infrastructure/（include/src/tests）、doc/05-infrastructure.md、doc/16 中 infra 条目。
禁止修改 core/（含 core/ports，属 core 分支）、proto/、api/、deploy/；端口增删写交接给 core 分支。

待办（本模块无 P0，从 P1 开始）：
P1-1 端口层失真处置（只在本分支能做的部分）：对 IUnitOfWork/INamespaceStore/IIdempotencyStore/IAuditLogger
  等死接口，在 doc/05 标注"未实现/不使用"或补最小实现+接线；删除/改接口需 core 分支批准（交接）；
  FileStorage：接入真实使用方（core 交接）或明确"未接线、不承诺能力"；
  修 file_storage 的键长（1–256 vs NAME_MAX 255）、目录校验、get 大小上限、临时文件残留。
P1-2 可靠性证据：64 MiB / 1,000,000 项上限补测试并定义超限错误语义与告警字段；
  崩溃/半写恢复加 kill -9 / 断电模拟测试（SQLite integrity_check、重开行为）；
  migrate CLI 进程级测试（重复迁移、锁冲突恢复、坏 schema 退出码、并发实例）。
P1-3 模型/时区/日志：让 http_model_provider_test 能在有工具链环境运行（当前从未执行），
  补 TLS/连接中断/429 retryable 断言；时区启动预检（zoneinfo 缺失时 fail fast 且错误码区别于参数错误）、可选缓存；
  日志 sink 字段与 doc/08 通用规范对齐，明确轮转/持久化归属（写交接）。
不需要做（已声明延期/由 Core 承担）：Postgres/Redis/CronScheduler。

验证：cmake -S infrastructure -B build/infrastructure && cmake --build build/infrastructure &&
ctest --test-dir build/infrastructure --output-on-failure。本机缺 cmake/protoc 则写明未运行。

交付：1) 修改文件与理由；2) 行为/错误语义变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) "端口 ↔ 实现 ↔ 使用方"对照表；6) 未完成/未核实风险。
```

## 6. control-api

```text
你是 KiaraBot control-api 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-control-api
（git 分支 kiarabot-control-api，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/06-control-api.md、control/api/README.md、api/openapi.yaml、api/asyncapi.yaml；
读 reports/modules/control-api.md 的 §3/§4/§5/§8、reports/modules/contracts.md 的频道/权限结论；doc/18 只读。
先输出执行前影响判断。

范围：允许修改 control/api/（include/src/tests）、doc/06-control-api.md、control/api/README.md、
doc/16 中 control 条目。禁止修改 core/、proto/、api/、panel/、deploy/；messages/health 事件、
Action 查询数据、/groups 属 core/contracts，写交接项。

待办：
P1-1 生产可用性面（本分支可独立完成）：通用限流（非仅登录）、每用户连接/流配额、CORS/OPTIONS、
  HEAD/405 语义、keep-alive；优雅关闭（停止接受新连接、排空在途请求、WS 发 close 帧，
  并把 1013 写进契约交接）；访问日志与入站 trace 传播（接受 traceparent/X-Request-ID，成功响应回传 request_id）。
P1-2 事件流放大：把 100ms 全量轮询改为事件驱动或至少复用快照/断开即停，补负载测试
  （合法低权账号建满 1024 流不应拖垮 REST 与 Core）。
P1-3 测试补齐：health/messages 事件、504/503/容量型 429、1013/握手超时/权限变化关闭、
  CORS/OPTIONS/405、优雅关闭、并发同键写、64 KiB 边界。
P1-4 跨模块（写交接）：messages 频道授权与投影（contracts/core）、health.updated 事件（core）、
  Action 查询生产者与保留期（复用 ActionRouter::lookup）、/groups 数据源（core）、/logs 与审计解耦（core/infra）。
P2：/api/v1/health 每次写事务改为只读探针或降低频率；插件 manifest 做 schema 校验；credentials CLI 不回显密码。

验证：uv sync --project control/api/tests --locked --python 3.11.18 后
control/api/tests/.venv/bin/python -m pytest -q control/api/tests；需要 build/proto 的 C++ 测试单独列。
本机无 uv/cmake 则写明未运行，禁止声称通过。

交付：1) 修改文件与理由；2) 行为/错误语义变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) "REST/WS 逐 path/频道覆盖表"（可用/部分/未接线/未实现）与修复后变化；6) 交接项；
7) 未完成/未核实风险。
```

## 7. control-panel

```text
你是 KiaraBot control-panel 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-control-panel
（git 分支 kiarabot-control-panel，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/07-control-panel.md、doc/06-control-api.md、api/openapi.yaml、api/asyncapi.yaml；
读 reports/modules/control-panel.md 的 §3/§4/§5/§8；doc/18 只读（其"至少需要管理"11 项是需求基准，但不得修改）。
先输出执行前影响判断。

范围：允许修改 panel/、doc/07-control-panel.md、doc/16 中 panel 条目。
禁止修改 core/、control/、proto/、api/、deploy/；需要 API 新增/修复的写交接项
（Action 查询端点当前恒 404，需 control-api 先修，先做 UI + 明确错误态）。

待办：
P1-1 把 API 已就绪的 7 个能力接进面板（只加 UI/客户端，不改服务端）：插件 validate / enable / disable /
  config GET / config PUT（带 expected_version）/ health、会话列表与撤销、Action 查询（待 API 修复）。
  每个操作要有加载/成功/失败/权限不足态，不引入新业务规则。
P1-2 契约漂移检测真做：从生成的 paths 派生端点/方法表，或生成常量并在 CI 校验；
  已知事件白名单与 asyncapi 的 6 个 *.updated 做一致性检查。
P1-3 测试可复现：tests/live.test.ts 的前置（build/proto 二进制 + control/api/tests/.venv）要么文档化，
  要么缺失时 skip 并给原因；mock 补分页 next_page_token、资源事件、4401/4403、429/5xx。
P2：修 4403 文案（实际不重试）、RESYNC 状态被覆盖、streamReady 断线不复位、错误分类与 request_id 展示、
  ErrorBoundary、page_size、日志页流式追加、i18n/a11y（超范围就写未完成）。

验证：npm ci && npm run check:contracts && npm run lint && npm test && npm run build。
本机无 node/npm 则写明未运行，禁止声称通过。

交付：1) 修改文件与理由；2) 行为/错误语义变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) "逐页管理动作前后对照表"（只读 → 可操作/仍只读）；6) 交接项；7) 未完成/未核实风险。
```

## 8. operations

```text
你是 KiaraBot operations 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-operations
（git 分支 kiarabot-operations，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/08-operations.md、doc/12-ci-cd.md 部署段；读 reports/modules/operations.md 的 §3/§4/§5/§8、
reports/verification.md 关于回环绑定的复核；doc/18 只读。先输出执行前影响判断。

范围：允许修改 Dockerfile、.dockerignore、deploy/、scripts/deploy/、doc/08-operations.md、doc/16 中 ops 条目。
禁止修改 core/、control/、adapters/、panel/、proto/、api/、.github/、scripts/ci/；
Core 监听地址、适配器/面板健康接口、环境变量名属其他分支 → 写交接项，compose 使用已约定名字。

待办：
P0 让部署骨架从"不可运行"到"可解释地接近可运行"：
  1) Dockerfile 改用 proto + infrastructure 构建（KIARABOT_GENERATE_PROTO=ON、
     KIARABOT_ENABLE_INFRASTRUCTURE=ON），构建并复制 kiarabot-migrate，运行层装 curl/tzdata，非 root，
     确保 /health/live|ready 存在；
  2) compose：core 加 --grpc-listen、KIARABOT_RUNTIME_DB、控制配置挂载（0600）、对齐 HEALTH_PORT；
     迁移服务给出唯一权威 MIGRATION_COMMAND（kiarabot-migrate），失败即阻断；
  3) 网络：与 Core 分支约定的监听方案落地（受控监听地址 / host 网络 / 共享网络命名空间三选一），
     在此之前明确标注"跨容器不可用"；
  4) adapter/panel 镜像：要么本分支提供 Dockerfile（adapter 需 console script 与 outbox 卷、
     panel 需静态站点与反代），要么在 doc/08 明确"未交付"并从 compose 移除依赖，二选一并写理由；
  5) 环境变量名对齐（CORE_GRPC_ENDPOINT/CORE_AUTH_TOKEN/ADAPTER_ID/... 与 VITE_* 构建期），
     不对齐的写交接项。
P1：TLS 反代、日志 driver/轮转/集中收集、备份与 DB 分卷 + 保留/异地 + integrity_check + restore 守卫、
  部署记录（commit/image digest/schema/config 版本）、rollback 前 schema 兼容检查、升级窗口与 trap、
  systemd 加固（User/Restart/ProtectSystem）、部署后 live/ready/smoke。
验收不能只靠脚本单测：给出隔离环境可执行命令与预期输出；本机无 docker 则明确未验证。

验证：python3 scripts/deploy/tests/deployment_test.py、python3 scripts/deploy/tests/healthcheck_test.py（本机可跑，先跑）；
ruby -e 'require "yaml"; YAML.load_file("deploy/compose.staging.yml")' 校验 YAML；
docker build / docker compose config/up 不可用 → 写明未运行。

交付：1) 修改文件与理由；2) 行为/错误语义变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) "compose 四服务依赖链：现在哪一步必失败 → 修复后预期"表；6) 交接项；7) 未完成/未核实风险。
```

## 9. toolchain

```text
你是 KiaraBot toolchain 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-toolchain
（git 分支 kiarabot-toolchain，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-cpp-coding-standard.md、doc/00-boundaries.md、
doc/11-toolchain.md；读 reports/modules/toolchain.md 的 §3/§4/§5/§8、
reports/modules/ci-cd.md 的矩阵/静态分析结论；doc/18 只读。先输出执行前影响判断。

范围：允许修改 scripts/check-toolchain.sh、scripts/quality/（含 tests/）、cmake/、根 CMakeLists.txt、
CMakePresets.json、.clang-format、doc/11-toolchain.md。
禁止修改 .github/、scripts/ci/（ci-cd 分支）、proto/、api/、各模块实现目录；CI 侧改动写交接项。

待办：
P1-1 依赖门禁前置化：把 rg（check-format 硬依赖）、python3、ruby 加入 require_command/版本表/文档；
  缺工具要在最前面失败，而不是跑完两次构建后才失败。
P1-2 统一"本地 vs CI"的文件集：format/tidy 覆盖必须一致（CI 用 dev 编译库只含 core，本地用 proto 库含
  control/api+infrastructure）；提供两个显式模式（dev 快速 / full 全模块），CI 用 full。
P1-3 边界检查加覆盖：除 Core 直接 include 外，检查 Python（sdk/adapters）导入边界、面板不导入 Core、
  CMake target 链接方向、传递依赖（至少对违规 include 链做一跳）。
P1-4 其他：check-toolchain.sh 的 ctest 加 --no-tests=error；bash -n 覆盖 proto/tests 与 tests/acceptance；
  两套 validate-asyncapi.cjs 合并为一份；Python/TS/Shell 的 lint 接入（或明确不接入并写风险）；
  dev 预设是否默认编译 control/api 与 infrastructure（二选一并在文档说明）。
P2：CMake 依赖检查前移；离线/受限网络提示；单模块最小启动命令缺口说明。

验证：bash -n 相关脚本；python3 scripts/quality/tests/boundaries_test.py；
python3 scripts/quality/check-boundaries.py；check-format/tidy 需 clang-format/tidy（本机无 → 写明未运行）。

交付：1) 修改文件与理由；2) 行为变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) "检查项 × 覆盖模块"矩阵（现有 vs 修复后）；6) 需要 ci-cd 分支同步的条目；7) 未完成/未核实风险。
```

## 10. ci-cd

```text
你是 KiaraBot ci-cd 模块分支的实现代理。工作目录固定为
/Users/leekiara/Workspace/KiaraBot.worktrees/kiarabot-ci-cd
（git 分支 kiarabot-ci-cd，当前 HEAD 8caecb1）。不要在主仓库或其他 worktree 修改文件。

先做只读准备（不改代码）：读 AGENTS.md、doc/12-ci-cd.md、doc/11-toolchain.md、
README.md 的「当前状态」；读 reports/modules/ci-cd.md 的 §3/§4/§5/§8、
reports/modules/contracts.md 的 G1/§4.5、reports/verification.md；doc/18 只读。先输出执行前影响判断。

范围：允许修改 .github/workflows/ci.yml 及 .github/workflows/ 下新增文件、scripts/ci/、
doc/12-ci-cd.md、doc/16 中 ci 条目。
禁止修改 scripts/quality/、根 CMakeLists、proto/、api/、各模块实现；需要 toolchain 配合的写交接项。

待办：
P0-1 恢复自动触发（先保证手动全绿再恢复）：先修使 job 失败的具体问题；交付里明确
  "手动运行 → 全绿 → 取消注释 pull_request/push"的步骤；恢复后删除 inputs.base_ref 与
  BASE_SHA 的 fallback，PR 用真实 base.sha；无法本地验证 GitHub Actions 时必须写明"未在远端验证"。
P0-2 descriptor 基线可执行：上传当前 descriptor 作为后续基线（artifact 或 release），
  check-descriptor 从"上一版基线"取而不是 main/HEAD；保留 test-descriptor-gate 负例。
P1-1 构建矩阵补全：Clang 16 也构建 proto + infrastructure（现在只有 GCC13）；
  静态分析覆盖 control/api 与 infrastructure（proto 编译库或独立 job），与 toolchain 分支约定一致。
P1-2 依赖与缓存：vcpkg/uv/npm 缓存；依赖审计扩展到 panel、Python（uv.lock，可用 pip-audit/osv-scanner）；
  SBOM 若纳入范围则产物化。
P1-3 工程化：失败时上传测试日志/报告；job timeout；node/python 版本显式锁定；
  acceptance 失败保留分阶段日志；面板检查从 contracts job 拆出独立 job。
P2：macOS job（若承诺 AppleClang 支持）；定时依赖重扫；镜像构建/发布流水线（明确是否本分支范围，否则交接）。

验证：actionlint（本机可能无 go → 写明）；python3 解析 workflow YAML；
npm ci --prefix scripts/ci --ignore-scripts（无 node → 写明）；descriptor 脚本需 protoc/构建产物。
禁止声称 CI 已绿。

交付：1) 修改文件与理由；2) 行为变化；3) 文档影响判断；4) 实际运行检查与未运行项；
5) "job × 检查 × 覆盖模块"表；6) 恢复自动触发 checklist；7) 需要 toolchain 分支配合的清单；
8) 未完成/未核实风险。
```
