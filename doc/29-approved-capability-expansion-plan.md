# 已批准的五项能力扩展计划

- 决策日期：2026-11-04。
- 来源：[本轮用户决策](plans/user-decisions-2026-11-04.md)。
- 基础：[原实施计划](23-implementation-plan.md)、[当前证据矩阵](27-phase-exit-evidence.md)。
- 组织：主代理统筹；具体任务交由 `gpt-6.1-sol`、`reasoning_effort=high` subagent。
- 后续执行：用户已继续授权契约冻结及按依赖实现，实际进度与命令见[执行台账](plans/2026-11-04-capability-execution-ledger.md)。下文“本轮交付”与第 6 节保留计划整理阶段的作用域。
- 本轮交付：正式登记范围、源码核对、依赖拆分、文件所有权、实现及验收任务定义。
  本文和子计划不是已实现、已冻结线协议或已通过验收的声明。

## 1. 范围与历史决定的替代关系

以下五项全部进入实施计划，分别跟踪，不将局部证据扩展为整项完成：

1. **WS：目标 Control WebSocket。** 提供真实目标服务的管理事件通道和 Panel 恢复；
   按操作进度、健康、资源状态及其余已声明频道分批交付，各批单独验收。
2. **STR：模型 streaming。** 打通 Provider → Model → Runtime → PluginBridge → SDK
   文本增量、结束、故障、取消和计量；保留当前非流式能力。
3. **HERR：未知 Host 请求的错误语义对齐。** 区分缺少操作、未知操作及合法但不支持的操作，
   先确认 reference/target 的证据与适用范围，再修正目标处理及登记，避免改写历史 blob。
4. **HOST：独立 host operation 管理。** 定义宿主维护/排空操作、持久进度和授权查询，
   将 Host 作为独立 operation owner 接入 Control；保留 Runtime 实例生命周期的协调责任。
5. **BOOT：bootstrap 安全交付及字段级验收。** 从 Runtime 首次签发到受控执行/部署端消费，
   处理一次明文、响应丢失、重放、撤销和受控重新发放，普通资源 REST 不回显秘密。

本决定替代上述五项原来的“延期”“明确不做”或“只文档化、不扩契约”的范围限制；
当前代码尚未交付的能力仍是 `not_implemented`。历史日期记录不重写为今天的实现状态。

**两主机部署与断网/网络分区演练继续延期。** 五项自身的 WS 重连、流中断、交付响应丢失和
依赖故障测试仍必须完成，但不因此追加两主机项目。自动跨主机接管、高可用、任意远程 shell、
动态驱动市场不属于本次范围。真实 QQ 主动收发、付费 Provider 和发布动作沿原授权边界执行。

## 2. 统一基准、状态与管理规则

本轮基准是 `0496b90053d046fd61d7f106090bb87fff172f8a` 加既有未提交重构差异。
此前本机 `167/167` CTest、格式和 tidy 的结果只适用于 [doc/27 第 0 节](27-phase-exit-evidence.md)
记录的工作区，不能作为新增能力完成证据。已有工作不得被 reset、覆盖或混入新任务完成声明。

每项任务的状态只能从“计划已批准 → 方案已核对 → 契约已冻结 → 实现中 → 局部实测 →
集成验收通过”顺序推进。当前首批 subagent 负责方案核对和可执行任务拆分；
子计划里的建议 RPC、字段、端口、错误和测试名，在契约任务通过前都属于提案。

首批三份子计划合计 **34 个任务**：WS-01～10、STR-01～11、HERR-01～03、
HOST-01～05、BOOT-01～05。方案与任务拆分、[波次 B 契约冻结](30-capability-expansion-contract-freeze.md)
已完成；当前按依赖实施第一批切片，实际验收状态以执行台账和 doc/27 为准。
子计划中“主代理/契约 owner 串行收口”表示共享编辑锁和集成评审责任；
具体协议、代码与测试编辑仍下发给指定 subagent，主代理不代替各模块实现者。

主代理负责：

- 按用户批准范围裁定常规设计取舍，处理依赖和跨模块冲突，不把已有批准重复当作等待批准。
- 协调公共契约、文件所有权、构建资源与集成顺序；同一共享文件只指定一个当前编辑者。
- 复核拒绝、超时、依赖故障、未知结果和权限收缩路径；不以 happy path 代替完整验收。
- 维护当前阶段证据与实际命令结果；保留首轮失败，不以孤立重跑抹去失败记录。

subagent 必须先读根 `AGENTS.md`、[C++ 规范](12-cpp-coding-standard.md)、
[模块边界](00-boundaries.md) 和所属模块文档；不得修改 `doc/18-architecture-baseline.md`、
其保护清单、生成文件或无关改动，不得泄漏秘密、跨库或将传输类型反向引入内层。

## 3. 已分派工作流与边界

### WS：Control WebSocket

- 执行者：`/root/control_ws`，GPT-6.1 Sol / high。
- 首批产物：[WS 子计划](plans/2026-11-04-control-ws-plan.md)。
- 输入：当前 Identity 会话/权限、真实 owner 状态与游标、客户端 hello/resume。
- 输出：按当前用户和资源授权过滤的事件、有限重放、`RESYNC_REQUIRED` 与权威 REST 恢复。
- 首批需核定：现有 owner 只有 operation 快照查询，缺乏已验收的完整变更源；
  不能用 Control 进程内捕获的 operation id 集合冒充 scope 订阅或历史。
- 计划修改范围：`api/asyncapi.yaml`、直接受影响的 owner RPC、Control 应用/HTTPS 外层和
  `panel/src/{stream,api}/`、`panel/src/App.tsx`、`panel/src/views/p12-live.tsx` 及对应测试；逐任务精确名单见子计划。
- 关键语义：无权限不发事件，撤销/到期终止，owner 不可用不能推送假成功；
  游标不可恢复明确全量重查；慢客户端有界，重启后按游标策略恢复。
- 文档：`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`doc/06-control-api.md`、
  `doc/07-control-panel.md`，涉及探针/部署时同步 `doc/08-operations.md`。

### STR：模型 streaming

- 执行者：`/root/model_streaming`，GPT-6.1 Sol / high。
- 首批产物：[模型 streaming 子计划](plans/2026-11-04-model-streaming-plan.md)。
- 输入：认证实例、模型、逻辑调用 id、消息、deadline 和取消。
- 输出：有序文本增量、明确终结结果、usage 来源和可能计费事实；现有非流式调用保持可用。
- 计划修改范围：`proto/kiarabot/v1/{model,plugins}.proto` 及相关公共类型、
  Model/Runtime 的应用和端口、Provider HTTP 适配、目标 RPC/PluginBridge、Python SDK、
  本地 HTTP/TLS/受管插件夹具及直接 CMake；逐任务精确名单见子计划。
- 关键语义：取消、断流和 partial 不等于未执行或未计费；同逻辑调用重放不重新调用 Provider；
  Bridge 不能在同步模型调用期间堵塞读循环，增量队列/内存有界、结果不能跨实例混流。
- QQ 默认聚合完整结果后一次发送；QQ 分段发送/编辑和网页聊天产品交互单独列任务范围，
  不因 streaming 直接向真实会话发消息。
- 文档：协议 README、`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、
  `doc/02-core.md`、`doc/03-plugin-sdk.md`、`doc/05-infrastructure.md`，运行参数变化同步运维。

### HERR、HOST、BOOT：分别跟踪的 Host/凭据工作流

- 执行者：`/root/host_bootstrap`，GPT-6.1 Sol / high。
- 首批产物：[Host/Bootstrap 子计划](plans/2026-11-04-host-bootstrap-plan.md)。
- **HERR 输入/输出**：HostRequest 的 envelope、oneof 与协商能力 → 稳定业务错误和传输映射。
  `OPERATION_NOT_SET`、未知 wire 字段、已知未支持能力必须分别测试；旧 reference 的错误差异
  不能直接归到 target，也不得为修改旧错误码破坏 reference manifest。
- **HOST 输入/输出**：有权管理员的宿主维护/排空请求 → 持久 operation、进程事实、
  `BLOCKED`/`UNKNOWN` 和恢复进度。Host 离线不是进程停止，排空须与 Runtime 禁派协同。
  Host 操作事实留在 Host 所有的存储，Control 不跨库，HostBridge 保持受控执行通道。
- **BOOT 输入/输出**：精确资源与已授权部署 recipient → 受控秘密交付/消费证据；
  Runtime 仅存摘要，普通资源查询不返回明文，同请求重放不能重新暴露秘密，
  丢失响应与交付失败必须有显式恢复/重新发放路径。Adapter、外部自管插件、受管插件分别验收。
- 计划修改范围：HERR 的目标 PluginBridge/SDK 校验与测试；HOST 的 `host.proto`/Operation owner
  公共契约、Host 应用/SQLite journal、Runtime 协调端口和 Control 查询；BOOT 的 Runtime 资源
  应用/RPC、Control 受控秘密交付端口、部署脚本/执行端、Adapter/SDK 消费与测试。
  具体文件按子任务冻结，不授权改其他模块私有表。
- 文档：`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`doc/02-core.md`、
  `doc/03-plugin-sdk.md`、`doc/06-control-api.md`、`doc/08-operations.md`；
  BOOT 与 [接入向导](26-real-qq-onboarding.md) 联动。

## 4. 实施顺序与跨工作流依赖

### 波次 A：源码核对与任务拆分

三个 subagent 并行完成各自子计划，主代理核对五项无遗漏、范围不互相替代，登记输入输出、
错误语义、代码/文档范围、验收命令和共享文件。此波次不变更生产代码或契约实现状态。

### 波次 B：契约及纯端口冻结

1. HERR 先明确 reference/target 与缺失/未知/不支持的精确语义，不改受保护历史。
2. BOOT 冻结 recipient/授权、秘密交付、响应丢失与重新发放；HOST 冻结维护动作及独立 owner。
3. WS 冻结真实 owner 的 OperationChanges 或等价权威变更源、权限映射、游标和保留期；
   Host 的变更事件复用这套操作变化契约，避免两套独立进度/游标语义。
4. STR 冻结流式帧、调用关联、取消/部分失败/计量、能力协商和重放规则，保留非流式协议。
5. 先更新唯一契约和直接模块文档，运行解析、代码生成、descriptor/target/evidence gates；
   未实现接口明确登记不可用，不提前翻转 `implemented`。

### 波次 C：按模块实施和局部实测

优先形成 HERR 和 BOOT 的可独立交付切片，再实现 HOST；WS 与 STR 在各自公共契约冻结后
可由不同模块并行推进。功能的应用层、基础设施、传输、SDK/Panel 各有明确文件所有者，
跨模块通过已冻结公共端口连接，不由单个业务任务越界改内部实现。

共享文件包括 `proto/tests/{contract_status,target_spec}.json` 及其 oracle/evidence、
公共 `platform.proto`、`core/src/runtime/platform/runtime_bridge.cpp`、相关组合根/CMake、
`doc/01-contracts.md`、`doc/01-runtime-semantics.md`、`doc/02-core.md`、`doc/03-plugin-sdk.md`、
`doc/06-control-api.md`、`doc/08-operations.md` 和 `doc/27-phase-exit-evidence.md`。
公共登记由指定契约任务串行收口；`panel/src/App.tsx` 和 stream 客户端以 WS 所有者为主，
STR 如需界面承载必须先对齐接口及合并顺序。

共享 `build/full-locked` 的 configure/build、生成目录、PG 测试环境和全量 CTest 由主代理
统一排队；并行 worker 使用独立构建目录，不能同时重写同一编译数据库或生成目录。

### 波次 D：真实链路集成与交付

- WS：真实 Control/Identity/owner + Panel，覆盖成功、越权/撤权、游标失效、断流/重启、慢消费者。
- STR：真实 managed SDK 插件 + Runtime/Model + 本地真实流式 HTTP Provider fixture，
  覆盖首段、结束、取消、中途错误/断流、UNKNOWN、usage 和并发隔离，不用 fake port 冒充全链路。
- HOST：真实 Host/Runtime + 所有者存储 + Control 查询，覆盖排空收敛、未知执行、重复操作和恢复。
- BOOT：真实创建/绑定 → 受控交付 → Bridge 注册，覆盖丢失响应、重放不泄密、错误 recipient/实例、
  撤销和重新发放；HERR 正常、缺失、未知/不支持和畸形请求分别验证。
- 以上通过后再统一构建、运行全量套件和适用质量门禁，更新字段/方法/事件状态与阶段证据。
  目标部署重建与 release SHA 验证单列，不能由本机 fixture 代替。

## 5. 验证入口与验收条件

具体新增测试及目标名由各契约/实现任务确定；子计划中尚未落地的名字不能当成已注册测试。
固定依赖、编译器和 Python/Node 路径沿用 [工具链](11-toolchain.md)，不得顺手升级依赖。

协议/公共 API 变化的检查入口：

```bash
./proto/tests/check_contracts.sh
python3 proto/tests/check_implementation_status.py
python3 proto/tests/check_semantic_rules.py
./scripts/quality/check-openapi.sh
./scripts/quality/check-asyncapi.sh
./scripts/quality/check-target-api.sh
```

C++ 实现及集成检查入口（锁定环境按 doc/11 准备）：

```bash
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset full-locked --parallel 4
ctest --preset full-locked --no-tests=error --output-on-failure
python3 scripts/quality/check-boundaries.py
python3 scripts/quality/check-architecture.py --inner
python3 scripts/quality/check-architecture.py --links build/full-locked/architecture-targets.json
python3 scripts/quality/check-architecture.py --database build/full-locked/compile_commands.json
./scripts/quality/check-format.sh
KIARABOT_TIDY_BUILD_DIR=build/full-locked ./scripts/quality/check-tidy.sh
./scripts/check-toolchain.sh
```

`check-toolchain.sh` 是另一个完整工具链入口，会包含 dev/full 等既有路径，不能把它的结果
混作同一 full-locked 环境的证据；若未运行，须记录原因及显式替代检查。
Python/Panel 按各自锁定环境运行 pytest、协议生成与 `check:contracts`/lint/test/build；
不以未收集测试或仅注册测试当通过。既有格式/tidy 失败须和新增诊断分开，但仍保留
“门禁未通过”的事实；正式发布的要求按 [发布清单](28-p14d-release-checklist.md) 执行。

每个子任务交付至少包含：实际修改文件、公共输入输出/错误/状态、直接文档影响、
实际命令及退出码、未运行项及原因、对应环境/源码版本、残余缺口。
只有本模块局部测试、跨模块真实链路和最终集成检查按范围通过后，才可把整项记为完成。

## 6. 本轮文档影响判断

本轮只把已批准范围和任务组织写入计划，不改变运行时协议、API、配置、权限或业务行为。
需同步：本页、用户决策登记、`doc/23` 原计划入口、`doc/27` 当前待实施状态、
`doc/README` 导航，以及 SDK/Control 文档中“streaming 不做”“目标 WS 延期”的当前范围说明。
旧日期记录保持历史，新的实现与字段状态不提前更新。

本轮无需修改 `proto/`、`api/*.yaml`、SQL、代码、生成配置、运维启动参数、
`doc/18` 或保护清单，因为尚未实际引入可观察行为变化。后续契约与实现任务按第 3–5 节同步。
文档检查与实际执行结果登记在 [当前证据矩阵](27-phase-exit-evidence.md)；
本页列出的构建/测试命令是未来验收入口，不是本轮已经运行的检查。
