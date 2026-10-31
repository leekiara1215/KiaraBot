# KiaraBot 十模块功能覆盖评估

> **评估对象**：`main@8caecb1`（工作区 `/Users/leekiara/Workspace/KiaraBot`）
> **方法**：8 名并行分析员逐模块静态审计 + Lead 横向交叉核验 + 独立对抗复核；十份明细见 `reports/modules/`。
> **限制**：本机无 `cmake`/`ninja`/`node`/`npm`/`uv`/`protoc`/`docker`/`clang-format`/`clang-tidy`（有 python3.9/ruby/clang++/git），**未构建、未运行任何测试、未联网核对远端 CI**；"已通过/已验收"数字均引自仓库台账，属历史自述。所有行号以 HEAD 工作区为准。

---

## 0. 结论摘要（先回答三个问题）

### 0.1 十个分支是什么？

| 分支 | 模块 | 主要目录/文件 | 规模（非 lockfile 行） |
| --- | --- | --- | --- |
| `kiarabot-contracts` | 协议契约 | `proto/`、`api/` | 2885 |
| `kiarabot-core-runtime` | Core 运行时 | `core/` | 9789 |
| `kiarabot-plugin-sdk` | 插件 SDK | `sdk/`、`plugins/` | 1799 |
| `kiarabot-nonebot-adapter` | NoneBot 适配器 | `adapters/nonebot/`、`tests/acceptance/` | 1355 |
| `kiarabot-infrastructure` | 基础设施 | `infrastructure/` | 2006 |
| `kiarabot-control-api` | 控制 API | `control/api/` | 1894 |
| `kiarabot-control-panel` | 控制面板 | `panel/` | 546 |
| `kiarabot-operations` | 运维 | `deploy/`、`scripts/deploy/`、`Dockerfile` | ~950 |
| `kiarabot-toolchain` | 工具链 | `scripts/check-toolchain.sh`、`scripts/quality/`、`cmake/`、CMake 预设 | 524 |
| `kiarabot-ci-cd` | CI/CD | `.github/workflows/ci.yml`、`scripts/ci/` | 709（含文档） |

**关键事实**：十个分支当前**全部指向同一个 commit `8caecb1`**，与 `main` 的差异为 `0/0`；十个 worktree（`KiaraBot.worktrees/<branch>`）均 clean。reflog 显示它们被 `rebase (finish) ... onto 8caecb1`。因此**分支名是"模块范围标签/工作区划分"，不是并行的独立代码线**；模块边界实际靠目录约定 + 模块文档 + 少量脚本门禁（`scripts/quality/check-boundaries.py`）维持，而不是靠分支隔离。

### 0.2 十个模块是否完全覆盖项目功能？

**否。** 覆盖分四种情况：

| 覆盖状态 | 内容 |
| --- | --- |
| **有模块且主链路完整** | 事件入站→去重→持久化、插件投递与 ACK、Action 出站（仅纯文本）、Host storage/model/tasks、控制面 REST/WS（除 2 个频道）、SQLite 持久化、契约定义与双向 round-trip |
| **有模块但功能不完整** | 面板只读（零管理动作）、插件只能单实例、适配器只实现 1/7 Action、Core 无保留期清理/无全局串行、基础设施多个端口是死接口、控制面 `messages` 频道不可用、`/groups` 恒空、运维骨架不可运行、工具链不覆盖 4 条模块验证链 |
| **覆盖了但未生效** | **CI/CD**：8 个 job 设计齐全，但自动触发被注释（仅 `workflow_dispatch`），CD（镜像/发布/部署/回滚）整章未实现 → 十模块的自动化保护实际为 0 |
| **无任何模块覆盖** | ①跨模块验收入口 `tests/acceptance/` ②真实环境联调 T07b ③生产发布与 CD ④安全/封禁风控（`doc/17` 要求）⑤可观测性指标/告警/追踪 ⑥未来 C++ 层（T12，基线第 8–9 层）、C++/TS SDK ⑦会话/上下文存储 ⑧用户/群/角色/权限数据模型与写接口 |

### 0.3 最需要先处理的 8 个 P0 簇（多模块独立发现，已交叉印证）

| # | P0 簇 | 来源模块 | 证据 |
| --- | --- | --- | --- |
| 1 | **Action 会话 ID 契约自相矛盾**：接收侧事件 `conversation.id` 恒为 `group:<gid>`/`private:<uid>`，而 Core 的 Action ID 校验禁止 `:`、适配器发送侧又强制纯数字 → 按 SDK 示例（复用事件 conversation）构造的回复必 `FAILED INVALID_ARGUMENT`（失败点首先在 Core，适配器数字校验是第二道） | 适配器（+core/contracts） | `adapters/nonebot/.../mapper.py:45` vs `actions.py:50-52`；`core/src/application/action_router.cpp:26-32`；`plugins/ai-reply/ai_reply.py:34` |
| 2 | **入口背压与错误语义不一致 → 队列满丢事件**：适配器入口自身返回的队列满 `RATE_LIMITED` 为 `retryable=false`，而 `AuthorizedCommands` 对同类错误置 `true`；适配器把非 retryable 拒绝标 failed 并前进 → 队列满时消息静默丢失，违反 `doc/01-runtime-semantics.md:119` 与 T02a | 适配器 + contracts + core | `adapter_ingress.cpp:26-31,242-245`；`authorized_commands.hpp:128-129`；`client.py:110-120`；`adapter_ingress_test.cpp:122,205`（断言可重试，与实现静态矛盾） |
| 3 | **适配器无健康/状态上报**：无 8081 `/health/ready`、无 `AdapterStatusChanged`、无 `get_status()`；compose 的 adapter 永远 unhealthy → 依赖 `service_healthy` 的 panel 永不启动 | 适配器 + operations | `adapters/nonebot/src/`（无 health）；`compose.staging.yml:57-69,88-90` |
| 4 | **Core 保留期清理全部未接线**：`purge_expired()` 只有测试调用；审计 `vector` 无裁剪；命令幂等表终身保留 → 审计到 10 万条后所有受保护写（含事件接收）永久 `UPSTREAM_UNAVAILABLE`；快照超 64 MiB 后所有 commit 失败 | core（+infrastructure） | `runtime_engine.hpp:390-439`（仅测试调用）；`authorized_commands.hpp:84-86`；`sqlite_runtime_store.cpp:12,98-100` |
| 5 | **镜像构建的是 dev Core**：Dockerfile 未传 `KIARABOT_GENERATE_PROTO`/`KIARABOT_ENABLE_INFRASTRUCTURE` → 镜像无 gRPC/控制 API/SQLite/健康端点 → compose 的 core 健康检查必失败 | operations | `Dockerfile:11-12,24`；`CMakeLists.txt:4,120`；`control/api/src/http_mapper.cpp:143-152` |
| 6 | **容器网络与绑定策略冲突**：gRPC 与控制 HTTP 都只接受 `127.0.0.1:`，compose 却按 `http://core:8080` 跨容器访问并发布宿主端口 | operations（+core/control-api） | `core/src/runtime/bridge_server.cpp:528-529`；`control/api/src/http_server.cpp:296`；`compose.*.yml:33,59,84` |
| 7 | **迁移路径默认必失败**：`MIGRATION_COMMAND` 默认空 → `migrate.sh` 退 1；镜像内无 `kiarabot-migrate`；用 `true` 旁路则等于放弃"迁移前备份"保证 | operations（+infrastructure） | `compose.*.yml:15`；`migrate.sh:25-34`；`Dockerfile:12,24` |
| 8 | **CI/CD 未生效且兼容门禁空转**：自动触发被注释；HEAD `8caecb1` 的修复从未记录全绿；descriptor 基线在 main 上默认等于 HEAD，diff 恒等；上传的 descriptor 从未被下游消费 | ci-cd + contracts | `ci.yml:3-17,120-132`；`check-descriptor.sh:5-27` |

> 另：`compose` 中 adapter/panel 镜像在仓库内不存在（无 Dockerfile/console script），属 P0 簇 5 的同级阻断；详见 operations 报告 P0-2。

---

## 1. 判定基准：项目"功能"的权威来源

为避免用"代码有什么"反推"应该有什么"，本评估以仓库声明的功能为准（优先级从高到低）：

1. `doc/18-architecture-baseline.md`（人类冻结基线，只读）：九层架构 + 附录 A 落地对照；第七层"控制面板至少需要管理"11 项（`:234-245`）。
2. `README.md`「目标架构/仓库组成/当前状态」；`doc/09-roadmap.md` 阶段 0–9。
3. Core 11 项职责（`doc/18:135-147`）：事件接收分发、命令路由、权限检查、插件调度、会话管理、AI 请求编排、任务调度、统一消息发送、配置管理、日志审计、控制面板 API。
4. `doc/16-development-tasks.md`：T01a–T12 台账（状态为自述，需代码核实）。
5. 各模块文档 `doc/01`–`doc/08` 的自述职责与限制。
6. 契约面：`proto/` 7 类事件、7 类 Action、2 个 gRPC 双向流服务；`api/openapi.yaml` 25 个操作；`api/asyncapi.yaml` 7 个频道。
7. `doc/17-account-ban-risk.md`：出站配额、危险 Action 门禁、急停、错误分类、运维暴露面规范。
8. 横向：`doc/08`（运维）、`doc/11`（工具链）、`doc/12`（CI/CD）职责。

---

## 2. 覆盖矩阵：项目功能域 × 十个模块

图例：● 完整/主要覆盖　◐ 部分覆盖（有实现但缺关键子集/未接线）　○ 仅骨架或未生效　— 无归属

| 项目功能域 | contracts | core | plugin-sdk | adapter | infra | control-api | panel | ops | toolchain | ci-cd | 判定 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 跨语言契约定义（事件/Action/桥接/Host） | ● | — | — | — | — | — | — | — | — | — | 完整 |
| 契约→实现落位与兼容门禁 | ◐ | — | — | — | — | — | — | — | ◐ | ○ | **部分**（门禁空转） |
| 事件入站/去重/持久化 | — | ● | — | ◐ | ● | — | — | — | — | — | 主链路完整，事件类型仅 1/7 有生产端 |
| 权限与审计 | — | ◐ | — | — | — | ◐ | ◐ | — | — | — | **部分**（仅 `message.send`；无角色/资源模型） |
| 插件调度与生命周期 | — | ● | ◐ | — | — | ◐ | ◐ | — | — | — | **部分**（单插件、无日志/健康/重启） |
| Action 出站 | — | ● | ◐ | ◐ | — | — | — | — | — | — | **部分**（1/7 类，纯文本） |
| Host 能力（storage/model/tasks） | — | ● | ● | — | ◐ | — | — | — | — | — | 完整（模型仅非流式 OpenAI 兼容） |
| 持久化/迁移/恢复 | — | ◐ | — | — | ● | — | — | ◐ | — | — | 代码完整，容量/崩溃/演练未验证 |
| 控制面 REST | ● | — | — | — | — | ● | — | — | — | — | 22/25 完整，2 个 Action 查询未接线、`/groups` 未实现 |
| 控制面 WebSocket | ◐ | — | — | — | — | ◐ | — | — | — | — | **部分**（7 频道 5 可用；`messages` 不可用、`health` 永不推送） |
| 控制面板管理 | — | — | — | — | — | — | ◐ | — | — | — | **部分**（9 页只读，零管理动作） |
| 渠道适配（QQ/OneBot） | — | — | — | ◐ | — | — | — | — | — | — | **部分**（仅 message、4 种段、1 类 Action、无健康） |
| 部署/发布/运维 | — | — | — | — | — | — | — | ○ | ◐ | ○ | **不可运行**（T11b 未验收） |
| 质量门禁/工具链 | — | — | — | — | — | — | — | — | ◐ | — | **部分**（不覆盖面板/SDK/适配器/acceptance） |
| 自动化集成与发布 | — | — | — | — | — | — | — | — | — | ○ | **未生效**（自动触发关闭、无 CD） |
| **跨模块验收编排** | — | — | — | — | — | — | — | — | — | — | **无模块归属**（`tests/acceptance/` 被 4 个跨模块提交修改） |
| **真实环境联调（T07b）** | — | — | — | — | — | — | — | — | — | — | **无归属**（仅人工 runbook） |
| **安全/封禁风控（doc/17）** | — | ◐ | — | ◐ | — | — | — | — | — | — | **无归属**（Core 只拒绝危险 Action，无配额/门禁/急停） |
| **可观测性（指标/告警/追踪）** | — | ◐ | — | — | ◐ | ◐ | — | ○ | — | — | **无归属**（只有内存日志环 + 控制面 `/logs`） |
| **会话/上下文存储** | — | ◐ | ◐ | — | ◐ | — | — | — | — | — | **无归属**（`IConversationStore` 只有文档名） |
| **用户/群/角色/权限数据模型** | ◐ | ◐ | — | — | — | ◐ | — | — | — | — | **无归属**（契约与 Core 状态均缺） |
| **未来 C++ 适配器/插件/SDK/面板（T12）** | — | — | — | — | — | — | — | — | — | — | **无归属**（明确延期） |

> 结论：十模块覆盖了"单实例、单渠道、单插件、纯文本、只读看板、本机验收"这一条**最小可用纵向链路**，但没有覆盖"多实例/多插件、真实平台、生产发布、安全管理、集中观测、跨语言替换"这些项目明确声明的目标能力。

---

## 3. 逐模块缺口摘要（按分支）

> 各模块自评覆盖率口径不同（功能点枚举方式不同），**不可横向相加**；数字用于同模块内部定位，不用于模块间排名。

### 3.1 `kiarabot-contracts` — 契约（`proto/` + `api/`）

- **自评**：41/71 完整（58%），契约定义完整度高、实现覆盖度中低、兼容门禁无效。
- **P0**：契约兼容门禁空转（CI 手动 + 基线恒等 + descriptor 工件无人消费）。
- **P1**：`messages` 频道整条不可用（无投影、无事件、无角色可持有 `message.read`）；Action 仅落 1/7；7 类事件中 5 类无生产端；REST Action 查询结构性恒 404；契约门禁只覆盖 4/9 枚举、两套负向夹具未接线、Python 旧 reader 测试未接线。
- **契约面 > 实现面**：`Health`、`PageRequest/PageInfo`、`Account.display_name`、`Error.details`、`ActionEnvelope.target.user_id/group_id`、`PluginStatusChanged.lifecycle_state`、`MessageSegment.AUDIO/VIDEO/FILE/LOCATION`、`ConversationRef.CHANNEL` 等字段无生产/消费；`PluginManifest` 的 OpenAPI 枚举（cpp-rpc/native/wasm）与实现不符。
- 明细：[`reports/modules/contracts.md`](modules/contracts.md)

### 3.2 `kiarabot-core-runtime` — Core 运行时（`core/`）

- **自评**：47/64 完整（73%），含部分 89%；主链实现完整、引擎级测试扎实。
- **P0**：保留期清理与过期扫描全部未接线（审计/幂等/快照单调增长 → 写路径不可自愈死亡）。
- **P1**：5+ 并发写者共享同一全量 SQLite 快照，revision 冲突在 ACK/Action 结果路径被当作断连；插件 ACK 无超时（单插件挂起即全量停摆）；`recover/dispatch/expire/purge` 生命周期 API 仅测试接线；组合根只能配 1 适配器 + 1 插件；Action 权限只有 `message.send`（无群/速率/内容策略）；`User/Group/Conversation/Permission/Role/Policy` 状态不存在；容量上限全部硬编码。
- 另有 `stream_snapshot` 未判空 `find_if` 的潜在 UB、`GET /groups` 有策略无实现。
- 明细：[`reports/modules/core-runtime.md`](modules/core-runtime.md)

### 3.3 `kiarabot-plugin-sdk` — 插件 SDK（`sdk/` + `plugins/`）

- **自评**：完整 11/34（约 32%），其余为部分/未覆盖；**无代码证据支持的 P0**。
- **P1**：无插件发现/多插件运行时（CLI 只能启一个插件）；会话管理（ConversationService）无 proto 也无 SDK；基线插件 `group_admin`/`reminder` 完全缺失、`ai-reply` 仅 40 行单轮示例；插件日志被重定向 `/dev/null`；插件崩溃不重启、无健康通道；CI 未自动跑 SDK 测试。
- 显式 `CAPABILITY_NOT_SUPPORTED`（依赖解析/native-wasm/非空 config schema）属已声明限制；主要问题是文档漂移（清单示例无法注册、权限词表与实现不符、`PLUGIN_SDK_DEPENDENCIES.md` 已过期）。
- 明细：[`reports/modules/plugin-sdk.md`](modules/plugin-sdk.md)

### 3.4 `kiarabot-nonebot-adapter` — NoneBot 适配器（`adapters/nonebot/` + `tests/acceptance/`）

- **自评**：完整 33/68（49%），部分 15，未覆盖 20。
- **P0**：①Action 会话 ID 契约矛盾；②背压拒绝被判永久失败（队列满丢事件）；③无健康/状态接口（compose 依赖链断裂）。
- **P1**：事件仅 `message(group/private)`，notice/request/meta 不处理；消息段仅 text/image(URL)/at/reply（file/voice/video/face/forward 丢弃）；outbox 10000 上限计入已确认行 → 约 1 msg/min 打满并静默丢消息；failed 记录永不清理；平台错误不分类（限频/验证/被踢不可区分）；无发送限速；备份只恢复一侧会持续 `out_of_order CONFLICT`；T07b 仅人工 runbook。
- 明细：[`reports/modules/nonebot-adapter.md`](modules/nonebot-adapter.md)

### 3.5 `kiarabot-infrastructure` — 基础设施（`infrastructure/`）

- **自评**：完整 25/60（42%），部分 10、未实现/未接线 16、未验证 9；**无 P0**。
- **P1**：`doc/05` 宣称的 `IInboxRepository/IOutboxRepository/IActionRepository` 不存在，`IUnitOfWork/INamespaceStore/IIdempotencyStore/IAuditLogger` 零实现零消费者（"只换适配器不改业务"不成立）；`FileStorage` 完全未接线；64 MiB 快照上限无测试、超限后所有提交失败；崩溃/半写恢复无进程级验证；`migrate` CLI 无测试；模型 Provider 唯一边界测试从未执行。
- P2：时区无预检且误分类、无缓存；日志 sink 仅内存环、字段与 `doc/08` 规范不一致；`Postgres/Redis` 未排期。
- 明细：[`reports/modules/infrastructure.md`](modules/infrastructure.md)

### 3.6 `kiarabot-control-api` — 控制 API（`control/api/`）

- **自评**：REST 25/25 有路由（22 完整）；WS 事件 8/12；认证/幂等/分页/错误映射完整；生产可用性 6/15。
- **P0**：未发现鉴权绕过/明文凭据/路径缺失。
- **P1**：`messages` 频道永久 4403；`/ws/health` 永不推送且与 REST 健康聚合语义不一致；两个 Action 查询端点无生产者恒 404；事件流 100ms 全量轮询（断开会话仍保留 10 分钟）、单 io 线程可被低权账号放大；生产面缺通用限流/CORS/keep-alive/优雅关闭/TLS/访问日志；`/groups` 恒空。
- 明细：[`reports/modules/control-api.md`](modules/control-api.md)

### 3.7 `kiarabot-control-panel` — 控制面板（`panel/`）

- **自评**：41/70 完整（59%）、3 部分、26 未覆盖。
- **P1**：九页全部只读、**零管理动作**（基线 11 项管理能力无一可操作）；契约已就绪的 7 个端点（插件 validate/enable/disable/config/health、会话、Action 查询）未接线；机器人/模型 API Key/群白名单/用户角色/任务触发/审计过滤在 OpenAPI 中也无写端点；`npm test` 依赖 `build/proto` C++ 产物与 `control/api/tests/.venv`，文档未说明；CI 中面板检查在 `contracts` job 且自动触发暂停；契约漂移检测只断言 6 组 schema 结构。
- 明细：[`reports/modules/control-panel.md`](modules/control-panel.md)

### 3.8 `kiarabot-operations` — 运维（`deploy/`、`scripts/deploy/`、`Dockerfile`）

- **自评**：完整 3/25（12%）、部分 11、未覆盖 11；脚本层质量较好（本机实测部署测试 7/7），但镜像—Compose—启动参数—容器网络四层互不相接。
- **P0**：Dockerfile 构建 dev Core（无 proto/infra/健康端点）；compose 的 adapter/panel 镜像不存在；compose 未把 Core 配成可服务模式（无 `--grpc-listen`/DB/控制配置，`HEALTH_PORT` 无读取者）；跨容器网络不可达（只绑 127.0.0.1）；迁移默认必失败。
- **P1**：环境变量名不一致（`CORE_ENDPOINT` vs `CORE_GRPC_ENDPOINT`；`CONTROL_API_URL` vs 构建期 `VITE_*`）；adapter 无数据卷；panel 被错误 gated 在 adapter healthy 之后；无 TLS/日志轮转/集中收集/指标/告警；备份与 DB 同卷、无保留/异地/完整性校验/演练；rollback 无 schema 检查与部署记录；升级窗口必然停机；systemd unit 无 `User/Restart`、oneshot 不感知容器死亡。
- 明细：[`reports/modules/operations.md`](modules/operations.md)

### 3.9 `kiarabot-toolchain` — 工具链（`scripts/quality/`、`check-toolchain.sh`、根构建）

- **自评**：本机 12 类检查中 11 类有实现，但只覆盖 C++ 侧 4 个模块。
- **P1**：统一检查不覆盖面板/SDK/适配器/`tests/acceptance` 四条验证链；未声明 `rg`/`python3`/`ruby` 依赖（缺 rg 时在长时间构建后才失败）；本地与 CI 的 clang-tidy 覆盖不一致（CI 只分析 core）；边界检查只扫 Core 直接 include（不查传递依赖、不查 Python/TS/CMake 链接）；dev 预设不编译 control/api 与 infrastructure，Clang16 从未编译这两模块。
- P2：两套 AsyncAPI 校验器行为不同；`bash -n` 漏 `proto/tests/`、`tests/acceptance/`；CTest 未用 `--no-tests=error`；Python/TS/Shell/CMake 无 lint；API 校验依赖网络。
- 明细：[`reports/modules/toolchain.md`](modules/toolchain.md)

### 3.10 `kiarabot-ci-cd` — CI/CD（`.github/workflows/ci.yml`、`scripts/ci/`）

- **自评**：`doc/12` 要求的 8 类 CI 检查中 5 完整、3 部分；CD 6 条职责 0 落地；10 条验收标准 2 部分成立、8 不成立。
- **P0**：自动触发被注释（仅 `workflow_dispatch`）；HEAD 的修复从未在远端验证全绿。
- **P1**：双编译器与静态分析只覆盖 Core；依赖审计只审 `scripts/ci`（面板/Python/vcpkg 均未覆盖）；vcpkg 每次源码构建、无缓存（90 分钟超时风险）；无 macOS/Windows job（与 AppleClang 承诺不符）；无镜像/SBOM/发布/部署流水线。
- 明细：[`reports/modules/ci-cd.md`](modules/ci-cd.md)

---

## 4. 未被十模块覆盖的功能域（这是"是否完全覆盖"的核心答案）

1. **跨模块验收层 `tests/acceptance/` 无归属**：它是唯一同时拉起真实 C++ Core、SQLite、Python SDK、控制 API、适配器 fake 平台的入口（`tests/acceptance/run.sh`），但十个分支里没有"验收/集成"模块；历史由 4 个跨模块提交（`330ca47`/`dfde624`/`f8388f0`/`8caecb1`）顺带修改。**后果**：没有模块对"跨语言闭环是否持续可用"负责，CI 一旦暂停它就完全失效（事实上已如此）。
2. **真实环境联调 T07b 无归属**：仅有 `tests/acceptance/real-environment.md` 人工 runbook，无脚本、无结果记录；真实 QQ/NapCat、真实模型、真实会话均未验证。适配器报告独立指出：真实 ID 与契约 ID 方案不一致（P0 簇 1）恰恰只会在真实联调时暴露。
3. **生产发布/CD 无归属**：`ci-cd` 无镜像构建/GHCR/SBOM/部署/回滚；`operations` 只有脚本片段且默认不可运行。`doc/12` 的 CD 责任 6 条全部未实现，属"明确延期"但当前无模块承接。
4. **安全/封禁风控无归属**：`doc/17` 要求出站配额、危险 Action 白名单+每日预算、急停开关、风控错误分类、NapCat 暴露面规范。现状：Core 只有 `message.send` 且无速率；适配器无发送限速、错误统一 `UPSTREAM_UNAVAILABLE`；运维文档无账号/网络暴露规范。这是**跨 core/adapter/ops/contracts 的需求，但没有一个模块或分支拥有它**。
5. **可观测性（指标/告警/追踪）无归属**：只有 Core/基础设施的内存日志环与控制面 `/logs`（且 `/logs` 实为审计投影）。无 metrics、无 alert、无 trace 传播、无日志轮转/集中收集；`doc/12` 第 5 步与 `doc/17:127` 的指标清单全部未落地。
6. **会话/上下文存储无归属**：基线 Core 职责含"会话管理"、基础设施含 `IConversationStore`，但 `plugins.proto` 无会话操作、`ports.hpp` 无该接口、RuntimeSnapshot 只有 `SessionStatus`（无上下文内容）。插件只能退化到 storage 自建。
7. **用户/群/角色/权限数据模型与写接口无归属**：Core 声明拥有 User/Group/Conversation/Permission/Role/Policy，实际状态模型没有；OpenAPI 的 `/users`/`/groups`/`/bots`/`/models`/`/tasks`/`/audit` 全部只读；`/groups` 连实现都没有。面板因此无法补齐管理动作。
8. **未来 C++ 层与多语言 SDK 无归属**：基线第 8–9 层（C++ 适配器、C ABI 插件）、`sdk/cpp`、`sdk/typescript`、`tools/kiara-plugin` 均不存在；T12 已由用户明确延期，但"延期"不等于"被十模块覆盖"。
9. **备份恢复/灾难恢复演练**：`operations` 有脚本但无演练记录、备份与 DB 同卷、restore 的"已停止所有 DB 使用者"守卫依赖不存在的 `-wal/-shm`（现用回滚日志）→ 可在 Core 运行时覆盖活跃 DB。

---

## 5. 跨模块系统性问题（把十份报告放在一起才看得出的）

1. **"唯一契约"在实现层被多套语义改写**：`conversation_id`（前缀 vs 数字）、`retryable`（文档可重试 vs 实现 false）、gRPC 错误状态（文档映射 vs in-band Error）、ID 长度/字符集（128 vs 256、`:` 允许性）、Host 未知操作错误码（`INVALID_ARGUMENT` vs `CAPABILITY_NOT_SUPPORTED`）——契约文件质量高，但**边界语义没有单一实现裁决点**。
2. **健康与就绪契约层层断裂**：Core 健康端点只在 proto+infra 构建存在；适配器/面板根本没有健康服务；`HEALTH_PORT` 无代码读取；控制面 `/ws/health` 永不推送。运维 compose 却按"四服务依次 ready"编排 → 部署设计基于不存在的契约。
3. **网络绑定与部署拓扑冲突**：Core gRPC 与控制 HTTP 强制 `127.0.0.1:`（安全设计），Compose 却要跨容器访问。这不是某模块写错，而是安全策略与部署形态没有统一决策。
4. **迁移权威来源分裂**：Core 打开 DB 时自动迁移 v1→v5；`migrate.sh` 默认失败、镜像无 `kiarabot-migrate`；运维文档建议 `MIGRATION_COMMAND=true` 旁路。三者对"谁是迁移权威、备份时点在哪"没有共识。
5. **权限模型不足以支撑已声明的管理面**：Core 只有 `message.send`；AsyncAPI 声明 `message.read` 但没有任何角色能持有；面板按服务端权限过滤导航；`doc/17` 要求配额/危险 Action 门禁。**契约、Core、面板、风控四方对"权限"的假设不一致**。
6. **单实例假设贯穿代码、部署与产品目标**：Core 组合根只支持 1 适配器 + 1 插件；插件 CLI 不支持发现；Compose 却按 4 服务 + 多插件生态设计；面板有 9 个资源页。架构目标（可扩展平台）与装配能力（单实例）不匹配。
7. **文档漂移成片且互相矛盾**：受保护 `doc/18` 附录 A 仍称"适配器未入仓"（已于 `f8388f0` 入仓）；`doc/03` 清单示例当前无法注册、权限词表与实现不符；`doc/05` 端口清单含不存在接口；`doc/08` 目录清单含不存在的文件；`doc/12` 关于 OpenAPI 探针的描述已过时；`sdk/PLUGIN_SDK_DEPENDENCIES.md` 仍称 T06b/T06c 待接线（已实现）。**注意：`doc/18` 是受保护基线，按规则只报告差异，不修改。**
8. **所有"已验证"都建立在不可复跑的路径上**：十份报告一致指出，测试数字均来自台账自述；CI 自动触发暂停、HEAD 修复未验证；本机无工具链无法复跑。**当前没有任何一条"通过"结论能被独立复现**——这本身就是覆盖评估里最大的不确定性。

---

## 6. 优先修复建议（按投入产出排序）

**第一优先级（P0，建议同一批次处理）**

1. 统一 Action 会话 ID 方案（契约 + 事件映射 + 适配器 + Core + 示例插件），并用真实群/私聊 ID 加回归测试 → 解 P0 簇 1。
2. Core 对 `RATE_LIMITED/存储暂时故障` 返回 `retryable=true` 与 `retry_after_ms`，修复测试断言矛盾；适配器保持 pending 重投 → 解 P0 簇 2。
3. 接通保留期/容量治理：生产装配调用 `purge_expired`、审计裁剪、配置化上限；对 64 MiB 快照与 10 万审计上限加告警/测试 → 解 P0 簇 4。
4. 部署四件套一起修：Dockerfile 改 proto+infra 构建并产出 `kiarabot-migrate`；补 adapter/panel 镜像（或明确延期并去掉 compose 依赖）；Core 增加受控监听地址/同网络命名空间方案；`MIGRATION_COMMAND` 给出唯一权威值 → 解 P0 簇 5–7。
5. 适配器健康/状态上报 + `AdapterStatusChanged`，与 compose 健康检查对齐 → 解 P0 簇 3。
6. CI：恢复自动触发（先手动全绿），修复 descriptor 基线（固定发布版基线 + 基线工件消费）→ 解 P0 簇 8。

**第二优先级（P1，按模块）**

- contracts：补齐 `messages` 频道授权路径或删频道；Action 7 类分批实现或把契约标注为子集；5 类事件排产生产者；契约门禁接入两套未接线夹具 + 覆盖率门禁。
- core：跨服务写入串行化（全局写锁/单写者）；插件 ACK 超时与重试；组合根支持多插件/多适配器与配置化上限；权限模型升级（资源范围 + 配额）。
- plugin-sdk：插件发现/多实例；`ConversationService` 是否进协议需决策；恢复正常插件与 SDK 日志通道。
- adapter：notice/request/meta 事件、更多消息段、Action 其余 6 类（或明确只支持纯文本的策略）；outbox 容量/保留/失败清理；错误分类。
- infrastructure：补齐/删除死端口；`FileStorage` 接入或明确不用；容量/崩溃/迁移测试；模型边界测试进 CI。
- control-api：health 事件与 REST 语义统一；Action 查询补生产者与保留期；流式轮询改事件驱动 + 每用户配额；生产面（限流/CORS/优雅关闭）与 TLS 归属。
- panel：先接已就绪的 7 个端点（插件管理/会话/Action 查询），再补 OpenAPI 写端点；契约漂移检测改为从 `paths` 派生。
- operations：变量名对齐、adapter 卷、TLS/日志/备份保留与演练、rollback schema 检查、部署记录。
- toolchain/ci-cd：把 4 条未覆盖验证链接入统一入口与 CI；CI 增加 proto+infra 的 Clang 构建与静态分析、依赖审计扩展、缓存。

**需要人类维护者裁定的决策**（AI 只报告，不修改）：

- `doc/18` 与实现的范围差异：适配器已入仓；面板"至少需要管理"11 项 vs 只读看板；基线 8–9 层延期。
- 面板管理范围与 OpenAPI 写接口是否进入 v1。
- 控制 API 是否长期与 Core 同进程、是否允许非回环监听（部署形态）。
- 迁移权威路径（Core 自动迁移 vs `kiarabot-migrate`）。
- 是否接受"纯文本单 Action + 单实例"作为 v1 产品边界。

---

## 7. 方法与限制

- **执行方式**：8 名分析员并行（每人 1–2 个模块，各自独立读源码/契约/文档并写 `reports/modules/<module>.md`）；Lead 负责分支事实校准、横向矩阵、跨模块矛盾与最终报告，并对 4 条最高风险声明做了独立复现（见 §8）。
- **证据规则**：每条缺口必须有 `path:line`；"文档说已实现"须找到调用链/配置/测试接线点，否则标 `未接线`/`未验证`；"未验证"与"未实现"严格区分。
- **静态分析限制**：无法构建/跑测试/跑 CI/联网；所有"通过/34/34/66/66/24/24"等数字均为台账自述，未复核。
- **口径限制**：各模块覆盖率自评的功能点枚举方式不同，不可横向相加；本报告的 §2 矩阵是定性判定，不是加权总分。
- **历史快照**：`doc/14`、`doc/15` 明确为历史快照，引用时已标注；`doc/16` 台账为自述状态。

---

## 8. 独立复核结果

由独立复核员（panel-analyst，未参与其余 9 份模块报告）对十份报告各抽 2 条最关键 P0/P1，共 **20 条**回源核对（证据行号、缺口类型、严重度），完整表见 [`reports/verification.md`](verification.md)。

**统计：成立 19、部分成立 1、不成立 0、无法核实 0。**

| 复核项（抽样） | 结论 | 补充/修正 |
| --- | --- | --- |
| contracts：契约兼容门禁空转（P0） | 成立 | main 上手动运行 `origin/main==HEAD` 自比恒通过；artifact 只 upload、无 download 消费者 |
| contracts：队列满 `RATE_LIMITED retryable=false` | 成立（表述需修正） | 实现 `adapter_ingress.cpp:26` 默认 false、`:242-245` 未传 true；但"所有拒绝都 false"不成立（`authorized_commands.hpp:128-129` 对同类错误置 true）；测试行号为 `adapter_ingress_test.cpp:122,205` |
| core：`purge_expired` 无生产调用方（P0） | 成立 | 全仓仅 `core/tests/*` 调用；审计无 erase/clear |
| core：共享快照无全局串行化（P1） | 成立 | `main.cpp` 同一 store 注入 6 个服务；无串行化装饰器 |
| plugin-sdk：无插件发现/多插件运行时（P1） | 成立 | 组合根仅一组 `KIARABOT_PLUGIN_*` |
| adapter：Action 会话 ID 矛盾（P0） | 成立 | 失败点首先在 Core（`action_router.cpp:26-32` 禁止 `:`），适配器数字校验为第二道 |
| adapter：背压/拒绝一律永久失败（P0） | **部分成立** | 队列满与适配器标 failed 成立；"Core 全部拒绝 retryable=false"不成立（已按修正改写 §0.3） |
| infrastructure：`doc/05` 端口承诺未兑现（P1） | 成立 | `IUnitOfWork` 无实现，Inbox/Outbox/ActionRepository 不存在 |
| infrastructure：64 MiB 快照上限无测试（P1） | 成立 | 另有 1,000,000 项上限同样无测试 |
| control-api：`messages` 频道永久 4403（P1） | 成立 | 与 contracts G3 同根因 |
| control-api：Action 查询无生产者（P1） | 成立 | 补充：`ActionRouter::lookup` 已实现 7 天保留 + EXPIRED，修复应复用而非新写 |
| control-panel：零管理动作、7 端点未接线（P1） | 成立 | — |
| operations：镜像构建 dev Core（P0） | 成立 | — |
| operations：回环绑定与 compose 冲突（P0） | 成立 | 适配器 `client.py:29-31` 也强制 `127.0.0.1` → Core/控制面/适配器三方与 compose 的双重冲突 |
| toolchain：统一检查不覆盖 4 条链（P1） | 成立 | — |
| toolchain：本地/CI clang-tidy 不一致（P1） | 成立 | — |
| ci-cd：自动触发关闭（P0） | 成立 | — |
| ci-cd：HEAD 修复未远端验证（P0） | 成立 | — |

**复核揭示的跨报告重复/矛盾（已在 §0.3、§4、§5 采用去重后口径）**：

1. "CI 自动触发关闭"被 ≥6 份报告重复列出 → 主结论只归 ci-cd，其他报告改为引用，避免同一 P0 被多模块重复计数。
2. 背压 retryable 的分级不一致（contracts P1 vs adapter P0）→ 按 `doc/16:111`"背压不丢事件"已勾选口径统一为 P0，但表述以复核修正版为准。
3. `messages` 频道（contracts G3 ≡ control-api #1）、Action 查询（contracts G6 ≡ control-api #3）为同根因重复条目，本报告各保留一条并注明归属。
4. Core 保留期/幂等清理（G1/G19）与 infrastructure 64 MiB 上限属同一"运行期无界增长 → 写路径死亡"风险，建议合并为一条跨模块 P0 统一处置。
5. panel 管理动作依赖 control-api 的 Action 查询生产者，正确修复顺序是先 API 后面板。

**复核的边界**：本复核仍是静态回源，不代表运行时验证；CI 历史依据 README/`doc/12` 自述，未联网核实 Actions 记录。这与 §7 的限制一致——本报告的所有"通过/已实现"结论都缺少可独立复跑的运行证据。
