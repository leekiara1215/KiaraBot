# C++ Core 设计

T08a 增加同进程 IControlApplication v1 类型化端口与请求形状、默认角色、HTTP 错误映射辅助函数。它接收凭据而非调用者自报身份，真实会话验证/查询/管理实现由 T08b 完成；控制端口与 PluginSession 权限不混用。API/WS 决策以 OpenAPI/AsyncAPI 为准，Core 不依赖 HTTP 框架或面板类型。

T07a tests/acceptance/run.sh 提供独立闭环验收入口。test_closed_loop.py 将真实 Core/SQLite/SDK 与协议 fake 平台相连，验证权限拒绝无平台副作用、重复事件、平台已执行但 ActionResult 丢失后的断线/重启恢复，以及按原 action_id 回放结果。与模型/任务验收一同执行；真实平台联调结果单独由 T07b 记录。

T06c TaskScheduler 实现 IScheduler，规则和事务属于 Core，IANA 时间转换通过 ITimeZoneResolver 端口注入。HostDispatcher 将任务操作交给调度服务；主进程在启动桥之前恢复领取状态，停机先停止并 join 调度线程再停桥。每个 tick 的入队与任务终态同一 RuntimeStore 事务，复用 RuntimeEngine 的 inbox/逐插件投递。

T06b HostDispatcher 通过 IModelProvider 注入模型实现，使用已认证 PluginSession 的 model.chat 权限；调用前提交审计，释放事务/存储锁后调用 Provider，再单独记录结果审计。该外部操作无法与数据库原子提交，不提供自动重试或结果幂等。Bridge 仅在 Provider 配置成功且客户端请求时协商 host.model.v1；基础设施实现和密钥只由启动组合根装配。

T06a HostDispatcher 使用已认证 PluginSession 和 RuntimeStore 事务处理存储请求，默认拒绝未获授 storage.namespace 的插件；业务判断在 Core，SQLite 只保存数据。限额和 TTL 由 Core 决定。该路径复用 RuntimeTransaction 作为统一原子工作单元，不调用彼此独立提交的存储/审计端口；旧 IUnitOfWork 接口仍是后续扩展端口，并非另一套已接线事务。

## P07a-B 目标 Action 实现（2026-11-02）

目标 Runtime 已实现 Action 应用层：`RuntimeActionApplication` 通过 `RuntimeUnitOfWork` 的 actions/action_quota/action_policies/action_evidence 四端口在短 UoW 内完成受理（幂等重放返回原记录）、owner 查询/取消、Control 显式账号 scope 查询、元数据分页、ActionPolicy 读取/更新、结果/证据写入和过期/配额/证据清理。提交顺序为保证 `quota_windows` 外键的 Action 行先插入、再准入持久配额，任何拒绝回滚整个 UoW；提交结果 Unknown 时只按原 action/owner 幂等身份在新 UoW 复核，不重试插入。适配器终端结果通过 `record_terminal` 条件更新返回 RECORDED/DUPLICATE/EVIDENCE_ONLY/CONFLICT，迟到结果只追加证据；非终态证据只追加，绝不改写终态。Bridge 通过 `ActionDispatchPort` 在提交 `dispatched` 条件更新后下发动作帧，通过 `ActionEvidenceSink` 处理 adapter 结果/证据；`RuntimeService.QueryActionResult` 已实现 Control caller + `action.inspect` + 显式账号 scope 的只读查询（插件 owner 查询仍走 PluginBridge）。P07a-C 已用真实 PG16.4 + TLS + Runtime 进程的手工 wire 客户端验收补齐：插件 submit→派发 attempt/sequence→adapter terminal/证据→ActionReceipt→plugin owner 查询，以及 evidence/result 竞争、已派发取消 CONFLICT、重复提交不二次派发、持久配额窗口（第 4 个同群 Action RATE_LIMITED）、adapter 断线重连后补报旧 attempt、EXPIRED 迟到结果 EVIDENCE_ONLY 且不改终态；测试同时挂 p06/p07 标签。target 登记已将真实通过项翻 implemented（Action 帧/messages/fields、Permission 41–46、ActionReceiptDisposition 3 值、QueryActionResult、semantic.action 5 条），未覆盖的 policy RPC/Control REST/Mute-Kick wire 保持 not_implemented。目标契约翻转与 Control REST 接线仍按 P07-C/P12 分阶段推进。

## T04b Action 接线范围

ActionRouter 通过 RuntimeStore 原子保存请求、认证所有者、规范化 fingerprint、派发标志和结果；插件按 Action 类型申请对应权限（`message.send`、`message.react`、`message.delete`、`member.mute`、`member.kick`、`request.approve`）且获授后才可提交。v1 实现 `send_message` 的群/私聊纯文本子集、`react_message` 的群/私聊表情回应（平台中立 token）、`delete_message` 的 bot 自发送消息撤回、`mute_member`/`kick_member` 群管理动作（受会话白名单、每日预算与速率限制约束），以及 `approve_request` 好友请求审批（`request_id` 回填 FriendRequest token），其余 payload/消息段明确 `CAPABILITY_NOT_SUPPORTED`。适配器须声明对应能力（`action.send_message.text.v1`、`action.react_message.v1`、`action.delete_message.v1`、`action.mute_member.v1`、`action.kick_member.v1`、`action.approve_request.v1`）；账号和实例由注册上下文约束。提交、取消、派发和结果更新均记录事务审计；任何提交失败不调用平台。

ActionRouter 在受理可派发 Action 前按 `(Action 类型, 账号)` 与 `(Action 类型, 会话)` 检查进程内滑动窗口配额（`ActionQuota`，默认单账号 10 次/分钟、单群 3 次/分钟、单用户私聊 1 次/10 秒），超限返回可重试 `RATE_LIMITED` 并写审计。配额窗口不持久化，进程重启后重置；账号/会话/窗口键在进程内的跟踪数量有上限，达到上限时淘汰最久未触达的窗口。配额只约束 Core 侧出站，平台侧限频与风控信号仍由适配器错误分类上报。

响应与查询使用 T04a 协议。相同调用方/幂等键复用原结果，内容冲突拒绝，其他调用方不可查询；action_id 在存量记录中全局唯一。规范化内容忽略 action_id、trace_id 和目标连接实例，其余内容（含 deadline）不变。派发前持久化 dispatched，重连可重发同一 Action；适配器必须持久去重。派发后的取消返回 CONFLICT。到期优先 EXPIRED，迟到结果不能覆盖终态；已发生的平台副作用不能由 EXPIRED 撤销。终态保留至少七天，清理由运维任务补齐。

T03b 增加 POSIX PluginProcess 宿主，只管理外部进程，不链接 Python 运行时。子进程采用独立进程组并解除继承的停止信号屏蔽；注册 readiness 以前由 SDK 完成本地初始化。受管插件先于桥接服务停止，宽限期后强制回收。运行中插件退出保持 Core 存活并记录 UPSTREAM_UNAVAILABLE；自动重启与完整健康 API 留待后续任务。

## T03a 插件接收与投递接线

PluginHub 负责可信插件策略、活动连接登记、订阅快照、投递领取与 ACK。AdapterIngress 在入库事务中获取活动插件与已持久化运行状态的交集，按声明订阅创建逐插件投递记录；注册/断线并发修改存储时由 revision 冲突拒绝并重试，不补投给事件接收后才订阅的插件。没有订阅者的事件仍只保存 inbox，后续注册不会隐式追溯订阅。

业务锁只保护状态与事务，不覆盖网络写入。每条插件流有受控发送线程，退出先取消流、停止并 join 发送线程，再移除活动登记、标记不可用并恢复在途投递；清理提交失败保留存储中的在途状态，下一次注册恢复。进程重启没有旧发送线程，注册时可恢复遗留状态。网络服务必须注入支持并发事务的存储（当前 SQLite），单线程内存端口只用于串行单元测试。

## 责任

C++ Core 是系统的业务中枢，负责接收统一事件、执行领域规则、调度插件、管理权限和发出统一 Action。

Core 不负责登录 QQ，也不负责直接调用 AI、数据库或浏览器。

## 推荐技术

第一版建议：

```text
C++20（AppleClang 15+、Clang 16+ 或 GCC 13+）
CMake 3.24+
CTest
Protobuf + gRPC
SQLite
spdlog 或同类结构化日志库
```

具体库可以替换，但必须保持外部协议不变。

## 目录

Core 头文件和实现按 `domain/`（状态与状态机）、`application/`（用例与调度）、`ports/`（端口）、`runtime/`（传输、进程和运行时支撑）分组；进程组合根在 `core/app/main.cpp`。`RuntimeEngine` 通过内部 `RuntimeStatePort` 操作事务私有状态；`RuntimeState` 将快照适配为该端口。持久化端口 `runtime_store.hpp` 与测试和本地开发用的 `in_memory_runtime_store.hpp` 分开。Core 的 CMake 文件负责声明自身源码及日志目标，外层只链接 Core 目标。

```text
core/
  CMakeLists.txt
  include/kiarabot/core/
    domain/
      runtime_types.hpp
      runtime_engine.hpp
      runtime_state_port.hpp
    application/
      runtime.h
      action_router.hpp
      control_application.hpp
      ...
    ports/
      ports.hpp
      runtime_store.hpp
    runtime/
      runtime_state.hpp
      in_memory_runtime_store.hpp
      bridge_server.hpp
      plugin_process.hpp
      ...
  src/
    application/
    runtime/
  app/
    main.cpp
  tests/
    core_smoke.cpp
    ...
```

## Core 拥有的状态和事务边界

Core 拥有：

- BotInstance；
- AdapterInstance；
- PluginInstance；
- User、Group、Conversation；
- Permission、Role、Policy；
- Task；
- AuditLog；
- 事件和 Action 的处理状态。

适配器和插件不能直接修改这些表，必须调用 Core 的应用服务。

Core 的应用服务定义事务边界：一个用例在同一 Unit of Work 中完成状态变更、inbox/outbox 写入和审计记录，然后提交。基础设施只实现 `Repository`、`UnitOfWork` 和迁移执行，不决定业务提交时机。

```text
ApplicationService
  ├─ begin UnitOfWork
  ├─ read/write repositories
  ├─ append inbox/outbox/audit records
  └─ commit or rollback
```

当前 C++ runtime 以 `RuntimeStore` 和 `RuntimeTransaction` 作为 Core 端口：它们负责 inbox、会话顺序、Action 和插件状态的持久化及提交/回滚，Runtime 负责去重、会话顺序、背压、重试、死信查询和重放、Action 生命周期与插件重连状态转换。事件及其会话状态在同一事务写入；写入或提交失败时回滚并向调用方报告失败。Action 重复提交返回原记录及其状态或结果。时间、随机样本和存储均可注入。参考 Runtime 和内存存储要求调用方串行访问；生产存储须提供事务隔离和唯一约束。默认的 `InMemoryRuntimeStore` 只用于测试和本地开发；生产环境必须由基础设施提供持久化实现，不能把内存实现当作生产存储。

宿主在确认旧投递者已停止后调用 `recover_inflight_events()`，将重启前未确认的投递转换为重试或死信，再从 `dispatchable_events()` 取得可投递事件；恢复失败时保留未处理记录并报告存储错误。Action 即使在首次提交时已到期，也须持久化其 `EXPIRED` 结果及幂等键。`RuntimeTransaction` 的失败提交必须不改变持久状态；如果存储不能确定提交结果，则不得将该结果报告为可回滚的失败。

逐插件投递以 `(event_id, plugin_id)` 持久化，事件、会话状态和全部目标插件投递在同一事务提交。插件清单的 `ordered` 随投递记录保存；同一插件和会话按接收顺序等待较早投递进入 ACK 或死信，再开始后续投递。插件独立确认、退避和重放，其他插件不受影响。

会话排序键由 Core 从事件派生：`(adapter_id, account_id, conversation_id, conversation_type)` 的带长度前缀编码，类型来自消息载荷 `ConversationRef.type`，无会话载荷事件取 `TYPE_UNSPECIFIED`。会话水位（最大 sequence、degraded）与逐插件有序投递分组都使用该键；群号与私聊号相同不会共享水位。入口与持久化格式见 `doc/01-runtime-semantics.md`「顺序和背压」「T02a 适配器 inbox 入口」，快照 v6 迁移会重键旧三元组水位。

### 其他模块接入需求

- 插件宿主应在接收事件时确定订阅插件，传入其 ID，并使用注册清单中的 `ordered`；从已认证插件连接确定 ACK 的插件身份，将 ACK、拒绝及可重试错误映射到逐插件操作。连接断开或进程重启后，确认旧投递者停止再恢复未确认投递；重放时使用逐插件 trace ID 更新解码后的 envelope。
- 基础设施需实现新的逐插件投递、持久化接收顺序和原子事务端口，保证 `(event_id, plugin_id)` 唯一以及事件删除时投递记录级联删除；提供隔离以使队列和会话顺序判断与写入原子化，失败提交必须可回滚。现有内存实现不提供生产持久化。
- 控制 API 和面板未来接入死信查询、重放时需携带目标插件身份，并保留其他插件的状态；当前任务不修改其 API 契约或实现。
- PluginSDK 与插件应按 `event_id` 幂等处理重复投递。当前 ACK 协议没有尝试编号，不能区分旧尝试的延迟 ACK；若未来需要精确区分，须先评估并更新公共契约。

## 基础设施端口契约

`core/include/kiarabot/core/ports/ports.hpp` 定义 Core 内部端口，直接使用生成的 `kiarabot.v1` Host 消息和 `Error`，不复制跨语言类型。端口返回 `PortResult<T>`：成功值或已有协议 `Error`，不可把依赖故障伪装成未找到或重复。Core 应用服务负责鉴权、业务策略和事务边界；命名空间及任务所有者由 Core 从认证身份传入，基础设施不能接受插件直接访问。

Unit of Work 将 Repository、命名空间、任务、幂等结果和审计写入绑定到同一事务，显式 commit 或 rollback；销毁未提交事务须回滚。模型端口不暴露凭据，超时与结果通过端口表达。调度端口的领取和完成/重试操作必须持久化，重启恢复与取消语义见 `doc/01-runtime-semantics.md`。审计端口只接受明确的标识和状态字段，不接受消息正文或密钥。文件存储以逻辑键访问，不向调用方暴露文件系统路径。

## 结构化日志

日志端口与 LoggingRuntimeStore 已实现，当前覆盖范围见下方 T10b。每个公开运行时操作及拒绝、重复、超时、重试、死信、存储失败和状态转换路径均有结构化记录是持续的覆盖目标，不表示已逐路径验证。记录至少包含：

```text
level
operation
outcome
event_id / action_id
trace_id
resource_id（插件或其他资源操作）
```

日志只记录标识和状态，不记录 Token、API Key、消息正文或平台原始 payload。基础设施负责把日志端口接到 JSON sink、文件或集中式日志系统；Core 不绑定具体日志库。腾讯 [BqLog](https://github.com/Tencent/BqLog) 可作为候选实现评估，但当前仓库不引入该依赖。

## 消息处理流程

```text
接收 EventEnvelope
  ↓
校验 schema、来源和幂等键
  ↓
加载用户、会话和权限策略
  ↓
匹配插件订阅
  ↓
向插件发送事件
  ↓
校验插件返回的 Action
  ↓
执行限流、审计和能力检查
  ↓
发送到渠道适配器
```

任何一个步骤失败，都返回稳定错误码并记录 `trace_id`。

## Core 内部接口

Core 内部接口只在 C++ 内使用，不暴露给插件：

```cpp
class EventRouter {
public:
    void publish(const EventEnvelope& event);
};

class PolicyService {
public:
    Decision authorize(const Subject&, Permission, const Resource&);
};

class ActionDispatcher {
public:
    ActionResult dispatch(const Action& action);
};
```

跨进程接口必须使用 `proto` 定义，不能把这些 C++ 类直接暴露给 Python 或 TypeScript。

## Core 不做的事

- 不读取 OneBot 原始消息；
- 不导入 NoneBot2；
- 不读取插件的数据库文件；
- 不把模型 API Key 发送给插件；
- 不在前端实现权限判断；
- 不根据 QQ 特有字段编写业务分支；
- 不允许插件直接执行任意 Action。
- 不在 Core 中直接写 SQL；
- 不把数据库事务对象跨进程传给插件或面板。

## 健康检查

Core 健康状态至少包括：

```text
协议服务是否监听
数据库是否可写
插件运行时是否可用
适配器连接数量
待处理事件数量
最近一次成功 Action 时间
```

## 验收标准

- Core 可以在没有 NapCat 和 Python 的情况下启动；
- Core 可以接收模拟事件并返回模拟 Action；
- 权限拒绝不会调用适配器；
- 重复事件不会重复执行 Action；
- 一个插件进程退出不会导致 Core 退出；
- Core 可以通过健康检查报告故障原因。

## 代码启动句

> 请只实现 C++ Core 的事件接收、幂等检查、事件路由、权限调用和 Action 分发骨架，使用模拟适配器和内存存储完成独立测试，不接入 NoneBot2、NapCat 或真实模型。

## 进程入口（T01a）

`kiarabot-core` 无参数时在 POSIX 平台持续等待 SIGINT/SIGTERM，收到后正常退出（0）。这仅提供进程生命周期，不表示 RPC、存储或健康服务就绪，不输出 ready。`--check` 运行原有内存启动检查后退出；`--help` 输出用法。参数错误返回 2，初始化/等待错误返回 1。非 POSIX 平台暂不支持无参数运行，明确返回 1；`--check` 和 `--help` 保持可用。

信号在主线程初始化时阻塞并通过 sigwait 同步消费，避免异步处理器修改业务对象。未来工作线程必须在信号屏蔽后创建，并在结束前停止和 join。无参数模式没有业务工作线程、端口监听或存储接线；显式 gRPC 模式见下节。进程级测试在 POSIX 上用 Python 3 驱动独立子进程，验证持续存活、两个停止信号和参数拒绝；该无参数 Core 模式不依赖 Python；受管插件模式可启动独立的 Python SDK 进程。

## 本机 gRPC 入口（T01b）

proto 构建支持 `kiarabot-core --grpc-listen 127.0.0.1:50051`（允许端口 0 自动分配）。仅接受字面 IPv4 回环地址，拒绝外部地址。凭据从环境变量 `KIARABOT_ADAPTER_ID` / `KIARABOT_ADAPTER_TOKEN`、`KIARABOT_PLUGIN_ID` / `KIARABOT_PLUGIN_TOKEN` 读取；至少配置一对，成对提供，ID 1–256 字节，token 为 32–4096 字节可见 ASCII 非空白字符且不能重复。角色与 ID 在启动时固定；修改需重启。不要将 token 放进命令行或日志。明文回环模式只用于本机开发，不能用于跨主机生产部署。

端口占用或配置无效返回进程码 1；dev 构建没有 gRPC 功能，传入该参数返回 2。监听成功只输出 transport_listening=true，业务 ready 仍为 false。SIGINT/SIGTERM 触发立即取消流并 Wait；服务对象先于业务服务对象销毁。启动屏蔽信号后才创建 gRPC 工作线程。未配置持久化入口时只实现传输与身份边界，不接受业务；T02a 配置接线见后文。错误行为见统一契约 T01b 节。

## T05a 统一事务快照端口

当前公共 Runtime 通过 RuntimeStore::read/begin 和 RuntimeTransaction::snapshot/commit 访问基础设施。所有公共查询和写操作返回 StorageResult<T>（共享 Result<Value, Error> 即 std::variant）；成功的空 optional 表示不存在，StorageError 的 Unavailable/Conflict/Corrupt 明确表示依赖故障、并发冲突或数据损坏。边界服务将其分别映射到 UPSTREAM_UNAVAILABLE、CONFLICT、INTERNAL，不允许以空记录或 false 吞掉存储错误。Host PortResult 使用同一个 Result 约定及已有 Protobuf Error。

事务提供独立快照，销毁未提交事务丢弃修改；提交失败不改变持久状态。完整快照包括事件、逐插件投递、会话水位、Action、插件和投递序号。Runtime 单次调用以一个快照提交，不自动重试冲突；批量调用现在整体提交，不再部分持久化。调用方须检查外层 StorageError 后才能使用内层业务结果。公共 Runtime 串行访问；存储实现须保护不同事务之间的并发冲突。

原内存状态机移至 detail::RuntimeEngine，仅操作事务私有内存。其 bool/optional 不是数据库端口，故障由外层显式返回。原 ports.hpp 的不完整 IRuntimeRepository 移除，Host UnitOfWork 直接暴露同一 RuntimeSnapshot；Host 存储/幂等/审计组合事务已由 T06a 使用 RuntimeTransaction 实现。完整快照是首版正确性实现，会复制全部状态，规模较大时应以同样事务语义优化，不能作为吞吐性能保证。

## T10a 受保护的应用命令

AuthorizedCommands 是 Core 内部入口：调用方必须从已认证连接构造 CommandContext，并由服务端计算规范化请求的 SHA-256 fingerprint（64 位小写十六进制），不能相信插件自报身份、权限或 fingerprint。静态 PermissionGrant 按 subject/permission/resource 精确匹配，只有配置中的资源 `*` 表示全部资源；没有匹配规则时默认拒绝。此入口不是跨进程 SDK。

execute 的回调只能修改传入的 RuntimeSnapshot，不得执行网络/平台副作用或另启 Runtime 事务。业务修改、成功幂等记录和审计在同一提交中完成；提交失败全部不生效。幂等范围为主体+权限+资源+键，同键同 fingerprint 重放成功且不重复审计；不同 fingerprint 返回 CONFLICT。重放前重新鉴权。拒绝及回调失败记录审计，失败回调的状态变更被回滚；错误详情只保留稳定码和请求标识，不转发回调的自由文本。审计满（默认十万条）时返回 UPSTREAM_UNAVAILABLE 且不执行回调。审计/幂等保留和清理策略须后续管理任务落实，不能当作无限容量服务。

SQLite 从 v2 起保存审计和命令幂等记录；当前 schema v6 还保存 Action 路由、Host 命名空间/幂等响应、任务、Action 审计目标（账号/会话）与每日 Action 预算计数器。模型和任务已接线。T02a 适配器桥和 T03a 插件桥已使用此入口；控制 API 已由 T08b 通过独立应用端口接入同一事务存储。

## T02a 适配器接收应用入口

AdapterIngress 依赖 RuntimeStore 和宿主提供的 AdapterPolicy；负责注册账号权限、实例替换、输入校验、命令授权审计、事件去重与事务提交。BridgeServer 仅绑定认证身份并转换流消息，不从注册载荷生成权限。实例状态按连接生命周期管理，事件/幂等/水位持久化到 RuntimeStore。无持久化入口的 BridgeServer 继续显式拒绝业务，不发送假确认。订阅路由、插件执行及 Action 分发由后续任务实现。

## 控制应用服务

T08b 在 Core 应用层实现 IControlApplication：认证、角色权限、会话、配置版本、幂等和审计事务；control/api 仅做 HTTP 映射。保留命名空间 `$control` 存放加密状态，`$control.enabled` 保存插件投递开关，`$control.manifests` 保存注册后的公共清单。插件身份不能含 `$`，Host API 不可访问这些命名空间。运行时更新仍须完整保留 namespaces。控制状态与审计提交失败时没有新副作用，读取故障不能伪装成空资源。

T08b 后续解耦：`IControlApplication` 除 `invoke()` 外还暴露 `readiness()` 与 `stream_snapshot()`，默认实现分别返回 UPSTREAM_UNAVAILABLE（HTTP 503）与 CAPABILITY_NOT_SUPPORTED；`control/api` 传输层改为通过这两个接口访问，不再用 `dynamic_cast<ControlService>` 依赖具体实现。行为不变：健康探针与事件流快照语义保持原样。

## T10b 结构化运行日志

LoggingRuntimeStore 装饰现有 RuntimeStore，不改变事务结果；提交后按新增审计、事件/投递/Action 状态产生结构化记录，覆盖成功、拒绝、retrying、dead_letter 和存储故障。日志端口不接收事件正文、Host 值、配置值或凭据，只接收操作、稳定结果和关联标识。sink 故障计数并 fail-open，不把已提交操作报告为失败；需要阻止副作用的审计仍在原业务事务内。

## 维护循环与容量上限（W2）

组合根在持久化装配下启动维护循环（`RuntimeMaintenance`，`core/include/kiarabot/core/application/runtime_maintenance.hpp`），默认每 60 秒一次，并在启动时先执行一次。每次维护在同一个 `RuntimeTransaction` 内完成：删除超过保留期（默认 168 小时、不得低于 24 小时）的终态事件及其逐插件投递记录、终态 Action；按保留期与水位裁剪审计；按数量上限裁剪命令幂等记录；清理过期 namespace TTL 条目、过期 Host 幂等响应和过期每日 Action 预算计数器。只有确实删除记录才提交，事务提交失败不改变持久状态；与共享存储的 revision 冲突按维护周期重试，不自动重试、不影响业务请求，失败与每次实际清理量都以 `operation=maintenance` 结构化日志输出。

容量与超时由组合根注入，不再全部使用硬编码默认值：`KIARABOT_EVENT_QUEUE_LIMIT`（1–100000，默认 1024）、`KIARABOT_AUDIT_LIMIT`（1000–10000000，默认 100000）、`KIARABOT_ACTION_LIMIT`（100–1000000，默认 10000）、`KIARABOT_ACK_TIMEOUT_MS`（1000–600000，默认 30000）、`KIARABOT_MAINTENANCE_INTERVAL_MS`（1000–3600000，默认 60000）、`KIARABOT_RETENTION_HOURS`（24–8760，默认 168）、`KIARABOT_ACTION_ACCOUNT_LIMIT_PER_MINUTE`（1–100000，默认 10）、`KIARABOT_ACTION_GROUP_LIMIT_PER_MINUTE`（1–100000，默认 3）、`KIARABOT_ACTION_PRIVATE_LIMIT_PER_10_SECONDS`（1–1000，默认 1）、`KIARABOT_ACTION_DAILY_LIMITS`（逗号分隔的 `<action kind>=<limit>`，limit 1–100000，默认空=不限制）、`KIARABOT_ACTION_ALLOWED_CONVERSATIONS`（逗号分隔的 `<action kind>=<会话>|<会话>...`，默认空=该 kind 不额外限制；出现的 kind 只允许白名单会话）。非法值以 `operation=runtime_config outcome=failed error_code=INVALID_ARGUMENT` 退出 1。审计/命令上限的维护水位为上限的五分之四，保证写入方始终有头部空间；审计达到硬上限时 `AuthorizedCommands` 仍返回 `UPSTREAM_UNAVAILABLE`（可观测的稳定错误），快照超过存储实现上限时提交返回 `UPSTREAM_UNAVAILABLE`。命令幂等记录没有时间戳（快照格式只保存 fingerprint），因此按确定性数量上限裁剪；业务幂等仍由事件、Action 与 Host receipt 记录保证，被裁剪项重放会重新执行幂等回调并追加一条审计。

插件投递新增 ACK 超时：`dispatched` 记录超过 `KIARABOT_ACK_TIMEOUT_MS` 未确认时按一次可重试失败处理（进入退避；达到最大尝试次数后进入死信），使挂起插件不再永久阻塞该插件的投递队列。桥接层对 `retryable=true` 的 `CONFLICT` 在事件 ACK、配置 ACK、Action 提交/查询/取消、适配器 Action 结果与 Host 请求路径做 3 次 20ms 重试；重试预算耗尽后仍按原断连语义处理，断连原因由稳定错误码表达。控制应用 `stream_snapshot` 增加会话用户查找失败的防御性判断（返回 `UNAUTHENTICATED`）。

未完成项：`GET /groups` 仍是有策略无数据源（由控制 API/契约分支补数据来源）；任务每插件上限仍固定 1024；多适配器/多插件装配、完整 RBAC 与速率/内容策略不在 W2 范围。时间型命令幂等过期需要存储快照格式版本升级，属基础设施交接项。

## 配置下发闭环

配置通过 plugin.config.v1 / PluginConfig / PluginConfigAck 下发并按连接确认。REST GET 的 delivery 区分持久化与运行实例应用，PUT/幂等重放只确认持久化。状态、超时、重连、错误及事件门控的完整规则见 [PluginSDK 配置下发闭环](03-plugin-sdk.md#配置下发闭环t08b-后续)。协议唯一来源为 proto/kiarabot/v1/plugins.proto，REST 为 api/openapi.yaml；无新增错误码或 HTTP/gRPC 映射。

## 目标内层规格（P00 / P01b，尚未接线）

目标接口位于 `domain/target`、`application/target`、`ports/target`，命名空间为
`kiarabot::core::target`。它们只依赖 C++20 标准库，无传输、生成协议、JSON、SQL
或旧 RuntimeSnapshot 依赖；当前运行路径未替换，后续统一接线时移除旧接口。

`domain/target/foundation.hpp` 提供强类型稳定资源/安装/实例、执行/连接身份和分离代次、
所有者版本、operation 与失败上下文。ID 值由可信边界验证和所有者分配，本层不生成 UUID。
`Result<Value, Error>` 保持现有 variant 约定，目标 `Outcome<Value>` 固定内部 Failure，
通过独立命名空间避免引用旧 runtime_store.hpp；不是另一个生产存储实现。
失败区分未受理/已受理/未知及同身份查询等恢复动作；未知提交不能当作已回滚。
稳定业务码对应 doc/21 的目标矩阵，HTTP/gRPC 映射由后续边界实现。

独立验证：`cmake -S core/tests/architecture -B /tmp/kiarabot-core-inner`，
`cmake --build /tmp/kiarabot-core-inner`，
`ctest --test-dir /tmp/kiarabot-core-inner --output-on-failure`。根构建尚未接入。

`domain/target/context.hpp` 定义认证/持久交接上下文，
`application/target/context.hpp` 定义命令上下文，不携带 JWT、证书、
秘密或浏览器 token；认证上下文只能由可信边界构造，权限字符串须匹配目标封闭词表。
`ports/target/unit_of_work.hpp` 的 Repository 查询用成功空 optional 表示不存在，
写入 null expected 仅创建，更新必须条件版本。Repository 借用 UoW 生命周期，
不自行提交、不跨线程使用；应用服务持有唯一 UoW，析构活动事务必须回滚。
commit 明确返回 Committed/RolledBack/Unknown，Unknown 只能按原身份查收据，
不得据此重做外部调用。收据/操作/审计/outbox/inbox 属同一本地原子组。
`business_ports.hpp` 为事件/投递、Action 配额、namespace CAS、occurrence、模型意图
保留类型化端口；方法只参与所属服务本地事务，无实现、无跨库访问。后续业务阶段
需添加服务专属 UoW 组及真实数据库测试，当前不是完整业务/存储端口验收。

`domain/target/invariants.hpp` 提供条件版本递增（拒绝缺省、冲突与溢出）、完整
execution/connection fence 校验及单 attempt 租约规则。租期初始 30 秒，续租到
min(now+30s, start+300s)，到期相等边界拒绝，取消/到期不复活。ACK 先成功后
timeout 不覆盖；同 attempt/current fence 重复成功 ACK 返回 Duplicate；旧 attempt
或代次返回 Conflict。时间早于首次领取拒绝。所有时间由调用方注入权威数据库 UTC，
这些纯值操作无持久化/锁；竞争时须在同一 delivery 行事务内检查并保存。

Fence 同时绑定稳定 instance、runtime_epoch、execution/connection 身份及两种代次；
Runtime 重启更换 epoch，跨实例及旧 epoch 即使代次相同也拒绝。与契约规格
112ec27 的 required_fence 一致；额外执行/连接 ID 不是替代必需字段。
ExecutionOutcome 分 NotStarted/KnownSucceeded/KnownFailed/Unknown，与 operation
进度分离。UnitOfWork 仅提供事务/收据/journal 公共能力，RuntimeUnitOfWork 才暴露
实例及 operation 仓储；Identity/Scheduler/Model 应定义各自专属仓储组。

### 直接调用者与 fence 映射边界

AuthenticationContext.direct_peer 使用 DirectPeer（PlatformPeer / AdapterPeer /
PluginPeer），区分直接平台服务、Adapter 和 Plugin。各分支均携带直接认证主体，
Adapter 绑定稳定 adapter 资源，Plugin 绑定稳定 instance；这些值来自边界验证后的
注册绑定，不能从请求自报字段直接构造。AuthenticationContext.subject 则是本次
代表的业务主体，可与直接调用者不同；委托链继续由 actors 表达。
Service 仅包含六类平台服务，audience 与 HandoffContext.source 继续使用 Service，
不能把 Adapter/Plugin 伪装成 Runtime 或扩入持久 source_service 词表。

Fence 的 execution/connection ID 为 Runtime 内部关联标识，线协议无需据此新增字段。
Runtime 接收边界先验证流的认证主体及注册绑定，再将收到的 instance、runtime_epoch、
execution_generation、connection_generation 与当前可信注册状态逐项匹配；仅在匹配后，
从 Runtime 所有者维护的执行记录/连接登记取出对应 execution_id/connection_id。
受管执行登记与已验证 Host 观察关联，外部自管执行由 Runtime 的获准注册流程登记，
connection_id 由 Runtime 接受连接时分配；均不能用客户端自报 ID、代次字符串或随机
占位补全。缺失/过期绑定必须拒绝，不能用当前代次替换入站旧值；后续 delivery 事务
仍须重检 gate/current fence，不能只依赖边界的早先快照。此处只冻结映射要求，
认证、注册存储与传输映射实现仍待后续阶段。

## P02 独立服务就绪端口

`ports/target/service_readiness.hpp` 定义纯标准库 ServiceReadinessPort，服务类型沿用
六平台 Service；依赖状态与 ready 同时表达。P02 application 业务未实现，始终包含
BusinessImplementation/NotImplemented 且 ready=false，不以监听成功当业务就绪。
公共 runner 声明位于 `runtime/platform/service_process.hpp`，命名空间
kiarabot::core::platform；借用 application 至线程、socket、RPC 全部关闭。

五个独立 application 工厂为 make_identity_application、make_runtime_application、
make_host_application、make_scheduler_application、make_model_application，头/源位于
application/target/<name>_application.hpp/.cpp，返回 unique_ptr<ServiceReadinessPort>。
每个实现拥有自己的服务身份、未验证依赖清单及业务未实现 gate，不装配旧 Runtime
或 Control 对象；对应 core/app/<name>_main.cpp 只装配 application 与公共 runner。

### P02 实际传输边界

`runtime/platform/service_rpc.cpp` 注册 IdentityService、RuntimeService、HostService、
SchedulerService、ModelService 的生成 Service 默认实现；Runtime 另注册 AdapterBridge、
PluginBridge、HostBridge。全部业务方法返回 UNIMPLEMENTED，不执行授权成功分支或写状态。
Control 不创建入站业务 gRPC。生成协议及 gRPC 只存在外层，五个 application 无协议依赖。
P06 已冻结并已实现的扩展：Runtime 在同一进程内增加可选外部 `bridge_listen`
（server-verified TLS，不请求客户端证书），只注册 AdapterBridge/PluginBridge；
`grpc_listen` 的 owner mTLS 监听、HostBridge 与既有服务不变。外部监听器由
`ProcessConfig.bridge_listen` 开关，缺 `bridge_listen` 而注册 bridge 服务时组合失败。

`service_process.cpp` 在创建 gRPC 工作者前阻塞 SIGINT/SIGTERM，在主线程同步检查并
sigwait 消费，不安装执行业务的异步信号回调。HTTP 探针单线程非阻塞轮询，每连接
读写共用500ms期限及8KiB头边界；RPC shutdown 使用2秒期限取消在途调用并 Wait，
借用 application 生命周期覆盖所有资源销毁。未实现阶段不启动业务工作者/进程/数据库。

## P03a 服务本地关系端口（接口已定义，数据库未接线）

目标端口继续位于 `core/{domain,application,ports}/target`，只依赖标准库。
ResourceRef 保存 kind+id，OperationRef 保存 owner service+operation id+完整资源引用；
授权scope、命令、收据、交接及审计沿用该引用，不将相同字符串的不同资源种类混合。
SHA-256以32字节内部值表达，计算/传输映射留在边界，不由数据库决定请求内容。

`domain/target/service_records.hpp` 定义最小主体/会话/授予/授权水位、账号/安装/实例、
任务/occurrence和模型意图。`persistence.hpp` 定义共享成功收据、审计与交接值类型。
ReplayResult 是可重放的完整类型化成功快照（空结果、版本结果、operation或对应记录），
不依赖可变业务行回查，不含请求ID或传输对象；重放先重新鉴权并使用本次请求ID。
登录/刷新/一次性秘密交付需要P04单独加密收据接口，不可使用本普通非秘密成功收据。

ReceiptRepository按(subject,method,resource kind+id,key)唯一，记录fingerprint、完整成功
结果、created_at、retain_until；普通成功至少保留7天，默认上限10000条。相同请求返回
原快照及duplicate，不续期/重复审计；异内容Conflict；容量满RateLimited，不淘汰有效记录。

JournalRepository在同一本地UoW里组合audit/outbox/inbox。outbox键为source+message，
保存不可变envelope/hash与带版本的不可变payload引用；义务未完成前所有者须保留载荷。
普通队列默认10000条，控制保留1024条，inbox默认10000条，均由内部容量参数配置。
claim显式携source、数据库UTC、批量1–100及0<lease<=30秒，保存安全唯一token/租期；
confirm携完整source+message、token和now，旧/过期claim不能确认新领取者。100次派发后
NeedsAttention，禁止静默删除或换ID自动重发。相同envelope/hash重复enqueue不改变原期限。
inbox保存首次接收时间、完整成功结果，至少保留max(首次接收+7天,valid_until+7天)；
同键异内容Conflict，同内容重放不延长保留，清理后再次收到过期消息仍拒绝新执行。
普通outbox满不能阻止Identity撤销：AuthorizationState提供持久dirty/version合并标记；
不能以普通队列挤占控制容量，也不能删除已受理记录来腾空间。

所有UoW共有receipts/journal/operations，派生组分别为：Identity的principals/sessions/
grants/authorizations，Runtime的accounts/installations/instances，Scheduler的tasks/
occurrences，Model的model_intents。各自工厂返回对应unique_ptr<UoW>，begin签名不携
框架/数据库对象；仅接收所属库，不为Identity/Model提供Runtime仓储。更复杂的注册、
配置、事件、动作、namespace、凭据、模型配置仓储在所属阶段追加，P03不要求提前实现。

P06a 追加 Runtime 事件账本：纯领域规则在 `domain/target/event_delivery.hpp`（四元组
水位与跳号 degraded、fan-out 目标选择、30s 租约/300s 上限、1/2/4/8/16s±20% 重试与
死信、ACK/Renew 的 Acknowledged/Duplicate/Stale/Rejected 判定、容量与重启恢复）；
`RuntimeUnitOfWork` 新增 `events()` 端口，PG 实现在同一事务内完成事件/水位/目标/
attempt 的接收、领取、ACK、续租、超时扫描、恢复、死信查询、重放、容量与保留清理。
Runtime schema 在 P05 合并后升到 v8，并新增五张事件/投递表（events、conversation_watermarks、
delivery_targets、delivery_attempts、ordered_delivery_heads）；开发阶段无迁移，
v5 库必须清空重初始化，target 仍在 Bridge 接线验收后才翻转。

Repository查询成功空optional表示不存在，故障必须Failure；save的空expected仅创建，
更新要求当前版本匹配及下一版本。唯一约束与条件版本由实际数据库事务执行，不能将
纯值测试当作并发验证。应用服务控制原子组，Repository不commit、不持锁调用网络。
commit为终止操作：Committed/RolledBack/Unknown均释放资源，Unknown不能假定回滚、
不能自动重跑，必须用原收据/交接身份新开UoW查询。析构只回滚仍活动未提交事务；
显式rollback不能撤销Unknown。四库隔离/真实锁/容量/提交未知仍须P03数据库验收。

UoW::current_time()以Outcome返回调用时的权威数据库UTC，失败不得回退本机system_clock；
不能使用被数据库固定在事务开始时的CURRENT_TIMESTAMP冒充调用时刻。该值用于应用
记录时间与传参，租约/到期/claim条件写仍必须由实现用写入时的数据库当前UTC原子重检。
本地短事务预算2秒、锁等待250ms；不能跨事务/网络等待复用采样，也不能让客户端建议
决定租约。ReceiptCapacity/JournalCapacity只来自可信应用配置，不透传客户端容量参数。
版本继续使用uint64_t，不经double/JSON数值转换，任务/模型记录不损失高位。

### P03 四个进程的实际数据库装配

Identity/Runtime/Scheduler/Model入口通过基础设施公开工厂打开自己的typed UoW，
只持本库runtime_role凭据；连接、schema和唯一writer校验完成后才允许监听。
组合根持有工厂的生命周期长于application；内层不接收ProcessConfig、SQL或PG连接。
依赖维护通过同一工厂begin/current_time/rollback探测，避免绕过其同步或另建writer连接。
250ms忙返回Conflict不认定依赖丢失；其他依赖故障使进程停止，不能自动重连续跑。
readiness只读取缓存观测，数据库成功不放行尚未实现的业务依赖。缺database配置时
注入明确返回UpstreamUnavailable的端口，不创建内存/SQLite业务备用库。

### P04 业务 RPC 的外层装配

ServiceComposition可提供configure_rpc回调，在配置、依赖验证之后且任何监听之前，
向本进程ServiceRpc登记受审的外层gRPC适配器。generated服务类型只出现在runtime/
platform、组合根与传输测试中；application/ports仍使用独立typed内部模型。
ServiceRpc拥有适配器并先停止/等待server，再销毁适配器，最后才释放application与
依赖。登记空适配器、owner不符或启动后登记拒绝；组合失败/异常脱敏退出1。
无自定义适配器时保留所属owner的generated UNIMPLEMENTED表；注册适配器本身不能
跳过mTLS、业务授权或提升readiness，未覆盖方法仍由generated默认拒绝。

### P03a 公共持久规则的纯验证

`domain/target/persistence_rules.hpp` 提供无状态校验与决策，不维护内存数据库。
空记录指针仅代表调用方已经确认的数据库成功缺失；读故障不能转为空指针。
收据按完整scope+fingerprint判断新建/重放，重复时忽略调用方新给出的结果/期限并要求
返回原快照，容量满不阻止原结果重放。新收据至少保留7天，已过期/未来创建时间拒绝。

交接同时比较source+message、hash与不可变上下文/载荷版本；改变有效期或载荷指向也
是Conflict。新消息在now==valid_until时拒绝，即使去重记录已被清理；已持久接受的
同内容消息在到期后仍可返回原确认事实，不能把重放变成新执行或续期。inbox保留计算
使用max(received_at,valid_until)+7天，所有时间加法检查溢出。控制lane独立核对其保留
容量，不能以普通lane满为由挤占或删除既有工作；记录数量必须由实现锁定后权威读取。

claim请求校验owner、批量1–100及0<lease<=30秒。纯函数只提出带新token/次数/租期的
候选值，输入保持不变；数据库必须先原子保存才能发送。尚存活租约不能接管，过期
重领不能复用旧token，租期不超过消息有效期；满100次返回NeedsAttention而不删除。
确认使用完整source+message及当前token，精确租期边界拒绝；已确认的相同token返回
Duplicate事实。缺token等损坏状态返回Internal，不能当作空队列。所有校验都不授予
权限，真实库仍须条件写、唯一约束、并发锁与当前DB UTC重检，纯测试不能替代PG验收。

## P07 目标 Action 应用与端口（未实现）

P07a 在 Runtime application 内实现 submit/dispatch/result/cancel/query/policy 的单 UoW 原子组；Runtime 私有 PG 新增 `actions/action_idempotency/quota_windows/daily_budgets/adapter_result_evidence/action_policies`，`RuntimeUnitOfWork` 追加 `actions()/action_policies()/action_quota()`。派发是提交后的流写，不在 UoW 内等网络；结果/取消/到期用单行条件更新，dispatch 扫描只领取 due/current 记录。审计复用共享 `audit_entries` 的 `target_account_id/target_conversation_id` 扩列（P07 与 P06/P09/P10 协调，不重复定义）。不新增服务、进程或端口。

## P08 目标 Host 应用与端口（P08b-2b 传输服务端已实现，持久化/P08b-3 分阶段）

P08b 在 Runtime application 内新增 HostBridge 入站应用：内部 mTLS 注册（证书→预注册 `host_id`、连接代次分配）、心跳（5s/15s 在线判据，只更新 `online/last_heartbeat_at` 不 bump 版本）、观察 inbox（source=Host 去重 + P05 `HostRegistration`/`InstanceConfiguration` 条件更新）、Host 命令下发（P05 outbox/desired version；`ApplyDesiredProcess`/`StopExecution`/`DrainHost` 用 `command_id`+content hash 幂等）、跨主机 gate（旧执行未确认时 operation `BLOCKED`）。HostBridge server 注册在既有 owner mTLS `grpc_listen`，不新增进程/端口；`HostService` unary 方法列仅作方法目录，不在外部监听。Host 侧进程事实与恢复归 Host 进程（`host_application`/SQLite `command_journal`/`managed_executions`/`execution_observations`），Runtime 只存投影与观察，不解释 marker。配置应用证据按 P08 规则持久 `applied_version/apply_state`、delivery 易失。实现前 P05/P06 的 Host 记录与配置投递仍为 WIP，本契约不改变其 owner。

**P08b-2b（已实现，传输层）**：`core/src/runtime/platform/host_bridge_server.{hpp,cpp}` 实现 `v1::HostBridge::Service::Connect` 同步 bidi：首帧必须为 registration，服务端用 `extract_peer_certificate(*ServerContext::auth_context())` 取内部 mTLS 叶证书 SHA-256，交给 `RuntimeHostApplication::register_host` 与部署 `hosts[]` allowlist（`certificate_sha256 → host_id`，空 allowlist 拒绝所有）匹配，回 `registered{host,fence}`；此后每帧必须携带精确当前 fence，旧 generation/缺失 fence 以 `CONFLICT` failure 结束流；heartbeat 经 `RuntimeHostApplication::heartbeat` 更新 `last_heartbeat_at`/online 并回 `heartbeat_ack{fence,received_at,interval=5s,resync_required}`，cursor epoch 不匹配强制 resync；inventory/receipt/observation 做帧校验并交给可选 P08b-3 回调（当前 composition 未接持久化回调，只有传输校验）。注册认证失败映射稳定 gRPC `PERMISSION_DENIED`/`UNAUTHENTICATED`，注册后业务失败映射 `RuntimeToHost.failure` 的稳定 Error。服务端只由 Runtime composition 注册在 owner mTLS `grpc_listen`，替换 generated `HostBridge` 占位，不占用 `bridge_listen`。配置 only Runtime 允许 `hosts[]`；PG `HostRegistration` 持久化接线与真实 Host 二进制、PG 证据留 P08b-2c，当前 composition 用进程内 allowlist/registration/connection store（重启不保留，显式未完成）。

**P08b-3（部分实现）**：Runtime UoW 新增 `host_commands()`（默认 fail-closed），PG 实现
`host_commands` outbox 与 `host_observations` inbox（Runtime schema v14）；P05 的
`CreateInstance`（带 host）与 `SetDesiredState` 在同一事务内写确定性 `hc-<payload sha256>`
命令快照（kind/generation/desired version/package sha/launch profile/payload hash），重复插入
同 payload 幂等、同 id 不同 payload CONFLICT；受管实例与外部实例均生成一次性 bootstrap，
Runtime 只存 SHA-256、明文仅出现在部署方 CreateInstance 响应。**未完成**：HostBridge server
从 outbox 下发 apply/stop/drain、receipt/observation 帧写回 PG、重连重放/resync、restart/delete
命令与 Host 凭据目录自动预置；因此 command 的发送/确认仍没有端到端证据。
**P08b-3/3a（已实现传输闭环，3b 真实 Host/凭据待完成）**：Runtime UoW 新增 `host_commands()`
（默认 fail-closed），PG 实现 outbox 与 observation inbox（schema v16，含 installation 快照）；
P05 `CreateInstance`（带 host）与 `SetDesiredState` 在同 UoW 写确定性 `hc-<payload sha256>`
快照，重复幂等/异 payload CONFLICT。HostBridge server 在注册和每次 heartbeat 后按当前
connection_generation 读取 pending，下发 apply/stop/drain（含 command_id/generation/
installation/desired version/package/launch profile/args/env 与 payload hash），写入成功后标记
Dispatched；receipt 帧按 (command_id, connection_generation) 条件更新，旧 generation/不一致
receipt 返回 FAILURE，重复相同 receipt 幂等；observation 帧按
(host,connection_generation,cursor.sequence) 去重写入，校验 cursor epoch/generation，重复帧与
resync 重放幂等；新 connection 重放未 receipt 的同一 command_id。30s 未回执转 Unknown（不写
stopped），P13a 60s 清理批次执行 7d/30d 保留。**P08b-3b/3e**：`p08_host_bridge_e2e` 用真 PG16.4+真 mTLS Runtime+fake Host 覆盖注册→
apply 下发→receipt/observation 落 PG→去重→重连 generation→旧 fence；`p08_managed_host_e2e`
进一步用真 `kiarabot-plugin-host`+真受管 SDK 插件子进程覆盖 Host 注册/心跳→预置 host_command
apply→Host spawn 插件（P08d helper 0600 凭据/白名单 env）→插件经 PluginBridge 注册→
plugin on_start/on_stop→Runtime receipt/observation 落 PG→Stop→Host 重启不重复 spawn→Drain；
断言插件进程退出、journal 终态、凭据不泄露。真实 e2e 修复了 Stop/Drain handoff payload 未写
（导致 Host 拒绝 stop）的缺陷。Runtime schema v16（descriptor
`ec12933ab8bff61620db24ebcb05ef44`/152）。**P08f 退出矩阵（已完成）**：`p08_managed_host_e2e` 增加包摘要/参数/environment 白名单
拒绝（receipt persisted=false、无 spawn/journal）、Host 启动后 SIGKILL 重启认领活动子进程且
不重复 spawn、命令在 Host 停机期间到达并重连只 spawn 一次、忽略 TERM 的进程在 grace 后被
KILL 且记录终态；`p08_host_bridge_server` 增加旧 generation receipt 拒绝、重复 receipt、重复/
乱序 observation 转发语义；PG 检查增加乱序 observation 去重/有序读取；`p08_config_conflict`
覆盖 above-desired 拒绝、重注册后旧版本 ACK 不覆盖 applied。另修复 Host supervisor `alive`
在判定前不 reap 已退出子进程导致停止路径卡在僵尸 PID 的缺陷（P08f e2e 三连跑稳定）。P08 退出判定：**通过**（真实 QQ、
10s 上限仅以 5s 生效路径验证等非阻塞边界见模块报告）。
**P08d（部署 helper + Delete 快照）**：新增 `scripts/deploy/target_host_credential.py`，从
CreateInstance/BindAdapter 一次性响应生成 Host `credential_directory/<instance_id>.bootstrap`
（0600/atomic/幂等/显式轮换/清理/verify，不落日志/argv，见 doc/08），补上“自动预置”的部署
约定而不改协议；P13c 的 Delete Host 参与者 StopExecution 快照补齐 installation/package/
desired state 字段，Runtime 重启后同一 command_id 重放幂等、无已知 receipt 仍 BLOCKED
（UNKNOWN 不伪 stopped）。**未完成**：真实 `kiarabot-plugin-host`+受管 SDK 插件进程联测；
没有新增 Restart RPC（proto 冻结），Host restart 走 P13b drain、实例 restart 由 stop/start
desired-state 组合；launch_profile/arguments 仍来自部署映射（Runtime 不解析 manifest）。

**合并回归（2026-11-02）**：P13c 合入时重解 runtime_composition 冲突丢失 HostBridge
Persistence 回调，HostBridge 只注册不派发，main 的 `p08_host_bridge_e2e` apply 超时；已恢复四路
回调并保留 P13c HostLifecycleParticipant，e2e 等待增加阶段标签后通过。

## P09/P10 目标 Storage/Scheduler 应用与端口（P10 已实现并本地实测，target 登记待 P10g）

P09a/P09b：Runtime application 提供 storage 应用（scope 解析、成员/禁用 gate、Get/Set/Delete/CAS、TTL、幂等/收据、配额预留）；`NamespaceRepository` 扩 resolve/get/create_shared/set_members/disable/get_entry/put/erase/quota_admit/receipt_insert，`RuntimeUnitOfWork` 在 P06/P07 访问器后追加 `namespaces()`。共享 namespace 写按固定锁序 namespace 行→成员/版本→namespace 配额→installation 聚合配额→key 行；审计复用 P07 共享 `audit_entries`（resource=namespace/key）。**P09a 阶段 2（已实现）**：PG 落 `namespaces/namespace_members/namespace_values/namespace_quota_counters` 四表加 `namespace_receipt_results` typed replay 载荷表；Runtime schema v11（真 PG16.4 实测 descriptor=09c7176f032043f3bc7ba18f8c87b281、relation_count=134）；私有 namespace 以稳定实例 id 惰性建行，值读写/CAS/删除用条件写并按 DB UTC 惰性清理过期、同事务调整 namespace 与 installation 聚合计数；配额准入按固定锁序条件更新；storage/管理写收据复用 `command_receipts`，同键同 fingerprint 重放返回原快照且不续 TTL，容量按实例/安装计数且不淘汰有效行；Runtime owner RPC 适配 unary storage 与共享 namespace 管理面（application 注入才启用，否则 CAPABILITY_NOT_SUPPORTED）。**P09b（已实现）**：PluginBridge 消费 `PluginToCore.host_request` 的 storage Get/Set/Delete/CompareAndSet 帧并回复 `CoreToPlugin.host_response`；私有 namespace 由已认证实例服务端派生，共享 namespace 由注册的 installation 解析（插件不得自报 namespace id），Set/Delete/CAS 的 condition 由帧携带；bridge 以 P06 会话的 ConnectionAuthorization 版本/有效期 + 注册 profile 的 `storage.namespace` 生成在线授权快照，`RuntimeStorageApplication` 仍执行成员/禁用/配额/收据门禁并把失败映射为稳定 `Error`；unary storage 仅内部注入，不对外暴露。
### P09a 阶段 1/2 已实现：domain/ports/application + PG 四表/共享 namespace 管理面

域规则集中在 `core/include/kiarabot/core/domain/target/namespace_rules.hpp`：`NamespaceScope`（私有/安装共享）、`NamespaceValue`/`CompareCondition`/`SharedNamespaceRecord`/`SharedMember`/`NamespaceQuota`/`NamespaceReceipt` 值类型；上限 1024 键/1MiB、单值 64KiB、安装聚合 8192 键/8MiB、TTL 未来且 ≤30 天、共享成员 ≤1024；私有 Set 必须无 condition、私有 Delete 可省略（LWW）、共享 Set/Delete/CAS 及私有 CAS 必须有 condition；条件版本不匹配/并发写冲突 CONFLICT，quota/收据满 RATE_LIMITED。端口新增 `namespace_ports.hpp`（`NamespaceRepository` 条件写/条件删、`SharedNamespaceRepository` 一安装一 namespace/成员整表条件更新、`NamespaceQuotaRepository` 权威 quota 预留、`StorageReceiptRepository` 7 天 typed 收据）；`RuntimeUnitOfWork` 追加 `namespaces()/shared_namespaces()/namespace_quota()/storage_receipts()`，阶段 1 默认 fail-closed（CapabilityNotSupported），适配器未实现时不会伪造空 namespace。

`RuntimeStorageApplication`（`core/src/application/target/runtime_storage_application.cpp`）实现插件 Get/Set/Delete/CAS 与共享管理 Create/Get/SetMembers/Disable：每调用先短读 UoW 解析服务端 scope/成员（插件不接受自报 owner）、关闭事务后做 Identity 在线授权，再开写 UoW 复核成员/条件/版本，在同一事务内写 value、quota、typed 收据与 audit；同键重放返回原结果且不续 TTL，同键异内容 CONFLICT，Unknown commit 不报成功，依赖故障返回 UPSTREAM_UNAVAILABLE 而非空。管理方法使用 `namespace.manage`/`namespace.read` + exact installation scope，Create 幂等且每安装最多一个共享 namespace，成员/禁用更新带条件版本并产生 operation。阶段 1 不写 PG/schema 与 RPC 接线（`internal_dispatcher` 注册与 PG 四表、配额计数、真 PG 集成属阶段 2，待 P07a-A 合入后补）；现有连接/读路径未变。P10a：Scheduler application 实现任务/occurrence 状态机、create/get/list/cancel、到期 claim、receipts/审计、容量计数与 due 索引；`SchedulerUnitOfWork` 增 `tasks()/occurrences()/task_quota()`，occurrence claim 用条件写。P10b：Runtime AcceptTimerTrigger 应用以 inbox(source=scheduler,message=occurrence_id) 去重后持久产生 TimerTriggered，不新增 Runtime 表。Runtime/Scheduler 不新增进程/端口；SDK 的外部 storage/task 路径仍是 PluginBridge host_request。**P10f 状态（2026-11-02，基线 d508d67）**：SchedulerService 六方法、任务/occurrence 状态机、PG schema v9、Runtime AcceptTimerTrigger inbox 去重与 TimerTriggered 持久产生、三进程真 mTLS 定时交接 e2e 已本地实测通过（`ctest -R 'p10'` 5/5、`p03_postgres` 1/1、SDK tasks pytest 43 passed），证据矩阵见 [architecture-p10-scheduler.md](../reports/modules/architecture-p10-scheduler.md)；target override 尚未翻转，真实 7 天 EXPIRED、多 installation 容量、真实平台触发与生产委托来源未验收。租约/事务细节见 doc/22。

## P11 目标 Model 应用与端口（P11a 已实现应用/端口/PG/组合根）

P11a 已实现：domain `model_gateway.hpp`、端口 `model_ports.hpp`/`Model*Repository`、PostgreSQL v7 私有表、`ModelGatewayApplication` 与 `ModelService` RPC 组合根（开发 fake Provider、部署根密钥 AEAD、故障即关闭的调用者校验占位）。契约文本如下；HTTP Provider/Identity 委派/REST 仍由 P11b–P11d 完成，readiness 在 P11a 保持 false。

P11a 在 Model application 内实现 Chat 两次事务、GetCallStatus、GetOperation、Model 配置只读与 Resolve；端口新增 ModelCall/ModelConfig/ProviderSecret 仓储并替换 P03 `model_intents`→`model_calls`（与 P03 owner 协调 RecordKind/codec/schema v8/descriptor（P07 v7 合并后由真 PG16.4 实测））；txn1 只写意图/审计并提交，Provider 调用在 UoW 外，txn2 用新 UoW 写已终态或 UNKNOWN；commit=Unknown 不回滚成“未调用”。P11e 用真实 TLS PluginBridge + 已注册插件 + fake RuntimeModelPort fixture 验证 `HostRequest.model_chat` 的 wire 路径（成功/重复 logical_call_id 不重调 Provider/UNKNOWN→QUERY_SAME_INTENT/429/超时/撤销委托/校验与授权错误），并据此把 ModelChatRequest/Response/ModelMessage 与 host model_chat 帧翻 implemented；streaming 仍不支持。P11b 已提供 reviewed libcurl HTTP Provider 与 root-key AEAD 解封（根密钥部署注入、不入库；4xx/429 已知失败不记 billed，5xx/超时/畸形保持 UNKNOWN；loopback 假上游与两事务集成已测）；P11c 已实现真实 Model 调用者校验（ModelIdentityClient：delegation 结构绑定实际 mTLS peer + 每操作 Identity.CheckAuthorization，Runtime 二跳/Control 一跳严格区分，管理面与 Chat 互不越权；未配置 identity_endpoint 时 fail-closed）与 Runtime→Model typed client（固定 logical_call_id、服务端 request_hash、UNKNOWN=QUERY_SAME_INTENT、不自动重试副作用）及 PluginBridge `HostRequest.model_chat` ingress（校验 model.chat 授权、稳定错误/retryable、逻辑调用 id 透传）；Model readiness 仅当 Identity 通道可用时为 true，组合根仍 AND DB 观察。P11d 管理命令/REST 接线。Model 不新增进程/端口；审计复用 P07 共享 audit 列（Model 留空 target_account/conversation），秘密/Provider 正文/错误文本不进审计/日志/outbox。实现前 readiness 仍为占位，REST 仍 not_implemented。

## 目标Model查询契约补全（未实现）

Model应用负责GetOperation的权威operation/resource绑定与当前授权，以及GetModelConfiguration的精确资源授权与秘密隔离；边界使用model.proto typed请求/结果，内层仍独立值类型。读取不启动Provider、不写新业务操作；缺失/不可见NOT_FOUND，授权依赖不可用UPSTREAM_UNAVAILABLE，2秒默认/5秒上限。metadata与可编辑配置读取分开，配置只返回secret存在性，不返回密钥。P02 stub/参考实现不能作为上述生产者。

### P03 数据库组合输入

数据库材料留在platform的ProcessConfig，包含已安全读取的密码和独立PG CA引用，
不进入domain/application/ports。仅四个PG所有者可配置，Control和Host拒绝；此提交
仅加载材料，连接与writer校验待组合根通过基础设施工厂装配，不因存在配置变为ready。

### P03 配置后组合钩子

ServiceComposition/ServiceComposer属于外层platform，承接已验证ProcessConfig，创建
纯application并持有具体依赖维护回调。领域/应用/端口不引用该配置或框架对象。
原借用application入口继续使用同一实现；组合器先于监听、后于信号屏蔽，资源覆盖RPC
退出且比application更晚销毁。明确依赖失效会有界结束进程；不自动升级schema或改ready。

### Runtime typed读取目标接口（未实现）

Account/Installation/Instance/Adapter/Host各有Get/List，另有GetInstanceConfig；Runtime拥有资源配置/注册目录投影，Host实际进程事实仍归Host。输入exact资源或有限登记目录、PageRequest，输出typed record/SnapshotInfo或完整配置应用投影，默认2秒/最多5秒。当前Identity授权与child独立grant决定结果，不跨库读取身份表；错误/不可见与依赖失败分别NOT_FOUND/UPSTREAM_UNAVAILABLE。目录不继承child授权、未完成配置不算ready；target尚无生产实现。

### 目标时间的跨平台纳秒表示

目标TimePoint明确为time_point<system_clock,nanoseconds>，编译断言其rep为有符号64位，
不再依赖macOS/Linux原生system_clock的微秒/纳秒差异。内部保存精确int64 Unix epoch ns，
范围[-9223372036854775808,9223372036854775807]纳秒；用于接近当前的业务时间，不能表示
线协议允许的所有极端年份。域内转换使用`domain/target/time.hpp`的TimestampParts：
输入nanos只允许0–999999999，先验证完整边界再乘加；无法表示则InvalidArgument，不能
先duration_cast/窄化后再检查。最小边界为(-9223372037秒,145224192纳秒)，最大为
(9223372036秒,854775807纳秒)。输出对负数使用floor秒与非负nanos，-1ns表示为
(-1秒,999999999纳秒)，正负及边界均精确往返，亚微秒不得丢失。

后续wire/存储codec应统一调用该helper，不通过native system_clock::duration默认窄化。
PG原生timestamp的精度不能自动代表完整nano身份；需要精确保留的收据/去重时间应由
实际codec/列显式保存int64 ns或秒+nanos，并另验收DB当前UTC的业务条件判断。
证书校验的唯一to_time_t边界显式转换到native duration；此秒级证书API转换不用于
业务收据/交接时间的持久化或精确比较。reference模型及线协议未改。

已按唯一契约0de1622新增内部ResourceKind::Catalog=15，保留原0–14存储值；wire的
RESOURCE_CATALOG=16必须在边界按名字显式映射，不能数值强转。封闭资源校验接受该kind
及非空ID，但不授予目录内容/子资源权限；有限目录ID与操作授权由对应policy精确核验。
相同ID下Catalog/Account/Model/Task仍是不同资源，不能共享收据scope或混淆交接内容。
基础设施的resource_kind CHECK需要独立扩到15，现有库值不重编号；本提交不修改SQL。

## P04 Identity安全类型与扩展端口（声明，尚未接线）

`domain/target/identity_permissions.hpp`的封闭Permission按唯一契约0de1622逐名映射，
当前39项；Unspecified及未知值不可授权，不以数值cast替代边界映射。G08/G09的后续
目录登记/发现及认证shape待唯一来源增量，不预先造新词条或任意字符串权限。
`identity_types.hpp`定义带版本凭据、登录索引、会话验证材料、精确role/resource及
permission/resource绑定、受限委托/peer/时钟、分离的管理员Bootstrap和秘密成功收据。
access15分钟、refresh30天；公开Session.expires_at=refresh会话期限，access另有期限。
当前凭据rotate/revoke后，旧绑定版本的后续验证、刷新和秘密回放均须拒绝。

秘密结果只用move-only EphemeralTokenPair/EphemeralCredentialDelivery和SecretReplayPayload；
SecretBuffer由成熟crypto adapter提供安全分配/析构擦除，Core仅借用span，不实现密码/
擦除算法或输出格式。EncryptedSecretReceipt只存AEAD密文及绑定，24小时受限恢复，
不能进入P03普通ReplayResult/七天收据或audit/outbox。AEAD绑定包含用途/scope、subject/
session、credential版本、请求keyed commitment、输入/结果轮次及创建/截止时间；改变
行时间不能续命。每次解密前检查当前身份/会话/凭据/轮次，不因存在密文就允许重放。

`ports/target/identity_crypto.hpp`只声明密码、用途隔离HMAC、AEAD、CSPRNG、时钟、真实
证书peer和Ed25519 JWS端口，具体adapter沿锁定jwt-cpp/libsodium；Core不手写算法。
证书链有效不等于身份或业务授权，必须对照当前登记的SAN/服务/凭据；body自报指纹
不是TLS证据。未知key/算法降级/任意jku-x5u拒绝；proof期限56秒+2秒节点差+2秒容差
不超过60秒，actor链/证书绑定/收窄与最大两次业务转交留给后续规则及adapter验证。
密码、签名、密钥调用不持DB锁；SecurityClock不替代P03当前DB UTC。

`identity_repository.hpp`新增IdentitySecurityUnitOfWork继承P03 IdentityUnitOfWork，
新factory显式返回该类型；旧P03类及其纯虚集合未改，禁止dynamic_cast或另开事务补仓储。
credentials/session_security/exact_assignments/secret_receipts/bootstrap与继承的所有repos
必须共用一个物理Identity事务。登录名/凭据索引是类型化记录，不能藏进任意字典；
typed exact assignments是同一授予权威的规范化视图，不是复制授权库。P04授权不得
把P03字符串metadata投影当作绕过封闭词表的路径。普通角色权限与resource在同一绑定
原子项内匹配，默认admin无message.read；有限Catalog没有通配或子资源授权继承。

管理员Bootstrap只由显式可信离线命令一次性消费，初始主体/密码/精确范围/水位/审计
与Consumed同事务；已初始化库缺失marker应Internal，不得隐式重新Required。
服务/插件的WorkloadBootstrap凭据独立于管理员引导；连接proof续签帧尚待契约，当前
接口不实现该连接状态机。以上是类型/端口声明，不创建PG表、不实现RPC或解除假ready门禁。

首接口补充：CredentialState显式包含Pending，PendingCredentialMaterial只保存封闭kind，
不含任何验证秘密。make_pending_credential创建version1 stub，结构validator拒绝Active+
PendingMaterial、Active空密码hash及Pending+已发放材料；不持久化伪Expired终态，过期按
当前UTC/材料期限验证。G08 CredentialStatus的wire映射等唯一提交SHA，不用空hash或Revoked
假装未发放。SessionSecurity增加不可变created_at，脱敏SessionMetadata由该行投影，
expires_at明确为refresh期限；不得用查询now伪造创建时间。list_metadata提供单次一致的
主体有界完整读取，超容量/依赖故障显式失败；供十分钟不可变、每页当前授权及重启失效的
应用快照使用，不做live offset分页，当前还未实现该快照缓存。

秘密幂等scope只含稳定调用者+session/credential+key（Login另有固定规范化用户名），
不含input_rotation/expected_version。二者在SecretRequestParameters中参与keyed request
commitment及完整AEAD binding，因此同scope异版本/轮次必须冲突，不能换scope绕过。
RotateCredential即使没有客户端秘密正文，expected_version也必须进入commitment。
resolve_refresh是受限当前/已消费input verifier解析入口，返回RefreshHandle不是认证成功；
已消费历史仅对原control peer+原key、原input rotation和最长24小时/原token期限内可解析。
record_refresh_consumption与轮换、metadata、成功密文收据同事务，永不存raw token；
新key不能解析旧历史，原key也须当前session轮次等于原成功结果轮次、credential仍当前有效，
才可解密重放。CredentialDelivery先当前授权，再比较原expected_version请求绑定。

## Identity 当前投影与凭据状态（目标未实现）

Identity 的 ValidateSession/GetPrincipal 返回当前主体的逐条 AssignmentRecord、authorization_version 和权威 UTC checked_at。每条绑定独立计算权限与 exact scope，不能把显示 roles/permissions 与全部 scope 做笛卡尔积授权；PrincipalRecord.roles/display_permissions 仅是当前绑定的显示并集。固定角色只用于 human；显式权限使用 role=UNSPECIFIED 加非空、去重的封闭 Permission 列表，两种授予依据不能混用。message.read/action.inspect 等敏感权限不由默认角色或创建资源自动授予。空 scope 不代表全局。

SetAssignment 输入 AssignmentSpec：创建不带 assignment_id/expected_version，Identity 生成 UUID/version=1；更新二者同时提供且版本为正，主体不可更换，并核验旧、新 scope 并集上的当前授予权限。目标尚未发布，原开发快照的 AssignmentRecord 请求已统一替换为 Spec，不提供旧请求桥接。

SessionRecord.created_at 是 Identity 持久保存的首次创建时间，刷新/撤销不改写。会话状态以同响应 checked_at 判定：revoked 优先 REVOKED，其次 checked_at >= expires_at 为 EXPIRED，否则 ACTIVE。Login/Refresh 的剩余 access TTL 为 max(0,floor(access_expires_at-checked_at))；Control 不用本地时间、token iat 或占位时间补齐。缺失权威字段、非当前绑定或授权依赖不可用必须拒绝，不能伪造空权限/成功。

RegisterPrincipal 在同一 Identity 事务创建 Disabled Principal 与无秘密 PENDING WorkloadBootstrap 凭据引用（credential_id/principal_id/binding/version/created_at/checked_at），allowed_services 作为 Pending audiences 持久化；尚未发放的 PENDING 不得伪造 expires_at。首轮成功的 RotateCredential（WorkloadBootstrap 或 human Password）在同一事务把 Disabled principal 激活为 Active 并前进 principal version；RevokeCredential 保留历史版本，撤销后该凭据的 Login/Validate 当前凭据门禁必须失败。GetCredentialStatus 以 identity.principal.read 在线读取同一记录，默认2秒/最多5秒；ACTIVE/EXPIRED 必须有 expires_at，REVOKED 可保留原到期。状态查询不返回秘密、摘要或可重放材料。初次实际发放仍用 RotateCredential 单次 CredentialDelivery，未知结果不得从状态查询恢复秘密。WorkloadBootstrap 轮换交付一次 opaque token（可信策略 24 小时），human Password 轮换交付一次服务端生成的临时密码并由 PasswordCryptoPort 重新派生 verifier；CertificateBinding 轮换属于证书登记流程，本 RPC 明确返回 CAPABILITY_NOT_SUPPORTED。交付加密收据 scope 为稳定调用者+credential+幂等键，expected_version 进入 keyed request commitment，同键异版本 CONFLICT；重放先校验当前授权与当前凭据/轮次。未授权与不存在统一 NOT_FOUND；v1 约束一个精确 binding 只对应一个 Principal（Identity 私有 PG 唯一索引，幂等键不改变该唯一性）。应用/仓储/RPC 已接线，状态为实现中，待完整门禁与真实 PG/TLS 验收后翻转契约状态。

### P04 全局会话容量

既有API上限为4096个未过期认证会话，不能用每主体上限替代。SessionSecurity仓储新增
count_unexpired：同一UoW按实际数据库UTC统计全库未撤销且refresh尚未到期的会话，
失败返回Failure、不假设0。应用仅在创建新会话时按kMaximumUnexpiredSessions检查，
原成功收据重放先于容量判断；读取计数与插入同事务，并保留更小的可信每主体限额。
不会向无授权用户列出其他主体、泄漏会话或创建新的HTTP统计端点。

### P04 撤销会话的完整成功收据

SessionMetadata原样移至共享service_records（同一类型/布局），普通ReplayResult末尾
追加该无秘密快照，旧0–11类型序号不变。RevokeSession成功收据因此完整保存created_at、
主体、轮次、版本、到期和revoked事实，不能仅存旧LoginSession再从可变行补创建时间。
重放仍先当前鉴权/可见性，返回原快照；checked_at是本次权威DB验证时间。Raw token、
verifier、密钥引用和SecretReplayPayload仍不允许进入普通七天收据。基础设施必须
同步typed关系结果编解码/版本与实际持久化测试后，才接入该新variant。

### P04 文本凭据的安全随机端口

RandomPort::opaque_token返回安全内存中的64位小写hex文本，含32字节CSPRNG熵且不带
NUL，供TokenPair、Bearer和bootstrap文本凭据使用；Raw bytes仍供UUID随机位等内部
用途。成熟随机、编码和安全分配由crypto adapter完成，Core不自建明文缓冲/擦除算法，
不能把非UTF8 raw随机字节直接送入Protobuf string/HTTP。随机/分配失败明确返回Failure。

### P04 真实传输证书证据

platform的extract_peer_certificate只从ServerContext::auth_context提取经过TLS认证的
叶证书和链，生成叶在前连续DER及叶SHA256指纹。要求唯一安全transport/叶属性，
最多8张/64KiB，不接受请求body或metadata中的证书。gRPC服务端链可能不包含叶，
复用连接时链可空；统一补真实leaf并仅容许链中出现的一次相同leaf，重复中间证书拒绝。
本函数不构造VerifiedPeer、不授予权限：Identity仍须以真实指纹查询当前登记，验证
subject/service/SAN、凭据状态/版本与期限，再交PeerCertificatePort校验受信链和时间。
尚未接入生产RPC，不能把证据提取测试视为认证用例完成。

### 外部连接授权目标补全（未实现）

platform.proto三类ConnectionAuthorization/RefreshConnectionAuthorization/Result与两Bridge的登记、proof、refresh帧定义短期外部连接授权。名义最多56秒、总预算60秒、每秒检查失效，单飞原子轮换；丢响应须关闭旧流、新bootstrap与连接代次。完整规则见doc/01-contracts及运行语义；内部JWT委托和P02 mTLS owner监听不变，P05/P06真实接线前仍not_implemented。授权材料禁止日志/公开投影/持久秘密收据，高风险业务仍在线核验，刷新不续投递租约、不改历史业务事实。浏览器REST/WS不使用这些proof，OpenAPI/AsyncAPI因此无schema变更。

P06 冻结的 bootstrap 归属（未实现）：登记凭据由 Runtime 逐实例生成并只保存 SHA-256，`RuntimeBindAdapterResponse.bootstrap_credential`（Adapter）与 `RuntimeCreateInstanceResponse.bootstrap_credential`（仅 externally_managed 插件实例）一次性返回给 Control/部署；外部客户端在 `bridge_listen` 的 gRPC metadata 用 `authorization: Bearer <credential>` 换取 `ConnectionAuthorization`。Identity 仍是业务授权权威：ConnectionAuthorization 只表达连接事实，`principal_id` 取 Runtime 直接 peer 资源（adapter_instance_id / instance_id），`credential_version` 取 bootstrap 版本，`authorization_version` 取 Runtime 该 peer 的授权水位；Identity 在线 refresh gate 在 P04/P12 绑定落地后补，P06 先以 Runtime 本地 gate（实例/绑定/config/凭证未撤销）验收。`bridge_credentials` 是 Runtime 私有库的计划内新表，不新增服务/权限。

### G08 Identity模型兼容（唯一契约78b09a6）

Permission追加MessageRead，只有显式敏感绑定可以授予，固定角色/创建资源默认策略不派生。
ExactAssignmentSpec用RoleAssignmentSpec或ExplicitAssignmentSpec的variant表示role XOR
显式permissions；role仅human，explicit集合须非空/去重/封闭，scope须精确非空。更新
不换主体，并核验旧新scope并集；具体授权规则在后续纯policy能力实现。

CredentialVersionRecord新增权威ResourceRef binding及optional expires_at。Pending不得有
expiry；Active无论密码/bootstrap/证书都必须有有限expiry且晚于created_at，bootstrap/
证书不能超过材料期限，密码期限由可信策略提供，不能用空/默认时间假装永不过期。
make_pending_credential显式接收binding，创建version1无expiry/秘密的stub；不会根据
缺失数据臆造资源。credential_status返回无材料的当前投影，Revoked优先，否则Active
在checked_at>=expires_at投影Expired，Pending仍无伪造expires，不持久化Expired终态。
公开created/binding/expiry/checked_at均来自同一权威记录/DB采样，不由Control补值。

委托显式来源：DelegationClaims必须签入source_credential(id+version)及可选source_session；
exchange保留原subject与来源，不得删session伪装workload或替换凭据版本。Identity在线
按该来源核验当前head/active/expiry/session撤销，不依赖“authversion必随撤销增加”的假设；
普通读预算仍不超过60秒。缺少来源的旧proof不能默认为可信，成熟JWT adapter须签验字段。

真实peer登记查询使用IdentityCredentialRepository::by_certificate(leaf SHA-256)的唯一
current-head索引，不读请求body中的credential_id/subject、不遍历全部凭据。返回行仍须
核验主体/service/SAN、当前版本/Active/expiry与资源gate；该查询不等于认证成功。
PeerCertificatePort输入只接受真实TLS证据转换的leaf-first连续DER X509证书串，使用
成熟d2i_X509完整消费；总量<=64KiB、<=8张、非空，无额外前缀或尾随字节。PEM/DER/X509解析
交成熟OpenSSL adapter，不由Core手写；peer提供的根不会因此变成受信CA。

## P04 target transport codec（独立边界工具）

`runtime/platform/target_codec.hpp/.cpp` 仅在外层把内部 target 值类型与生成消息/gRPC 转换；domain/application/ports 不引入生成类型。公开转换返回 Outcome，非法/缺失值和未知/UNSPECIFIED 枚举返回 InvalidArgument，异常转 Internal，不抛出边界。消息 presence 属于包含它的调用边界：必须先检查 has_*，不得把缺失 Timestamp 当 epoch 或缺失 ResourceRef 当默认资源；显式存在的 Unix epoch 合法。

TimePoint 使用已确定的 signed int64 epoch nanoseconds；Timestamp 规范 nanos 为0..999999999，负时间采用向下取整 seconds，精确双向转换。超出 Protobuf 年份或内部 int64 纳秒范围均拒绝，不截断、不饱和。ResourceKind 和封闭 Permission 均按名称逐项显式映射，尤其 Catalog 内部15/wire16，禁止数字cast；未知权限不降级。目标资源 ID 依据doc21第4.1节“标识与版本”要求小写UUIDv4（不是仅凭OpenAPI format:uuid推断），Action 保留128字节[A-Za-z0-9._-]，Task 保留256字节可见ASCII来源规则；空值拒绝。

稳定错误映射严格使用 target_spec.json.error_mapping。make_error 只构造稳定码、合法request_id/trace_id和显式retryable的Error成员，不构造受理事实。target owner unary唯一binary详情为序列化kiarabot.v1.RpcFailure：to_grpc_failure/from_grpc_failure经标准grpc-status-details-bin（Status.error_details）完整携带error和context。前文T01b的Error-only仅是历史/reference Connect格式，target unary拒绝该格式且无自动fallback。from_http_failure验证完整RpcFailure，from_http_error仅验证Error成员。解码要求稳定码和gRPC/HTTP状态一致；缺error/context、未知或UNSPECIFIED acceptance/recovery、显式current_version=0均拒绝。存在operation时须有runtime/scheduler/model登记owner、UUIDv4 operation_id和完整合法资源；Unknown允许无operation，不能为Login/Refresh虚构操作。当前operation查询只由Runtime/Scheduler/Model提供（target_spec完整方法集及doc21第2节），Identity登录/秘密收据不创建operation。这里校验的是结构；owner仍须结合命令和持久事实核验acceptance/recovery组合，不能因枚举合法就让Unknown换新键或按CorrectInput重做副作用。Error.details仍须空，retry_after/current_version/operation/acceptance/recovery完整保留，503/UNAVAILABLE歧义由Error.code消除。

此工具不验证认证、当前授权、资源存在性或owner/gate，不实现owner RPC，也不改变任何能力的 target 状态。CMake/传输接线由主集成负责，独立测试和检查见 reports/modules/architecture-p04-target-codec.md；doc18只读。


### P04 继承 grant 的元数据投影

to_resource_grant_metadata只把完整且可表示的ExactAssignment转成继承ResourceGrant
元数据。id/subject/version保持一致，role分支保留单role并按每个exact资源展开Core的
封闭有效权限名；explicit分支保持原permission/resource元组，只从kPermissionNames
按名转换。没有假permission或空scope占位，不混配不同Assignment。授权仍只读取
exact权威；此投影不能成为另一套授权来源。PG只调用该Core helper同步头/版本/投影。

### P04可执行纯授权/会话/秘密恢复规则

identity_policy.hpp提供40项封闭权限映射、固定角色allowlist及exact resource判断。
viewer限Resource/Account/Plugin/Adapter/Host读取；operator另有Plugin管理、任务、投递重放、
Account策略及Plugin配置读取；admin另有账号、身份授予/主体/会话、模型、namespace和资源
管理。所有角色仍要求同一role/resource元组；MessageRead、ActionInspect、ModelCallInspect、
StorageNamespace、ModelChat、ActionExecute及工作负载能力不由人类默认role派生，需要
各自显式精确授权。插件/服务不继承人类role。Catalog与相同ID的child也不会相等或继承。

授权与公开投影均校验Assignment单basis。role组必须单一role且scope去重；explicit组
每个scope须具有完全相同的permission集合（完整product），不允许将非product的内部
pairs展平成wire permissions[]×scope[]。损坏/混合/重复ID的当前读取返回Internal。
Set的Spec非空、去重、human-only role或explicit XOR；更新不换主体，版本CAS并核验旧新
scope并集。assignment_changed只提出版本+1/dirty=true候选，应用必须与实际修改同事务。

本人ListSessions明确使用本人Principal UUID+IdentitySessionRead；本人Revoke使用目标
Session UUID+IdentitySessionRevoke，由Identity读取其真实owner后授予self-session权利。
这只是两个本人会话规则，不是Principal到Session或Catalog到child的通用继承；跨主体
依然需要exact Principal/Session grant。Control不本地放行或发明catalog id=sessions。

identity_state_rules.hpp验证当前principal、credential head/状态/有限expiry、session
轮次/撤销/独立access-refresh期限，再考虑旧成功receipt。15分钟access精确到期即拒绝，
仍有效的refresh可用；credential换代/过期或session撤销优先于receipt冲突检查。已消费
refresh仅原peer+原key+原input轮次、当前轮次=input+1、原token及24小时恢复窗内可恢复。
秘密row的24小时、method参数、轮次/版本与当前状态重新核验后，才允许调用AEAD open；
此状态规则不代替真实密码/HMAC/AEAD验证。Login/Refresh/Rotate同scope异commitment冲突。

签名来源会在线读取source credential/session，即使authorization_version未变仍拒绝
已撤销会话/旧凭据。委托首业务边hops1/chain[Control]，第二边hops2/[Control,Runtime]；
hops0和第三边拒绝。Identity签发/交换RPC及TLS握手不计业务边；exchange只追加当前转交
actor，aud必须变、权限-资源只收窄、subject/origin不变且期限不延长。普通预算/密码学
签验仍由成熟adapter及调用层执行，纯shape检查不等于JWT认证。

### P04可信离线初始化与Peer权威登记应用

初始化的人类管理员和 Control 平台主体均持久化自身 `Principal` 资源绑定，与各自凭据的
绑定完全一致。外层 PrincipalRecord 必填 binding 直接取已存事实，不能在 RPC/Control 中
临时拼造；这也满足后续在线 peer/credential 所有权核验。

`IdentityAdminApplication`供可信离线composition初始化，以及RPC边界查询真实Peer登记。
`bootstrap_offline`不暴露网络入口，输入为已派生密码verifier与已按可信CA/SAN校验的
Control证书登记，不接收CA私钥。管理员、Control平台主体及真实证书绑定、login、
精确授权、水位、审计和一次性marker在同一Identity Security UoW提交。管理员要求
显式scoped Admin角色；Control只接受explicit grants，不继承human角色。
只Committed返回成功，Unknown返回Unknown/QuerySameIdentity，restart不重开已消费marker。

Peer以真实TLS leaf指纹查当前登记，在线检查主体与凭据。成熟证书校验在事务外，
校验后重查当前登记，拒绝查询期间换代/撤销；可信CA链本身不是已登记业务身份。
查无登记Unauthenticated，查询故障传播Failure，当前存储绑定矛盾Internal。
Platform、Adapter、Plugin分别从已登记service、Adapter资源、Instance资源构造DirectPeer。

### P04短期证明签发应用

JWT NumericDate 只接受整秒：Core 用实际可信/数据库当前时间检查 deadline 和当前状态，
iat/nbf 向下取整；exp 从 `min(iat+56秒, 来源access/凭据/证书到期)` 向下取整，
只能缩短期限。返回 checked_at 保留真实数据库精度，不把测试/生产时钟伪造成整秒。

`IdentityDelegationApplication::issue_delegation`消费六方法应用验证的Control会话，
随后读取当前session、credential、精确授权与授权水位。每个tuple独立授权，水位在
授权查询前后核对；本人ListSessions/RevokeSession沿用前述真实目标规则。
CSPRNG生成新UUID proof ID，注入成熟delegation端口签名；随机源、clock与sign均在
无UoW时调用。证明为第一业务跳、actor链仅Control，期限不超过56秒及源会话/凭据/
Control证书有效期，签入明确source credential/session。签名后再次验证源会话与水位，
源撤销、换代、超时、依赖失败均不返回证明。

此签发应用不持久业务意图，不消费一次性credential，也不将JWT写入普通7天收据。
Issue/Exchange线协议幂等选项由协议拥有者单独确认同步；本实现未修改该选项。
Exchange和Check尚未在此类暴露；父证明的上一跳证书绑定验证，不得使用下一跳mTLS
证书替代或跳过，须先与成熟crypto端口确定验证契约。

## P04 短期委托证明的重试边界

IssueDelegation / ExchangeDelegation 只在线重验权威并签名，不产生业务意图或消费配额；
requires_idempotency=false，重试可返回新的短期证明。不得将 JWT 写入普通命令收据。
完整失败/重试与未来副作用边界见 [运行语义](01-runtime-semantics.md#p04-短期委托证明的重试边界)；
线契约以 `proto/kiarabot/v1/identity.proto` 为准，其他命令的幂等恢复要求未改变。

### P04 六方法 Identity 安全应用

`make_identity_security_application` 注入同一typed安全UoW工厂与成熟crypto ports，实现
Login、RefreshSession、ValidateSession、GetPrincipal、RevokeSession和ListSessions；
不替换P02 factory。readiness仍false，数据库/凭据门禁Unverified，生产组合根另行接线。
读事务释放后才运行密码/HMAC/AEAD/委托验签/随机准备；写事务重新读取当前peer、主体、
credential head、session/rotation和授权版本，使用CAS将security/base元数据、成功收据和
审计等原子提交。只有Committed返回新秘密；Unknown返回原key查询恢复语义，不自动重做。
秘密重放在解密前后重查当前状态；旧凭据/撤销/轮次与期限拒绝优先于旧成功receipt。
刷新历史只允许原peer+原key在原输入有效期及最多24小时内恢复当前成功结果轮次；新key
不能定位旧成功。令牌通过opaque_token获得可见ASCII，不以raw随机字节填wire string。

登录使用现有ASCII1..128用户名小写规范化；不存在用户与错误密码均执行密码verify，
前者使用可信dummy verifier。验证当前Control peer后，按注入clock的monotonic执行本机
固定窗口：每peer每分钟60次，每normalized username跨peer合计每分钟30次，两类计数键总数
最多4096，无活跃key驱逐；满额或超频RateLimited。计入失败尝试，过期窗口释放容量，
重启清空。它是本机CPU保护，不宣称跨实例/持久化分布式限频；部署边界限频另行负责。

每次access查询读取当前权威会话、主体、凭据及逐条exact assignments；每页list先验当前
委托签名来源session/credential、授权和版本。结果存为最多10分钟的不可变内存快照，
绑定actor/origin、target、page size、authversion和随机opaque cursor；总记录与快照数
受可信options限制，无有效快照驱逐、live offset或故障假空页。重启使旧cursor失效。
Revoke普通七天收据保存完整不可变SessionMetadata，使用固定可信ReceiptCapacity10000，
空请求指纹为成熟预计算域摘要，目标与actor已在ReceiptKey；新JWT/request_id/deadline
不改变请求内容。重放返回原快照，checked_at仍取本次权威数据库时间。

Login新建会话的最终短UoW在原收据重放分支之后调用count_unexpired，按同事务数据库当前
UTC统计未撤销且refresh尚未到期会话，4096全局上限之外另保留可信per-subject更小限额。
计数失败传播Failure，不当0；满额不阻止已成功原结果重放，不扫描其他主体拼造计数。
高风险操作提交前重新采样DB时间，超过一秒授权窗口或deadline/proof/credential/session
期限则回滚；拒绝审计只归因真实当前登记的direct peer，不用未验证represented actor，
不记录密码/用户名/token，Unknown不另当拒绝重试或补写审计。

ListSessions续页只需公开的opaque page_token，服务端有界索引将其定位到快照及页位置；
调用者无需另传snapshot_id。若内部调用同时提供snapshot_id，必须与token定位值相同。
当前actor/origin、目标scope、page size和授权版本绑定不变，过期时清理快照及token索引。

IdentitySessionSecurityRepository::save负责同一UoW内security row与base sessions metadata的
原子同步，version/rotation/revoked及refresh expiry一致。应用调用一次安全save，不再额外
调用sessions().save重复create/CAS；登录、刷新和撤销均遵循此责任。读取仍校验两投影一致。

Login秘密收据重放还绑定最初密码验证所对应的当前LoginIdentity（normalized_login、subject、
password credential及index version），解密前后每次重放gate均重读。用户名重新分配到另一
主体/凭据或索引版本变化时拒绝旧结果，不能仅因旧session仍有效返回原主体token。

刷新最终短UoW会重新按精确输入verifier及原peer/key解析refresh handle并读取成功收据，
不能沿用前一个已释放读取中的Current handle。若同键竞争者已成功提交，则释放事务后
按原结果受限重放；不同键仍不能查询旧输入历史，不把正常同键恢复误报为认证失败。

分页token不存在、篡改、过期、重启失效或快照owner/origin/scope/page-size/version绑定
不匹配，按唯一API pagination.token_error返回InvalidArgument（HTTP400），不是Conflict。
前置当前身份/授权拒绝仍保留其Unauthenticated/PermissionDenied语义，不由token错误掩盖。

有效当前actor请求撤销不存在的精确session也属于写拒绝：返回NotFound并在短UoW记录
拒绝审计，使用真实登记direct peer及其授权水位；审计依赖失败显式传播，不假成功。
这一规则不将普通读取的缺失无差别计为写审计，也不重写Unknown提交语义。

## P04 Identity RPC 边界（由 core/app/identity_composition.cpp 装配，P04-A 已接主体/凭据能力）

2026-11-02 更新：`core/app/identity_main.cpp` 通过 `identity_composition.cpp` 的 `compose_identity_service` 装配 IdentityService；组合根注入 PostgreSQL security factory、成熟 crypto ports、受信 `SystemSecurityClock`、基于 Core Admin 的 PeerAuthenticator 与 `IdentityPrincipalApplication`，并在 RPC shutdown/Wait 之后才释放应用、crypto/factory 与时钟。P04-A 在既有会话/委托边界上追加 RegisterPrincipal/RotateCredential/RevokeCredential/GetPrincipalStatus/GetCredentialStatus，并把 Identity readiness 从固定 false 改为真实门禁（业务实现 + DB writer + 密钥材料装载 + bootstrap Consumed + 可信 clock）；真实 PostgreSQL/HTTPS/TLS 生产链与部署验收仍按独立门禁记录。详见 `reports/modules/architecture-p04-identity-composition.md` 与 `reports/modules/architecture-p04-credential-management.md`。

`runtime/platform/identity_rpc.hpp/.cpp` 的 make_identity_rpc 借用 IdentitySecurityApplication 并持有组合根注入 PeerAuthenticator；适配 Login/RefreshSession/ValidateSession/GetPrincipal/RevokeSession/ListSessions，并通过可选 `IdentityRpcOptions{.principal=&app}` 追加 RegisterPrincipal/RotateCredential/RevokeCredential/GetPrincipalStatus/GetCredentialStatus 五个主体/凭据管理方法（RPC 层只做转换与稳定错误映射，业务在 `IdentityPrincipalApplication`）。未注入 principal 能力时五个方法返回 typed CAPABILITY_NOT_SUPPORTED；其余 Identity 方法仍按未实现返回稳定拒绝，不挂接 reference 实现。本工具存在不代表目标业务已通过完整门禁或已完成部署验收。

每次从真实ServerContext.auth_context提取受限叶证据，再调用必须核对当前登记/凭据/SAN的可信认证回调，并核对回调的主体、证书指纹、平台角色和凭据版本。六方法要求Control peer；不相信body actor/peer、不把CA有效当业务授权。纯应用仍须复核当前Identity状态、self Principal列表和exact Session撤销权限。认证回调为同步借用契约，生产者须有自身有界I/O并遵守实际取消/deadline；边界在其前后检查预算，不声称能抢占任意阻塞回调。

请求context需小写UUIDv4 request_id/trace_id（doc21§4.1），畸形关联生成新CSPRNG错误ID；幂等键至多128字节封闭字符且写方法非空。取body期限、真实gRPC期限与5秒上限最早者；未给传输期限时默认2秒。该边界预算比通用owner descriptor上限更紧，不允许正文延长。密码/token/委托正文只借用SecretView到同步应用返回，不记录秘密/异常正文。回调/应用异常分别保留未调用或Unknown语义，CSPRNG或分配故障无法形成安全关联时仅返回脱敏传输INTERNAL，不伪造ID或业务结果。

成功必须是应用已提交结果，局部完整投影后才返回；checked_at/created_at/expiry/version来自应用，不补本机时间或token iat。缺持久binding、时间、版本或不合法token结果返回Internal，不造占位。Assignment投影调用Core project_assignment，固定角色显示权限调用default_role_allows；不跨assignment拼scope，角色行必须同role无explicit，explicit行各资源权限集合必须相同，无法无损表达则Internal。角色与权限仅显示并集，真正授权仍依赖逐条exact绑定。List输入拒绝expires_at/next_page_token等输出字段，续页可仅传opaque cursor，若另传snapshot_id必须匹配，当前权限/快照恢复由应用裁定。

Core Failure无损映射RpcFailure的acceptance/recovery/operation/retry_after，未持有的current_version不伪造。ReadCurrent→RESYNC_SNAPSHOT、Backoff→RETRY_SAME_ID、QuerySameIdentity→QUERY_SAME_INTENT、Diagnose→MANUAL_REVIEW；Unknown保留Unknown，不得携带换新身份/修正输入重做副作用建议。畸形应用失败或投影保守返回Internal/Unknown及原身份查询，决不假称NotAccepted。标准grpc-status-details-bin仅RpcFailure，无Error-only回退。

六方法RPC恢复补正：同键Login/Refresh恢复已提交的原TokenPair时，access已过期但refresh仍有效可原样返回（checked_at可能已超过access期限，Control剩余TTL clamp至0），不重新签发或延长任何期限。ListSessions接受仅opaque page_token的续页，不要求浏览器另传snapshot_id；输出字段仍禁止作为输入。

### P04 当前登记 PeerAuthenticator（Core持有权威编排）

make_identity_peer_authenticator(IdentityAdminApplication&, SecurityClockPort&)只借用Core Admin应用和受信时钟。wrapper从真实AuthContext提取指纹/DER证据，取2秒本地与真实gRPC deadline较早者，调用Admin.authenticate_peer({leaf_fingerprint, deadline}, der_chain)。当前DB UTC、登记/凭据/主体、释放UoW后成熟crypto验证及验证后fresh recheck全部由Core负责；外层不直接读取repos、不复制业务规则。

该回调仅认证当前平台peer，不授予业务权限；必须与纯应用使用同一Identity权威工厂。2秒本地预算和真实gRPC取消/deadline在各阶段检查，I/O自身有界由受审端口实现保证，不声称抢占任意阻塞端口。库中不构造PG连接、信任根、bootstrap或ready；生产组合根（`core/app/identity_composition.cpp`，2026-11-02）注入已审完整typed工厂、成熟crypto及受信时钟，真实 PostgreSQL/HTTPS 生产链验收仍属后续任务。无wire/API字段变化。
wrapper仅核对返回指纹、正verified_at、受信clock/已知偏差及与verified_at的2秒窗口，再检查取消/期限。不构造数据库/信任根/bootstrap或ready。借用应用与clock须活到服务停止/Wait，Core应用与安全应用由组合根绑定同一完整Identity工厂。wire/API不变。

### P04默认角色目录跨契约一致性

默认角色能力唯一来源为OpenAPI的`x-control-policy.target_role_capabilities`，Core
`default_role_allows`逐项精确对应：model.read三角色可用，identity.session.read/revoke
仅admin/operator；account.policy.manage仍仅admin/operator。能力仍需与同一assignment
的精确资源匹配，不能跨scope或把角色当全资源通行证。本人会话的专用规则不因此改变。
action.inspect、message.read不属于任何默认角色，必须显式授予；G08基础读取
account.read/plugin.read/adapter.read/host.read保留三角色，plugin.config.read保留admin/operator。
`identity_rules_test --role-catalog`输出实际函数结果，Ruby回归直接读取OpenAPI并逐角色
比较精确集合，不维护第二份预期权限表；未知枚举/名称继续拒绝。

### P04 Identity trusted composition inputs

ProcessConfig may carry Identity-only identity_security file references and an explicit
trusted-clock declaration. It is an outer platform input, never a domain/application
or repository dependency. It requires database/TLS; strict validation and limits are
specified in doc/08. No cryptographic key bytes are loaded into this JSON structure.
The Identity entrypoint now builds the real security application, Admin and issuer
with the same complete PostgreSQL security factory, mature crypto and explicit
SystemSecurityClock. One IdentityService borrows all three applications; actual
transport evidence is authenticated through Admin, never through CA validity alone.
Keys, clock and a CSPRNG-derived dummy password PHC are validated before opening PG.
RPC shutdown/Wait precedes applications, crypto/factory and clock destruction.
An absent declaration keeps the existing not-ready placeholder. Overall Identity
readiness remains false while other Identity methods are incomplete.

### P04 IssueDelegation RPC（第七方法，可选依赖）

make_identity_rpc(IdentitySecurityApplication&, PeerAuthenticator, IdentityDelegationApplication* issuer=nullptr, InvocationDeadline={})仍注册一个IdentityService，借用issuer到停止/Wait；两参旧调用可编译，缺issuer时Issue返回RpcFailure/CAPABILITY_NOT_SUPPORTED。Exchange与其他未接方法仍明确未实现，不降级到reference。Peer factory当前签名为make_identity_peer_authenticator(IdentityAdminApplication&, SecurityClockPort&)。

Issue只接受当前Control peer，subject_proof按原始AccessSession token借用，不剥Bearer或接收现成委托替代。audience显式映射封闭六服务；scope/permissions非空、去重、已知枚举与合法exact资源，client_certificate_sha256必须恰好32字节并等于真实transport peer。wire声明的permission×scope product只转换一次为exact tuples，由Core issuer逐tuple复核当前来源与权限，外层不授予或拼接其他assignment。

按父定稿1b962d3，Issue/Exchange requires_idempotency=false：签发不消费配额或一次性材料、无业务意图/普通七天秘密收据，重复请求重新授权并可产生新proof ID。Login/Refresh/Revoke/Rotate仍要求原幂等键。返回只复制应用实际签出的proof及原audience/expiry/一跳信息，核对claim与请求tuple、真实证书及来源上下文一致，不补签名/期限/来源；crypto和权威查询均由Core持有。真实JWT生产签名/PG/HTTPS组合根闭环由父继续验收，本RPC fixture不据此标target implemented。

Issue 接受的精确 permission/resource tuples 最多128条，与成熟JWT适配器既有容量一致；
Core 在任何权威读取/签名前拒绝第129条。RPC在展开permission×resource product前也应
核对同一上限，不能先构造大集合再依赖签名器拒绝。既有精确资源授权要求不变。

### P04 Exchange 父证明密码学端口

`DelegationCryptoPort::verify_exchange_parent`接收真实已认证Runtime recipient、父JWT和
可信时钟。父audience必须是该recipient服务，父仍为第一业务跳；验证成熟签名/issuer/
key/封闭claims/时间，与普通verify使用相同严格规则。父client_certificate绑定的是上一跳
Control，不能拿接收Runtime的叶证书替代比较，也不能把Runtime冒充Control证明私钥持有。
此入口只证明签名和受众：Core还必须核验当前recipient与上一跳Control登记、原始source
credential/session及授权版本、精确收窄，再签第二跳；不绕过在线授权，不授予额外权限。
本接口须与成熟crypto实现一起集成，未实现时不能构造假的VerifiedDelegation或开启Exchange。

Issue RPC在解码/展开前同步成熟JWT的128 exact bindings上限：资源1..128、权限1..40且封闭去重，permission_count <=128/resource_count，以除法规避溢出。超限沿既有INVALID_ARGUMENT返回，不调用Core issuer的来源验证/签名；不先构建大型product再等crypto晚拒。Core自身的同一上限由Core能力维护，传输层不改其实现。

### P04 公开验签材料端口

独立PublicVerificationKey仅含key引用、raw32 Ed25519公钥、正版本及有限valid_until；
VerificationKeySet含非空有界keys、正集合版本和离线Identity部署UUID Catalog锚点。
VerificationKeyMaterialPort::snapshot只读不可变可信材料，不授权、不联网、不读DB。
唯一active key到期、空集合或时间不可信返回UpstreamUnavailable，不伪造空成功。
后续GetVerificationKeys由Core验证真实当前登记platform mTLS及显式identity.keys.read@
该Catalog，不以前置JWT造成未知kid循环。known_version相等/较旧/缺省返回完整当前集合，
future版本InvalidArgument/CorrectInput。此类型/端口提交不启用RPC或伪ready。

### Identity own TLS lifetime gate

The outer configuration retains the intersection of not-before/not-after for the
actual OpenSSL-selected verified server chain, including its trust anchor. No inner
Core dependency on OpenSSL is introduced. Configured Identity checks this finite
interval through the same trusted security clock borrowed by its crypto, session
application and real peer authenticator. Every operation gates before peer lookup,
again after authentication and at application security checks; an established TLS
channel does not bypass current credential validity. Invalid own credentials latch
unavailable; readiness Credentials is unavailable and the maintenance check causes
RPC shutdown/Wait and exit1. Clock faults keep their separately documented behavior.

### Identity 自身 TLS 调用期限

当前factory为make_identity_rpc(SecurityApp&, PeerAuthenticator, Issuer*=nullptr, InvocationDeadline={})，其中InvocationDeadline是function<Outcome<TimePoint>()>，闭包由服务持有。生产composition必须注入实际选定自身TLS链有效期与当前可信clock/lifecycle gate；空回调仅为fixture/源兼容，不得作为生产默认安全策略，也不得把自身到期时间写入远端Control的CertificateBinding。

run保留body/gRPC/default2秒/max5秒及取消检查，可信回调失败沿原Failure进入完整RpcFailure，异常在调用应用前转Internal/NotAccepted。返回非正时间失败关闭为UpstreamUnavailable；过期期限为Timeout/NotAccepted。有效期限取已有预算与自身limit最早者，回调之后与认证之后再次检查取消/到期；传入每个Core用例的context.deadline包含此上限，由Core final_check阻止自身TLS到期后的提交。外层不授权、不查仓储、不实现TLS链/clock判断或伪造有效期。较晚limit不能延长请求期限。

### P04 GetVerificationKeys 只读应用

IdentityVerificationKeysApplication以借用的同IdentitySecurityUoWFactory和材料端口构造，
无JWT前置。真实当前登记Platform peer经过credential head、主体、证书/SAN/service及
exact IdentityKeysRead@材料离线Catalog在线授权后，返回完整公开keyset及本次DB checked_at。
普通role、其他Catalog或子资源均不派生该权限。known_version=0为InvalidArgument；future
版本只在当前鉴权后拒绝，same/old/缺省仍返回完整原set，不伪造not-modified空集合。

材料snapshot在所有UoW之外调用；前后当前登记/主体/授权版本复核，最终只读短UoW用DB
UTC验证key/credential/cert期限与一秒授权窗口。材料必须可信单key、正集合/key版本、有限
有效期及显式UUID Catalog；损坏形状Internal，到期或材料/依赖不可确认UpstreamUnavailable，
不以空集合代表失败。应用不写库、不commit、不缓存、不联网、不签发；RPC/组合根另行接线。

### Identity verification material composition

The production bundle requires explicit offline Catalog UUID, positive set/key
versions and finite key expiry in identity_security.verification (doc/08 exact
schema). Secure file loading preserves key metadata; make_delegation_crypto_bundle
builds both ports from the same configuration, and composition owns them until after
all application/RPC borrowers stop. A current public-material snapshot is validated
before opening PG. No public material endpoint or authorization bypass is introduced;
the retained port awaits the independently reviewed Keys application/RPC integration.

### Production Identity invocation limit injection

The configured production factory now always supplies InvocationDeadline. Its owned
clock first validates the current trusted sample and own-chain lifecycle, then returns
exactly the retained selected-chain not_after. Missing own metadata, untrusted clock
or expired credentials fail closed. RPC takes the minimum with request/transport
budgets and passes that deadline into Core final_check, including work that starts
before own TLS expiry and finishes later. The remote Control certificate metadata
is never replaced. Empty callback compatibility remains confined to fixtures.

### P04-B 失效流与授权快照（本分支已实现，待与 P04-A 集成后统一验收）

Identity 持久失效日志（epoch/sequence 单行状态 + 失效记录 + 精确作用域）与 SetAssignment/RevokeAssignment（以及凭据撤销等授权变更）同事务写入；`WatchInvalidations` 以 ServerWriter 从持久游标续发并按上述连续性规则返回 `resync_required`，`GetAuthorizationSnapshot` 返回带 epoch/sequence 绑定的当前权威快照。订阅授权使用每资源显式 `identity.invalidation.read`，每次调用/续发重新校验当前 peer 与当前授权；缺失存储或依赖故障 fail closed，不返回空流/空快照。方法语义、保留窗口与 resync 规则以 [`doc/21-service-contract-design.md`](21-service-contract-design.md) 的“Identity 失效流与授权快照（P04-B 最小冻结）”为准；契约状态以 `doc/01-contracts.md` 与 `proto/tests/contract_status.json` 为准（验证通过后才翻转）。

### P04 授予撤销的当前状态模型

`AssignmentStateRecord` 将有效 ExactAssignment 与 Active/Revoked 当前状态分开。
撤销保留原 ID、主体和精确 scope，将版本条件推进一次并记录权威 UTC revoked_at；
空权限列表不表示撤销。Revoked 的 ID 永不重新创建、换主体或通过 Set 恢复。
有效授权查询只读取 Active 的 exact tuples；保留的墓碑仅供管理核验和原收据恢复，
不能作为权限依据。同幂等键重放由应用在当前鉴权后返回原成功，新键重复撤销为 Conflict。
纯规则沿现有版本错误语义：错预期版本 Conflict，uint64 耗尽 Internal，不回绕。
本节只冻结领域模型；仓储按 ID 读取、撤销 CAS、base metadata 过滤及生产接线仍须独立验收。

### IssueDelegation platform capability and represented permissions

An authenticated Control service must currently hold an explicit
identity.delegation.issue binding at its own stable Principal resource
(ResourceKind::Principal, id=the actual registered platform subject). Certificate
validity/registration and default roles do not grant this capability; a grant at a
represented human Principal, Catalog or other service Principal cannot substitute.
This service capability is checked independently of every represented subject's
exact requested business permission/resource. It therefore supports dynamic Session
resources without turning the service capability into blanket business authority.
Offline enrollment must explicitly provide the own-Principal grant; no CLI or daemon
automatically creates it.

Issue reads the current Control registration, credential, Principal, authorization
watermark and exact grants before signing and again after signing. Source access
and business grants are also revalidated. Each short authority transaction uses the
existing current-time/final-check gate with at most one second authorization window
and the request/certificate/credential deadlines. Crypto/random/session-application
calls hold no outer UoW. Missing/wrong capability returns PermissionDenied; revoked,
expired or mismatched current peer returns Unauthenticated; changed authorization
watermark returns Conflict/ReadCurrent; dependency and deadline failures propagate.
A signed intermediate proof is discarded on any final check failure and never returned.

### P04 ExchangeDelegation 独立只读应用

Exchange只接受真实当前Runtime平台mTLS，父proof由成熟verify_exchange_parent验证，
受众Runtime且为首业务跳。应用按父签名Control叶指纹查询当前登记/current credential head，
核对其Control service、主体、SAN、Active/期限/gate；不拿Runtime叶替代上一跳证据。
Runtime和上一跳Control都须在自己的稳定Principal资源（id=实际platform subject）上
具有当前显式IdentityDelegationIssue；原来源主体仍须逐条具备请求业务permission的exact
业务资源授权。服务能力与来源业务授权互不替代，无默认role、
通配或虚构Catalog放行。父旧签名不替代当前登记/凭据/session/authversion与赋权查询。

子受众仅Model/Scheduler，请求tuples必须非空封闭去重且为父集合子集；输出hop2链追加
Runtime，subject及source credential/session保持不变，client cert改为实际Runtime叶。
父签验、clock/random及签名都在所有UoW之外；签前后重查两平台与来源当前authority，
最终短事务一秒授权窗口和DB当前期限再检。NumericDate取整只缩短、不延长父exp、两平台
证书/凭据、source期限或本次deadline；纯签发不收据、不消费command intent、不写库/commit。

### P04源会话Control持久绑定

`SessionSecurity.control_peer`保存`SessionControlPeer {subject, credential}`。新会话必须
由实际已认证Control写入有效绑定；refresh/revoke保留不变，客户端不能自报或替换。
可空仅用于表达缺失/损坏供拒绝路径处理，不允许生产创建缺值会话，也不代表任意peer。
下游委托即使由Runtime转交，仍按该credential ID读取原Control当前head、证书有效期和
主体gate，并与签名actor链首项核对。稳定ID允许证书轮换后原会话继续使用，但只接受
该服务当前有效身份；不重用旧leaf认证新连接。原Control撤销/失效/禁用或缺绑定拒绝。
本批只冻结类型/端口契约，Login写入与PG schema/repo接入由父协调，未接入不得启用
依赖此绑定的Revoke/Exchange。只读类型测试不等于真实存储或并发验收。

Login 的新 SessionSecurity 现在从最终核验的实际 Control peer 写入上述稳定主体与
credential ID；客户端请求不提供此绑定。登录原收据重放及 refresh 保持原绑定。
对应 PG 保存与不可变约束仍由独立 schema 能力实现，缺失字段不得现场补默认值。

Exchange签后最终DB checked_at不得早于签前authority.checked_at；即使版本及期限仍表面
有效，跨UoW数据库UTC回退也返回UpstreamUnavailable并丢弃已签proof，不靠NumericDate
取整掩盖。每个单独UoW内的时间检查之外，还保持这一跨阶段单调证据约束。

### GetVerificationKeys RPC（借用Core公共验证材料用例）

factory保留原issuer与InvocationDeadline位置，末尾新增IdentityRpcOptions={.verification_keys=&app}第五参；app借用至server停止/Wait，缺省typed Unsupported。当前mTLS认证及自身TLS InvocationDeadline在查询前照常执行。GetKeys不读/验证context.delegation或JWT，不对unknown kid先做JWT预检，避免获取验证材料的自举循环；Core151f31e负责当前平台身份、显式IdentityKeysRead@可信Catalog与授权后future-version拒绝。

known_key_version显式0无效；正值原样交Core，none/same/old均返回完整current set，不做空delta或成功cache。只映射Core的public-only DTO：key_id、raw32 Ed25519公钥、正uint64键/集合版本与精确纳秒有限valid_until；空集合/零版本/不合法Catalog/零checked_at/过期或无限期限等畸形成功结果Internal。实际依赖/材料过期失败保持完整RpcFailure。外层不访问private/config路径，不加载材料、不重建keyring、不使用token选择远程key源。当前Core契约仅一个active key，生产PG及成熟材料端口闭环由组合根验收。

### P04 在线授权专用验签入口

`DelegationCryptoPort::verify_for_authorization`返回独立的
`AuthorityVerifiedDelegation{claims}`，只证明成熟JWT签名、issuer、有限key、封闭claims、
时间与实际已认证平台recipient的audience一致，支持一至两跳。它不比较recipient叶与
JWT中的上游actor指纹，不证明原业务连接的TLS possession。业务owner仍须用普通
`verify`绑定真实来客；Identity应用另读当前recipient、所有actor及来源凭据/session/
授权状态。此入口不授予业务权限、不访问数据库；坏proof/peer返回Unauthenticated，
不可信时钟或资源故障返回UpstreamUnavailable，不生成新稳定错误码或wire字段。

### Production GetVerificationKeys composition

Configured Identity owns IdentityVerificationKeysApplication alongside its session,
Admin and issuer applications. It borrows the same full security UoW factory and
public-material port returned with mature delegation crypto from the single trusted
bundle. RPC borrows this application until shutdown/Wait; the application is destroyed
before its material/factory/clock dependencies. No independent keyset configuration
or fixture Service replaces the production path.

Key bootstrap requires actual currently registered platform mTLS and explicit
IdentityKeysRead at the trusted offline Catalog anchor. JWT is not required or used
as the trust bootstrap. Known version absent/equal/older yields the full current set;
a future version is rejected after current authorization. Overall Identity readiness
is still false while remaining Identity operations are incomplete.

### P04 当前平台 actor 定位端口

`IdentityCredentialRepository::by_platform(PlatformPeer)`在同一UoW按受信signed actor的
service+subject定位CURRENT平台证书head；零条为空，多条返回Internal，不任选、不全主体
内存扫描。这只是权威定位，不是TLS验证；Core另核验current head、owner、主体状态、
证书SAN/service及有限期限。供无session来源的早期actor在线检查使用；PG实现另行集成，
缺实现不得启用依赖它的用例。现有未使用fixture直接失败，不提供伪成功fallback。

### P04 CheckAuthorization 纯应用（未接RPC）

`IdentityAuthorizationApplication`只允许真实当前Runtime/Host/Scheduler/Model recipient，
必须显式授予IdentityAuthorizationCheck@自身Principal，不能由普通role或业务scope替代。
受信AuthorizationMethodPolicyPort从外层唯一契约metadata解析canonical full_method到
封闭owner/permission/可用scope kinds；请求不能自报权限。method owner、真实recipient
服务、签名audience必须一致，Control管理员不能跨consumer询问；Identity本地管理直接
读取本地权威，不远程自查。scope为1..128个合法去重exact资源，逐资源同时检查proof绑定
和source当前精确授权，不把显示权限与范围做笛卡尔积。

应用先只读核验recipient与能力，释放事务再用专用crypto入口验签；随后两个独立短只读
事务读取source当前principal/credential/session、authorization/exact以及所有actor当前
登记，最后复核水位和DB UTC。首actor为Control，第二actor为Runtime；direct_actor/cnf
经by_certificate/current核验，Human来源原Control按SessionControlPeer稳定凭据ID读取；
无session的两跳来源经by_platform定位原Control。不存在伪VerifiedPeer或原业务TLS证据。
业务owner仍须普通verify真实来客，并遵守高风险检查到业务提交/dispatch最多一秒。

unknown method/scope、known version=0/future为InvalidArgument；缺consumer自身cap为
Failure PermissionDenied。坏proof/audience/source/session/actor当前状态或陈旧proof授权
版本为Failure Unauthenticated；业务proof或source当前缺grant为成功decision allowed=false，
denial为稳定PermissionDenied。DB/clock故障与Timeout保留Failure，不能转业务false。
known旧版本不授权旧状态；返回当前source授权版本和本次DB checked_at。前后权威水位
变化拒绝，DB UTC回退UpstreamUnavailable，总应用窗口及最终权威读取窗口均不超过一秒。
应用不写库、不commit、不缓存，不新增wire字段或错误枚举；外层resolver/RPC/PG接线另验收。

CheckAuthorization最终无UoW时钟采样后，以max(可信最终UTC, 最终DB checked_at)再次检查
proof/source/session/全部peer的最早有效期及请求deadline；即使DB读取仍有效、版本不变且
单调耗时小于一秒，最后采样跨期限也返回Timeout，不返回allowed或普通业务false。

独立IdentityAssignmentFingerprintPort为Set(spec,optional target,optional expected)及
Revoke(target,expected)提供服务端成熟指纹，固定SetAssignment.v1/RevokeAssignment.v1域，
不借用登录秘密commit scope，也不信客户端hash。规范编码包含方法、target、owner、basis、
role或permissions、exact resources、expected及可选字段存在标志；权限/资源排序等价，
重复或无效输入拒绝。request/trace/deadline/idempotency key不属于内容，port不授权、不查DB、
不收raw token，所有调用在UoW外。此纯端口声明不代表成熟哈希或Set/Revoke应用已启用。

### Current original Control of browser sessions

Every browser session retains its original Control subject and credential ID. Access,
refresh and secret replay require the actual current Control to match both stable IDs;
certificate rotation under that same ID is allowed after current TLS/registration
verification. Another Control cannot take over or rebind old tokens. Missing binding
fails closed. Current head, active PlatformService/Control, own Principal binding,
registered SAN, credential/certificate validity and dependency failures are checked
again at session authority reads; no VerifiedPeer is fabricated from stored records.

For delegated source_session, the persisted original Control subject must match
claims.actor_chain.front(). That original Control is checked from typed current
registration independently of the actual direct Runtime's own authenticated TLS.
Relevant final deadlines include the original Control current credential/certificate
expiry and the existing one-second authorization checkpoint. Crypto runs outside UoW;
fresh reads after verification/replay reject intervening invalidation or dependency
failure. Source binding does not pin a certificate version or overwrite direct peer data.

CheckAuthorization还要求每个签名actor（首Control、第二Runtime）当前具有显式
IdentityDelegationIssue@自身Principal，role-only、其他Principal或撤去该能力都使actor
当前无效并返回Unauthenticated。来源业务授权版本不能代替actor能力水位；签名后或最终
重读时撤权同样拒绝。consumer自身Check能力仍独立PermissionDenied，source业务grant
不足仍正常allowed=false；source/session/actor验证不因业务已拒绝而跳过。


### P04 ExchangeDelegation RPC 适配

`IdentityRpcOptions{.exchange=&app}` 可选借用 Core `IdentityExchangeApplication`，生命周期至
server 停止/Wait；原 issuer、InvocationDeadline 及 verification_keys 参数位置不变。
未注入时返回 typed CapabilityNotSupported。只接受当前认证的 Runtime 平台 mTLS，
子受众仅 model/scheduler；body 的32字节 client_certificate_sha256 必须等于实际 Runtime
叶指纹。parent_proof 原样借用，不剥离 Bearer，不在外层解析 JWT，不把 Runtime 指纹
当成父 proof 的上一跳 Control 指纹。父签验、两平台当前显式能力、来源会话/凭据/业务
权限及签后重查均由 Core 负责。

封闭 permission/resource 去重并在解码、笛卡尔积展开前限制最多128个 exact tuples；
父 proof 非空、可见 ASCII 且最多32768字节。无普通写入幂等键要求、无收据或库写入。
继承当前 mTLS、请求/transport/自身 TLS InvocationDeadline 的最小期限与完整 RpcFailure
映射。成功只投影 Core 返回的原始 proof、audience、expiry、hop2，校验当前 Runtime
指纹、actor 链和请求精确集合；异常成功结果失败关闭，不发送部分 proof。

真实 mTLS fixture 覆盖该适配与实际 Core 编排；测试的仓储、随机和签验仍为明确替身，
不代表生产 PostgreSQL、JWT 材料端口或组合根已接线可用。

CheckAuthorization的Human来源原Control gate复用已审
`identity_security_detail::read_session_control`与`session_control_valid_until`；签名链首项
仍匹配持久subject，显式Issue能力及其watermark仍由Check独立复核。无session的原actor
走by_platform，direct actor/cnf继续独立检查；共享读取不构造或替代实际TLS peer。

### P04 Assignment 墓碑仓储契约

IdentityExactAssignmentRepository::current(id)按ID返回Active或Revoked完整记录，供管理
旧scope核验；for_subject和base grants metadata仅投影Active。save_state同UoW原子同步
该唯一授权事实与base metadata：只允许Active v1创建、同owner Active CAS v+1更新及
Active→Revoked v+1；Revoked永久保留owner/旧exact scope/终版本/有限撤销时间，不恢复、
不复用ID、不删除owner。旧save(ExactAssignment)等同Active save，不能绕墓碑复活；旧base
save也不能复活Revoked。空optional仅不存在，依赖失败必须Failure，不能伪造空查询。
此接口与PG适配需成组接入，不将纯声明测试称为持久化或Set/Revoke应用完成。

SetAssignment普通成功收据追加完整ExactAssignment快照到ReplayResult index14；既有0..12
及RegisteredPrincipalSnapshot13保持不变。原成功的owner、版本、role/permission与exact
scope原样保存，重放不回查可变grant或逆拼base metadata；秘密payload仍不可进入普通收据。
此类型追加必须与PG typed codec/schema成组接入，不等于Set/Revoke应用已启用。

## P05 Runtime 资源、绑定和当前代次（实现中）

Runtime 资源层按 P05 拆分：账号与适配器绑定、适配器注册/当前连接代次、插件安装/实例、
期望状态/配置版本、typed Get/List/GetInstanceConfig 与 operation 查询。业务规则在
`core/include/kiarabot/core/domain/target/runtime_rules.hpp` 与
`runtime_resource_application.cpp`，只依赖内层端口；PG 仓储在
`infrastructure/postgres`，RPC 只做协议转换与稳定错误映射。

**身份层次与代次**：外部 Adapter 不是 Runtime 监督的进程，因此 RegisterAdapter 只分配/
递增 `connection_generation`（重连换代）并携带当前 `runtime_epoch`；Runtime 重启换 epoch，
旧 epoch 或旧 connection_generation 的后续帧拒绝（保留稳定错误映射）。P05 的 adapter
Fence 不填 `execution_generation`：execution 表示一次由 Host 启动建立的实际进程执行，
外部自管实例的可信绑定属于 P08；不得为字段完整伪造该值。心跳/注册后帧的 wire Fence 只带
instance/runtime_epoch/connection_generation，内部 connection/execution ID 由所有者维护，
边界匹配 wire 值后从权威注册记录解析，不能用客户端自报 ID 或当前代次替换旧值。

**管理顺序**：先 RegisterAdapter 取得 assigned_instance_id，之后才允许 BindAdapter；
注册只校验声明账号存在、可用且未绑定其他 adapter，不自动创建绑定。绑定/解绑是显式管理
命令并保留 owner 版本；禁用账号或已删除资源 fail closed；同资源 expected_version 不匹配
返回 CONFLICT，不覆盖。

**List 语义**：Runtime List 是一次当前权威读取，跨页不冻结行集（与 Identity session
snapshot 不同）。page token 由 Runtime 分配的不透明 id 表达，绑定查询条件、认证主体、
授权版本与页大小，并带十分钟窗口；非法、跨主体、跨查询/授权版本或过期 token 返回
CONFLICT（正确输入恢复），读依赖故障返回失败而不是空页/部分页。不新增 snapshot 表。

**授权与就绪**：应用通过 `RuntimeAuthorizationPort` 取得当前 Identity 授权决定；组合根注入
真实实现（mTLS peer + 当前 Identity 授权/委托验证），在接线完成前必须 fail-closed，单测
才允许 allow-fake；禁止从请求自报身份推断调用者。readiness 由组合根按真实观测判定：
业务应用已装配 + Runtime 私有库/schema writer 可用 + Identity 授权链路可用才 ready=true；
未观测或不可用保持 false，测试 fake 只验证判定逻辑，不能使组合进程报 ready。

**P06 外部 Bridge bootstrap（Runtime 与外部消费端均已实现）**：BindAdapter 创建新绑定时
由 Runtime 为 adapter instance 生成一次 32 字节 CSPRNG opaque bootstrap（64 位十六进制文本），
externally_managed 实例在 CreateInstance 时同样生成；Runtime 私有库
`bridge_credentials` 只保存 SHA-256 摘要，明文只在对应 RPC 响应中出现一次，不进入收据、
审计、日志或面板。已存在 active 凭据时不重复发放；解绑最后一个绑定会撤销 adapter 凭据。
`RuntimeBindAdapterResponse.bootstrap_credential`（字段3）/
`RuntimeCreateInstanceResponse.bootstrap_credential`（字段2）由 P05 RPC 返回；P06a-2 的外部
Bridge 服务端已消费该凭据：`bridge_listen`（server-verified TLS）校验 Bearer→sha256 行，
返回 ConnectionAuthorization（≤56s proof/refresh、fence、credential/authorization version），
每帧校验 fence+proof、单飞 refresh，并实现 AdapterBridge 事件接收与 PluginBridge
lease/ACK/Renew/死信/维护 sweep。A1：BindAdapter 不再要求 AdapterRecord 预先存在；首次
bridge 登记用 bootstrap resource_id 创建/补全 AdapterRecord 并要求 `assigned_instance_id`
等于该 id、声明的 account binding 全部属于该实例。B1：外部自管 peer 的目标 execution
generation 为登记连接代表（adapter 固定 1，plugin 首次分配 1 并持久，重连只推进
connection_generation）；Host 受管执行仍使用真实进程代次。

**P06a-2 Bridge 组合（已实现）**：Runtime 组合根在配置 `bridge_listen` 时创建
RuntimeBridgeApplication，并把真实 AdapterBridge/PluginBridge 注册在外部监听；`grpc_listen`
仍只注册 RuntimeService/HostBridge 的 owner mTLS 面。连接授权材料只存在进程内存，PG 只存
bootstrap SHA-256；bridge 维护（sweep/claim/push）挂在每秒有界的 check_dependencies 钩子上，
停进程前 `before_stop` 向活动流发送有界 ShutdownNotice。9 个 internal_dispatcher unary 方法
仍未实现、不在任何监听上实现。

**P06 全链路完成状态（2026-11-02）**：P06a-1/P06a-2 的 Runtime 事件账本与外部 Bridge 已在
真实 PG16.4、临时 TLS 与真实进程上通过 P06b SDK 插件与 P06c Adapter bridge client 的
端到端验收（`ctest -R p06_client_e2e`，注册/授权/refresh/事件 lease+ACK/断线重连新
generation/有界 ShutdownNotice）。P06 target 登记中 bridge_methods、授权/投递/停机字段与
`EventEnvelope.message_received` 已翻 implemented；未覆盖项保持 not_implemented：适配器本地
队列背压 semantic、插件 manifest 的 Identity 授予过滤（P06b/P08）、真实 QQ/OneBot 进程
（P07/M2）、P05 bootstrap RPC 的 target 字段级验收。

**Identity 在线授权接线（P04 就绪后）**：Runtime 组合根在存在 `identity_endpoint` 配置时创建
真实 Identity owner gRPC 客户端，复用 Runtime 服务 TLS 材料作为出站 mTLS 身份：
- PeerAuthorizer 从 gRPC `ServerContext::auth_context()` 提取真实 inbound 证书叶指纹，解析
  Control 委托 JWT 的公开 claims（aud=runtime、有效期≤60s、client_certificate 与叶指纹一致、
  actor/permission 结构合法）构造请求级 AuthenticationContext；公开 claims 只作为请求元数据，
  不单独授权任何业务操作。
- 每条业务操作调用 `IdentityService.CheckAuthorization`（500ms 预算、上限 1s、与命令 deadline
  取更早者）；Identity 是唯一权威：验证 JWT 签名、issuer、audience、origin/current credential、
  当前 assignment 与 recipient peer，返回 allowed/authorization_version/checked_at/denial。
  任何失败（UNAVAILABLE/UNAUTHENTICATED/PERMISSION_DENIED/DEADLINE_EXCEEDED/畸形响应）
  fail closed，不缓存授权、不接受请求自报身份。
- availability 观测 mTLS channel 是否 READY；readiness 要求应用已装配 + Runtime 私有库/schema
  可用 + 该 channel 可用。`identity_endpoint` 缺失时组合根保持 fail-closed 占位与 ready=false。
- 明文 delegation proof 只存在于请求内存（AuthenticationContext.proof），不写入收据、审计、
  operation 或日志。
- 平台 peer 供给：Runtime 的 mTLS client 证书、Principal 与 `identity.authorization.check`
  等显式 assignment 只能由 `kiarabot-identity-bootstrap` 的离线 `platform_peers` 登记，
  与管理员/Control 同事务提交（见 doc/08）；无匿名注册/自绑定或 RPC 绕过。缺少登记时
  Runtime 侧 CheckAuthorization 返回拒绝且 readiness=false，仍 fail closed。
- 委托 proof 是 Identity 签名的不透明 JWT；其 uint64 字段（permission/kind、
  authorization_version、business_hops、source_credential.version）按 Identity 签名器编码为
  **规范十进制字符串**。Runtime 消费者必须按此解析并拒绝数字形式/前导零/越界值，不得用
  宽松 JSON number 猜测。真实双进程 mTLS/PG e2e（允许→撤销 assignment 拒绝→撤销凭据登录
  失败→Identity 停机 readiness=false→缺配置 fail closed）由 `p05_runtime_identity_e2e` 覆盖。
- Runtime exact 资源方法通过 `scope_kinds` 标注（Account/Adapter/Installation/Instance）供
  Identity descriptor-driven resolver 校验；未标注方法（Create/List/GetResourceState/
  GetOperation 等）保持 fail closed，补标注必须同时补齐应用传入的 exact scope。

**持久化说明**：新增 adapters/adapter_bindings/hosts/instance_configs/bridge_credentials 关系与
installations.manifest、instances.desired 列在同一 Runtime 库 v1 初始化 schema 内创建。
该版本没有增量迁移机制，因此结构变更只在初始化 schema 中表达；不修改既有列语义，不重排
`ReplayResult` 既有索引（新增记录追加在末尾，RecordKind 使用相同索引值）。

## P08a Plugin Host 服务实现（2026-11-02，第一阶段）

P08a 把 `kiarabot-plugin-host` 从 readiness 桩推进为可运行服务：
`core/src/application/target/host_service.cpp` 在纯端口
（`HostJournal`/`ProcessSupervisor`/`HostClock`/`HostBridgeTransport`）之上实现受管执行，
`core/src/runtime/platform/host_bridge_client.cpp` 是 Runtime 内部 mTLS `HostBridge` 的客户端，
`core/app/host_composition.cpp` 由严格 host 配置装配 SQLite journal、POSIX supervisor、
mTLS bridge client 与 Host 服务。命令只从已认证 HostBridge 流进入（不暴露 unary HostService
监听）；Host 不读取 desired/注册/配置应用权威，只上报本地执行事实。

本地可靠日志（`infrastructure/src/host_journal.cpp`，SQLite WAL、单写入者）使用
`command_journal`/`managed_executions`/`execution_observations` 三表：命令在 spawn 前持久化并
按 command_id+payload hash 幂等重放；执行以 instance_id+execution_generation 唯一并记录
pid+进程启动时间 marker；观察以 Host 本地单调 sequence 持久化，未上报项在 HostBridge 重新
注册/resync 后补发。已解决命令保留 7 天，未知/未决项保留 30 天后进入人工处置，不静默删除。

执行语义：包摘要必须匹配部署 allowlist（`package_sha256` + `launch_profile`），参数为
`name=value` 且 name 受 HostPackage 白名单约束，环境变量只接受 profile 白名单中的名字，值来自
部署配置；受管实例 bootstrap 凭据由部署以 0600 文件放在 `credential_directory`
（`<instance_id>.bootstrap`，uid=运行用户），只注入 profile 的 `credential_environment`，
不持久化、不写日志、不进入观察。停止使用 TERM→10s 宽限→KILL；无法确认终止时 outcome 保持
UNKNOWN 而不是“已停止”。Host 启动时恢复：marker 仍存活则认领 Running 并补观察；进程已消失但
无已确认退出则上报 UNKNOWN；同一 execution_generation 永不重复 spawn。心跳 5s，Runtime 15s
无心跳只影响在线投影，不改变 process_outcome 为 FAILED。

边界与未完成：Runtime 侧 desired/outbox 与真实 HostBridge 监听属 P06a-2；SDK 配置 ACK 属 P08c；
P08a 的 bridge 客户端在连接丢失后不自动重连（readiness 转 false，需进程重启或后续阶段补齐）；
HostCommandReceipt 契约没有 error 字段，命令拒绝暂以 `persisted=false` 回执表达；包摘要为部署
登记值比较，不做目录内容重算；SQLite schema 无增量迁移（初始化时创建三表）。

## P10a Scheduler 应用与 PG 落地（2026-11-02，第一阶段）

P10a 在 `core/src/application/target/scheduler_service.cpp` 实现一次性任务应用：
`CreateTask` 同时支持 UTC `run_at` 与 IANA `local_time`（严格二选一，`LocalCivilTime` 只经
`SchedulerTimeZonePort` 解析；DST gap/fold、非法时区为 INVALID_ARGUMENT，时区数据缺失为
UPSTREAM_UNAVAILABLE，绝不猜测 UTC 偏移），时间必须晚于数据库当前时间且不超过 366 天
horizon，payload ≤64 KiB；创建命令按 idempotency key+请求指纹复用 command_receipts，重复
创建返回原任务；活跃任务容量为实例 1024、安装 8192。`CancelTask` 仅在任务 PENDING 且
occurrence 尚未创建时可取消（交接意图建立后返回 CONFLICT），取消/创建均先持久化再提交。
`GetTask`/`ListTasks`（owner keyset 分页 + 可选状态过滤）严格按认证上下文派生的 owner
隔离；`GetOccurrence`/`GetOperation` 做 owner/resource 校验。

触发生命周期（P10a 不调用 Runtime）：维护扫描 `due_tasks` → 每个到期任务独立事务内创建
occurrence（ACCEPTED）、Scheduler 所属 Operation、持久化 trigger outbox 交接意图，并把任务
置为 RUNNING；重复扫描不重复建 occurrence/outbox。`ClaimTriggers` 使用通用 outbox claim；
`ConfirmTriggerAccepted` 在 Runtime 确认后把 occurrence 置为 SUCCEEDED（记录
accepted_event_id/acknowledged_at）、任务置 COMPLETED——COMPLETED 语义严格为“Runtime 已持久
入队”，不是插件已执行。确认丢失重试同一 occurrence 幂等；outbox 未确认且超过 7 天有效期时
`ExpireStaleTriggers` 将 occurrence 置 EXPIRED、任务置 FAILED(TIMEOUT)，不自动重放。
P10a 的 outbox claim 只暴露给维护/P10b 交接代码；Runtime AcceptTimerTrigger 调用、SDK 任务方法
与 Host 执行链路属于 P10b/c。

PG 适配器为 Scheduler 私有库提供 owner/due SQL 投影（list_tasks、容量计数、due 扫描、
occurrence owner/in-flight 扫描），并在合并 P07a/P11a 后的 schema 上落地观察列、quota counter
关系与 due/owner 索引（Scheduler 私有库 v9）。四服务 descriptor/relation_count 在合并 schema
上用真实 PG16.4 重新实测并写回 manifest：Identity 129 / Runtime 120 / Scheduler 77 / Model 71
（Scheduler relation count 含 P10a 新增 quota 表与四个索引；其余与 main 一致）。RPC 组合根与
SchedulerService unary 监听见下一节；真实 Runtime AcceptTimerTrigger 交接与 Identity
delegation 授权链路仍属 P10b/后续集成。

### P08a 离线启动补充（2026-11-02 回归修复）

HostBridge client 的注册改为后台有界重试：`HostService::start()` 在 Runtime 不可达时仍成功
（journal 打开、执行恢复、探针就绪），readiness 由 `bridge.registered()` 决定并保持 false；
注册成功后 fence 接管、`bridge_ready` 触发 pending observations 补发。连接丢失后 client 重新
进入注册循环，不伪造 registered/stopped；仅配置非法（缺失/格式错误 host 材料、无 host_id/
凭据）才让启动失败。P02 生命周期测试使用不可达 `runtime_endpoint` 验证“离线进程存活 +
readiness=false + 可恢复注册”。

### P10a Scheduler RPC 与组合根（第二阶段）

`core/src/runtime/platform/scheduler_rpc.cpp` 是冻结的 SchedulerService 六方法 unary 适配器
（CreateTask/CancelTask/GetTask/ListTasks/GetOccurrence/GetOperation）：只做
RequestContext 校验（UUID/必填 deadline/30s 上限/幂等键 17–128 字符）、
scope 透传给 authorizer、wire⇄domain 转换（TaskRecord 的 target 仅 conversation id/type，
消息列适配器/账号在 TaskRecord 中不出现，内部 ConversationKey 的 adapter/account 留空、
由 P10b 按 owner 实例 Runtime 绑定解析）、以及稳定 gRPC 错误映射（v1::RpcFailure，
CONFLICT→ABORTED 等）。业务规则/收据/交接仍全部在 `SchedulerCommandHandler` 应用层；
RPC 依赖接口而非具体仓储，便于 fake handler fixture。

组合根 `core/app/scheduler_composition.cpp`：严格 `database` 配置存在时打开 Scheduler 私有
PG 库并装配 `SchedulerService`（CSPRNG occurrence/operation id、IANA POSIX zoneinfo 适配器）；
未配置数据库时使用 fail-closed 工厂，进程仍启动但 readiness=false（P02 无库生命周期）。
readiness 依赖库/时区数据，且由于 Identity delegation/current-authorization 链路尚未接入，
authorizer 当前 fail-closed，`FailClosedApplication` 强制 ready=false、Identity=Unavailable；
RPC 请求返回 UNAUTHENTICATED/INVALID_ARGUMENT（P02 探针接受该拒绝作为监听存活证据）。
维护循环由 `ServiceComposition::check_dependencies` 每秒执行（有界）：读取数据库权威时间后
due sweep（最多 10 个）、过期 trigger（最多 10 个）、command_receipts 与 inbox 保留期清理；
busy/Conflict 不终止进程，其它数据库故障按服务生命周期退出。

注意：`infrastructure::TimeZoneResolver` 链接 reference protobuf 生成物，与 owner 服务使用的
主 protobuf 库不能同进程混链（ODR/duplicate symbol），因此组合根实现了同错误语义的 POSIX
zoneinfo 适配器（`/usr/share/zoneinfo` + `mktime/localtime_r` 往返检测 DST gap/fold，1970–2100、
非法/不存在 zone→INVALID_ARGUMENT，zoneinfo 缺失→UPSTREAM_UNAVAILABLE）。该适配器尚未有独立
单测（应用层 fake port 已覆盖 gap/fold 拒绝），需在 P10b 真实运行环境补 zoneinfo/宿主缺失矩阵；
后续若统一 protobuf 生成物，应切回基础设施 resolver。容量计数仍为 SQL 聚合扫描（quota counter
关系保留给后续事务化缓存），RPC List 的分页 token 为 owner keyset task_id（非 10 分钟不可变
snapshot，P10b/后续补齐）。

### P10b Runtime AcceptTimerTrigger 与 Scheduler 交接（第一阶段）

Runtime `RuntimeResourceApplication::accept_timer_trigger` 接收 Scheduler 的一次性触发交接：
校验 TimerAccept 权限（online Identity 授权）、source 必须 Scheduler、handoff.message/occurrence_id/
event_id 一致、operation/resource 均指向同一 task_id、`schedule_version` 与 handoff.version 一致、
payload ≤64 KiB 且 `handoff.payload_hash` 与 payload SHA-256 相等、`valid_until` 未过期
（否则 TIMEOUT）；随后把 `TimerTriggered` 事件（event_id=occurrence_id）与唯一 owner-instance
fan-out target 在同一 UoW 内持久化。重复交接按 event id+fingerprint 返回 duplicate=true，不重复
入队/建 target；同一 event id 不同内容返回 CONFLICT。RuntimeService 新增 AcceptTimerTrigger
unary 处理器，wire HandoffContext 在边界转换为 domain handoff 并由边界序列化 TimerTriggered
EventEnvelope（应用层不依赖 protobuf）。

Scheduler 侧新增 `TimerDeliveryPort` 与 `SchedulerService::deliver_triggers`：维护循环 claim
trigger outbox → 在 UoW 外调用 Runtime.AcceptTimerTrigger（生产 `SchedulerRuntimeClient` 使用
服务 TLS 材料的 mTLS）→ 成功后 ConfirmTriggerAccepted（同一 occurrence 幂等）；Runtime 永久
拒绝（INVALID_ARGUMENT/PERMISSION_DENIED/CONFLICT/CAPABILITY_NOT_SUPPORTED）将 occurrence/task
置 FAILED 并结束 outbox 交接；UNAVAILABLE/DEADLINE_EXCEEDED/RATE_LIMITED 不伪造 COMPLETED，
保留 claim 到期重试并复用同一 occurrence/event id。Scheduler 组合根在配置 `runtime_endpoint`
时装配真实客户端，否则保持可启动但 Runtime 依赖不可用。Scheduler RPC authorizer 仍为
fail-closed 占位（真实 Identity delegation/current-authorization 接线与多进程 e2e 未在本次完成，
见 doc/08 未完成项）。

### P10b Scheduler 真实授权（第二阶段）

Scheduler RPC authorizer 不再是 fail-closed 占位：`SchedulerIdentityClient` 在
`runtime_endpoint`/`identity_endpoint` 配置存在时接入。authorizer 从真实 gRPC
`ServerContext` 提取入站 mTLS 叶证书指纹，用 `decode_delegation_claims(..., audience=Scheduler)`
校验委托结构/audience/客户端证书绑定，再要求委托权限绑定中存在 **Installation** resource
scope（冻结 delegation claim 无独立 installation 字段，按“Runtime/Identity 签发委托时必须绑定
Installation scope”的约定派生），owner instance 由委托 subject 派生；随后向 Identity
`CheckAuthorization`（full_method + 请求 ResourceScope + known_authorization_version）做在线
current-authorization 校验。缺 proof/installation 绑定、aud 不符、过期 deadline、Identity 拒绝
或依赖故障一律 fail-closed（Unauthenticated/PermissionDenied/UpstreamUnavailable）。组合根在
identity_endpoint 存在时把 Identity 可用性接入 readiness：库+时区+Runtime delivery+Identity
全部 available 才 ready=true，任一依赖故障 readiness=false。

未完成/阻塞：**真 PG+多进程 e2e 尚未实现**，原因是 Scheduler→Runtime 的
`AcceptTimerTrigger` 调用没有委托凭据来源：Runtime 的 owner 方法 authorizer 只接受
Identity 签发的 delegation proof，而 Scheduler 组合根/契约没有配置 Scheduler 服务主体的
delegation（或 cert SAN→平台服务主体的映射约定）；同时 Runtime→Scheduler 的 delegation
issuer 必须按上述约定绑定 Installation scope，否则真实调用会被 fail-closed 拒绝。需要
Identity/Runtime 约定：平台服务主体免委托的 internal peer 授权，或为 Scheduler 预置
delegation 配置字段；在缺失字段补齐前不能宣称三进程交接通过。上述阻断项已按 lead 要求上报。

### P10b-2 平台服务主体交接与 Scheduler 授权闭环

Scheduler→Runtime `AcceptTimerTrigger` 按 P05 platform-peer 语义授权：Scheduler 以部署注册的
平台 peer 身份（服务 TLS 叶证书 URI SAN `spiffe://kiarabot/scheduler…`；Identity
`platform_peers` 离线登记 principal/certificate/assignment）调用 Runtime internal mTLS owner
方法；Runtime 侧只认证平台 peer 身份并校验 handoff 完整性字段（source_service=Scheduler、
operation owner=Scheduler、occurrence_id/event_id、payload_sha256、valid_until、
schedule_version），**不要求用户 delegation**。Scheduler 对 Runtime 的 RPC 调用也不再携带
delegation；Runtime 其他 owner 方法仍走原有用户委托/在线授权，不共享该豁免。该豁免由边界在
验证真实 mTLS 叶证书后设置 `platform_peer_authorized`，应用层仍执行全部 handoff 校验。

Scheduler RPC 侧真实授权：入站 mТLS 叶证书 + `aud=scheduler` delegation 结构/证书绑定校验，
owner instance 由委托 subject 派生，installation 由委托权限绑定中的 Installation resource
scope 派生（Identity 签发约定：Runtime 受管实例的 delegation 必须包含 Installation scope，
否则 fail-closed），再调用 Identity.CheckAuthorization；因此 Scheduler 六个方法在
`proto/kiarabot/v1/scheduler.proto` 中使用既有 `scope_kinds` 注释声明 TASK/INSTANCE 精确
scope（复用 P05 扩展，无新增字段/权限/wire 变更）。

真 PG16.4 多进程 e2e（`p10_timer_e2e`）覆盖：真实 Identity(bootstrap platform peers)+Runtime+
Scheduler 三进程 mTLS；Control 签发 Scheduler-audience delegation 创建任务→到期→outbox→
Runtime 入队+唯一 owner fan-out→confirm→COMPLETED；重复平台交接 duplicate、过期 handoff
TIMEOUT、撤销授权后创建任务 PERMISSION_DENIED。容量/EXPIRED/确认丢失/崩溃恢复矩阵仍需后续
阶段补齐；P10c SDK 任务方法不在本阶段。

### P13a Runtime 生命周期协调与清理（2026-11-02 已实现应用/PG/RPC）

`RuntimeResourceApplication` 新增 `begin_delete`/`restore_resource`/`lifecycle_maintenance`，接在冻结的 `RuntimeService.BeginDelete/RestoreResource/GetOperation` 上，**不新增 proto 消息、字段、RPC 或权限**。`BeginDelete` 在单个 Runtime UoW 内提交：校验 `resource.manage`、资源存在与预期版本，置 `DELETING`（实例同时 desired=STOPPED），撤销 Runtime 本地 `bridge_credentials`，创建 operation 与 `runtime/host/scheduler` 参与者，写 `lifecycle_tombstones`（删除时间、30 天恢复截止、180 天清理截止），返回 `OperationRef`。没有外部参与者的资源（账号/适配器/主机）同事务直接进入 `DELETED`；实例/安装保持 `DELETING`，由每秒维护钩子通过 fail-closed 端口推进 Host stop/drain 与 Scheduler 任务取消。P08b-3/P10b 客户端未接线时端口缺失，参与者记 `BLOCKED`/可重试，**绝不伪造 stopped 或全局完成**；operation 的 `participants` 通过既有 `OperationProgress` 投影给 Control。

`RestoreResource` 只接受 `DELETED` 且未过 30 天恢复窗、版本匹配的资源，回到 `ACTIVE` 并删除墓碑；不复活已撤销凭据、已取消任务、过期租约或旧 token。超过窗口返回 `CONFLICT`。`lifecycle_maintenance` 每轮有界扫描墓碑：依赖恢复后完成 `DELETED`，超过 180 天转 `PURGED` 并移除墓碑；每 60 轮执行一次有界保留清理（receipts/inbox/outbox/audit/operations/storage receipts，单项失败只置 `cleanup_failed`，不撤销删除 gate）。Runtime schema 升至 v13：operations 增 `created_at/updated_at/retain_until` 与保留索引，新增 `operation_participants`、`lifecycle_tombstones`；Identity/Runtime/Scheduler/Model 的实测 descriptor/relation_count 已同步。`BeginDelete/RestoreResource` 的 proto `scope_kinds` 元数据补齐五个 Runtime 资源种类（无新字段/RPC/权限）。真实双进程 Runtime↔Identity e2e 已覆盖 BeginDelete→GetOperation(participants)→Restore，P08b-3/P10b 接线后的真实 Host/Scheduler 参与者验收仍待补。
### P10b-3 定时交接残余矩阵（真 PG16.4 + 真 mTLS）

`p10_timer_e2e` 在同一套三进程真实环境中继续覆盖残余矩阵：
- **容量**：实例 1024 / 安装 8192 非终态任务上限，直接填充 owned Scheduler 表后 RPC 创建返回
  RATE_LIMITED，不产生超发任务；
- **EXPIRED**：Runtime 停机使首次交接可重试，到期后把 occurrence handoff 窗口改到过去，
  maintenance 将 occurrence 置 EXPIRED、task 置 FAILED(TIMEOUT)，且不自动重放；
- **Scheduler 崩溃恢复**：due sweep 建立 occurrence/outbox 后 SIGKILL 并重启 Scheduler，
  恢复后 outbox 收敛到 COMPLETED，不产生第二次事件；
- **confirm 丢失/重复**：Runtime 已持久化事件而 Scheduler 未确认时，重启 Runtime 并由 outbox
  重试，Runtime 返回 duplicate、Scheduler 确认，事件与 fan-out target 恒为 1；
- **撤销授权后 maintenance**：RPC 撤销 task.schedule 后，撤销前已受理的任务仍由平台
  peer maintenance 完成；撤销后的新命令 fail-closed。
矩阵仍限于单 installation/单 Runtime 进程；多 installation、跨实例容量与长时间 7 天真实等待未覆盖。

### P10b-3 Installation-scope 约定复用检查

按 doc/21/22 的签发约定检查了 Runtime 委托接受面：Scheduler 是当前唯一需要从 delegation
permission bindings 派生 Installation resource scope 的消费者；Model gateway 只使用
caller subject/owner（不消费 installation 字段），Runtime Action 使用
authentication.subject 与请求 owner_instance/account，不接受 installation 输入。二者目前
**无需** installation scope 派生/校验；若 P11d/P07 后续引入按 installation 隔离的 Model/Action
行为，必须复用 Scheduler 的“缺失 Installation binding 即 fail-closed”规则，届时再实现。

### P13b Host/SDK/Adapter 优雅 drain 与崩溃恢复（2026-11-02）

`HostService::stop()` 与 Host 组合根 `before_stop`（SIGTERM/SIGINT）按固定顺序收尾：先对每个未终态 `managed_executions` 执行有界 TERM（grace≤10s，supervisor 负责 TERM→KILL），再 `flush_pending_locked()` 把最终 execution observation 通过 HostBridge 上报，随后对 Host 本地 SQLite journal 做一次有界 prune，关闭 journal，最后停止 HostBridge 连接。任何无法确认的终止保持 `ManagedExecutionState::Unknown`/`ExecutionOutcome::Unknown`，shutdown 不把 UNKNOWN 改写成 stopped/succeeded；同代次恢复不重复 spawn（P08a marker 规则）。Host 进程以 0 退出；`kiarabot-plugin-host` 的 `host_connections` 代次/epoch 仍由 Runtime 注册投影维护（P08b）。

SDK `ShutdownNotice` 收尾：停止接收新事件，但 reader 在 drain 窗口内继续处理 delivery receipt/host/action response，等待已受理事件队列排空后才结束 session；超窗口则停止等待（不无限阻塞），`on_stop('shutdown')` 最后执行一次，`_draining` 抑制重连，进程正常退出 0。Adapter `ShutdownNotice`：在该连接内不再发新事件/新 Action，未确认事件保留本地 outbox，drain 窗口后按退避重连（Runtime 重启新代次），`on_shutdown` 顺序为 stop event → 取消连接任务 → 关健康端口 → close action journal/outbox；致命鉴权失败以非零码退出。Adapter 在异机部署时与 Host 无共享 SQLite，仅通过 Runtime/PluginBridge 观察事实，Host 停止不代表 Adapter 已停止。

### P13c Host 参与者真实接线与 P13 target 登记（2026-11-02）

Runtime 生命周期 maintenance 的 Host 参与者不再使用 fake 端口：`RuntimeHostLifecycleParticipant` 在维护线程外开启短 UoW，按与实例 desired-state 一致的确定性 command identity 向 P08b-3 `host_command` outbox 写 `StopExecution` 快照（host/instance/execution_generation/desired_version/package hash/payload hash），同 command_id 重放幂等。只有 outbox 出现持久 receipt 且 `receipt_outcome` 为 `KnownSucceeded/KnownFailed` 时才返回 Done；`Pending/Dispatched/Unknown` 持续 BLOCKED（UNKNOWN 不伪 stopped），Host 明确拒绝（persisted=false）返回可观测 BLOCKED(Conflict)，等待 P08b-3b 的 HostBridge 命令下发/回执与 P13b Host drain 语义完成。Scheduler 参与者仍按 P10 契约的 `ListTasks/CancelTask` 实例委托接口预留：Runtime→Scheduler 的真实 delegation 签发/endpoint 配置未在 composition 中提供前保持 BLOCKED，测试用 fake 端口验证“离线不完成、恢复后收敛”。Control 的 operation 快照（participants/blocked/reason）与 P12c 路由已就绪，本阶段只补 target 登记，不重复实现 REST。

P13 target 翻转（证据：P13a 应用/PG/RPC + 真实 Runtime↔Identity e2e + P13b Host drain 矩阵）：`RuntimeGetOperationRequest/Response`、`RuntimeBeginDeleteRequest/Response`、`RuntimeRestoreResourceRequest/Response`、`Operation`、`OperationRef`、`OperationProgress` 及字段、`OperationState.*`、`operations.getOperation`。`RuntimeService.BeginDelete/RestoreResource/GetOperation` 的 target_spec 状态同步为 implemented；REST 的 beginDelete/restore 操作仍 not_implemented（Control 未接线），getOperation 以 P12c owner gateway 实现并保留 REST 规则的 unavailable marker。

### P13d 生命周期可观测性最小面（2026-11-02）

不新增 API、权限、RPC 或 metrics 通道。Runtime 的 once-per-second dependency hook 在 lifecycle maintenance 有实际进展或异常时向 owner 结构化日志写一行（doc/08 键值格式，仅计数/稳定码）：

`operation=lifecycle_maintenance outcome=ok|degraded reconciled=N completed=N blocked=N purged=N pruned=N cleanup_failed=true|false`

- `blocked>0` 或 `cleanup_failed=true` 时 `outcome=degraded`，用于日志告警/检索；`pruned` 仅在 60 秒保留批次非零时出现；cleanup 失败是**非致命**的（dependency hook 本身失败才会退出进程），不会伪完成 operation，也不清理未决记录。
- operation/participant 进度仍只由现有 `GetOperation`（participants schema 含 state/reason/next_attempt_at）投影；BLOCKED/UNKNOWN 不映射成全局成功。
- begin_delete/restore 等命令仍写现有 owner 审计（`ListAudit` 可按 resource/时间过滤）；`ListLogs` 是审计的脱敏投影，不新增业务日志表。
- 容量/租约/积压通过现有 owner 表只读 SQL 核对（见 doc/08 P13d 排障步骤）；Metrics/告警、集中日志和真实生产规模仍是 P14/环境项。

### P09f Storage/Namespace 退出证据（2026-11-02）

P09 storage 退出证据以现有测试为准：`p09a_runtime_storage_application`（application 层 scope/condition/TTL/quota/receipt/成员 gate/管理 operation）、`p09_postgres_storage_records`（owned PG16.4 四表/CAS/收据重放/成员/配额/禁用/重启持久）、`p06_bridge_acceptance` P09b 段（真 TLS PluginBridge `HostRequest.storage_*` 帧：私有/共享 scope、CAS/stale、TTL/重放不续 TTL、过期、配额 RATE_LIMITED、显式 installation_shared_id 拒绝、共享成员/禁用）。target 仅翻转有上述证据的 message/field/semantic；unary `Runtime*Storage*` 为 internal_dispatcher，PluginBridge 帧是插件唯一对外 storage 面。quota 多连接横压、storage 断线重连专项和共享成员删除不误删他人数据的直接断言仍待补；真实生产规模/长时保留待 P14 环境。
## P06f Runtime 事件 fan-out 根因记录（2026-11-02）

真实 adapter→Runtime 事件链路的 fan-out 只在事件以 `MessageReceived` 进入且插件 profile 在线时生成 `delivery_targets`；`AdapterStatusChanged` 等状态事件不匹配消息订阅。P07d 复现发现：fake OneBot 的字符串 `message_id` 使 NoneBot 解析为通用 Event（matcher 不执行），同时 adapter mapper 误把平台 `self_id` 与 Runtime account UUID 比较，导致消息在 outbox 前被拒绝；只有状态事件到达 Runtime，故 `delivery_targets` 为空。修复采用独立 `platform_self_id` 校验、e2e 使用整数 `message_id`，并新增真实订阅/不匹配/离线/重连断言；Runtime fan-out/lease/ACK 实现本身无需改动。

### P07g Action 退出证据（2026-11-02）

P07 Action 退出证据：领域规则 `p07_action_rules`、真 PG16.4 `p07_postgres_action_records`、真 TLS/PG/Runtime `p06_bridge_acceptance` P07 段（submit/dispatch/terminal/evidence/receipt 幂等、配额窗口、取消、EXPIRED 迟到证据、断线重连回带旧 attempt）、真实 adapter 进程 `p07_adapter_e2e`、SDK `test_actions.py` 28 项、Control REST `p12_control_owner_gateway` 均通过。target 仅翻转有这些证据的 Action 帧/状态/字段/权限/语义；ActionPolicy、react/delete/mute/kick/approve payload、plugin 权限过滤独立项与 ListActionMetadata 未翻，待 P07f-1/2 wire 证据或接线。真实 QQ/NapCat 平台副作用、reaction API、EXPIRED 迟到故障注入与跨主机仍待环境。
## P11f Model 退出证据（2026-11-02）

Model owner 的 11 个 RPC（GatewayChat/GetCallStatus/Resolve/元数据/配置/Disable/Enable/GetOperation 等）由 P11 全套测试实测（9/9），应用层两事务/UNKNOWN 保守计费/AEAD secret/HTTP provider 映射/Identity 调用者校验均有覆盖；插件入口仅 HostRequest.model_chat 的 bridge fixture 证据，真实插件→Runtime→Model→Provider 单链路待 P05b 委托语义确认。ModelChatDisposition/ModelCallDisposition 不作为线上客户端能力验收（无暴露消费证据），streaming 明确不做，GetOperation 仅 model owner 分支。详见 `reports/modules/architecture-p11-model.md` P11f 段。

## P05f Identity/Runtime 退出证据（2026-11-02，main@439fb9e）

- 本机 full-locked 完整构建后运行 `ctest -R 'p04_identity|p04_control_auth_https|p05_runtime|p03_postgres|p02|architecture_p02'`：**46/46 passed**；其中 `p04_identity_*` 28 项、`p05_runtime_*` 4 项（含真实双进程 Identity/Runtime mTLS/PG16.4 的 `p05_runtime_identity_e2e`）、`p04_control_auth_https`、`p03_postgres_database`、P02 相关 12 项。命令与逐组结果见 [P04 报告](../reports/modules/architecture-p04-identity.md) 与 [P05 报告](../reports/modules/architecture-p05-runtime.md) 的 P05f 段。
- target 核对（只读）：IdentityService 方法 target implemented 7/23（RegisterPrincipal/RotateCredential/RevokeCredential/GetPrincipalStatus/GetCredentialStatus/ListAudit/ListLogs），消息 17、字段 59；RuntimeService 方法 target implemented 10/55（P09a-2 namespace、P13 生命周期、P12e audit/logs、QueryActionResult），消息 23、字段 77。
- 未翻项（未改契约）：P04 Login/Refresh/Validate/GetPrincipal/ListSessions/RevokeSession、IssueDelegation/CheckAuthorization、Exchange/Assignment/Invalidation/VerificationKeys、ListPrincipals；P05 核心 Runtime 资源方法（CreateAccount…GetInstanceConfig）；`bootstrap_credential` 字段级、P05b 实例委托、`HostRequest.storage_compare_and_set`/host operation owner。原因与证据逐项见两份模块报告的 P05f 段。
- 剩余缺口：P05b 委托语义待用户确认；真实目标栈 live、Host peer 生产证书签发/轮换、跨主机网络分区演练未验收；P06 外部 Bridge 的 bootstrap_credential 消费未形成端到端证据。
### P13e Host 联合删除/恢复补充（2026-11-02）

HostBridge live StopExecution/DrainHost 帧已由真 PG16.4+真 Runtime+fake mTLS Host 验证下发与
receipt 落库；Host 离线/未回执保持 Pending/Unknown，不推断进程停止。共享 namespace 成员撤销
或 namespace 删除只影响本 namespace/成员关系，私有 namespace 与其他 installation 数据保留
（真 PG+storage 帧断言）。仍缺 BeginDelete/Restore 与真实 Host 的单一联合 e2e 与 Scheduler
participant 生产 ListTasks/CancelTask 委托接线；P13 退出判定未通过，待后续增量或用户确认测试
注入方式。

### P06g 真实 adapter+Runtime+受管 SDK plugin 全总线 e2e（2026-11-02）

`p07_adapter_e2e` 现在是单一真实进程链：真 PG16.4 + 真 mTLS Runtime + 真 NoneBot adapter +
fake OneBot WS + 独立 SDK plugin 子进程（真实 PluginBridge）。平台消息经 adapter 持久入
Runtime 事件 inbox，按订阅投递到 SDK plugin 并 ACK；plugin 提交 send_message 与 policy-gated
mute（真 PG action_policies），Runtime 派发 adapter 执行平台 API，result/evidence/receipt 回
Runtime，plugin 查询终态。重复平台帧不产生第二事件/Action，adapter 断线重连不重复执行；
非订阅/离线插件不伪造 delivery target；post-dispatch cancel 为 CONFLICT。真实 QQ/NapCat 与
单链 UNKNOWN/EXPIRED 迟到证据仍是未验收项（后者已有手工 wire fixture 证据）。


### P10k PluginBridge task 帧 ingress 与 SchedulerTaskPort（2026-11-03）

Runtime `PluginBridge` 现在消费 `HostRequest.task_create/task_get/task_cancel`：从已认证 P06 会话派生 instance/installation，校验注册 profile 的 `host.tasks.v1`、`task.schedule`/`task.read` 权限及 `event.timer.triggered` 订阅，校验 request/trace/idempotency、task_id/目标对话/payload 上限，稳定映射 `HostResponse.task_*` 与 wire 错误码；客户端自报 owner/installation 一律不采纳。

新增 `RuntimeTaskApplication`（`core/src/application/target/runtime_task_application.cpp`）和 `SchedulerTaskPort`（`core/include/kiarabot/core/ports/target/scheduler_task.hpp`）；组合根可选配置 `scheduler_endpoint` 并装配 `SchedulerTaskClient` typed client（参考 RuntimeModelClient）。**真实 Runtime→Scheduler 委托来源尚未确认（P05b 同类语义）**，生产 composition 注入 `unavailable_scheduler_task_delegation()`：delegation 为空时稳定返回 `UNAUTHENTICATED/UPSTREAM_UNAVAILABLE`，端口缺失时返回 `UPSTREAM_UNAVAILABLE`，绝不伪造任务成功；测试注入 fake `SchedulerTaskPort` 验证成功/拒绝/幂等/取消/CONFLICT/超时。未改 proto、权限或 RPC。**待确认**：冻结的 `TaskCreateRequest` 只带 `target_conversation` 字符串、不带会话类型；P10k 暂按 GROUP 构造 Scheduler `ConversationKey`，需用户确认任务目标是否允许私聊或如何在 wire 上表达类型（否则私聊任务的事件 target 类型可能不准确）。另真实 Runtime→Scheduler delegation 来源仍需用户确认。
