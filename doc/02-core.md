# C++ Core 设计

T08a 增加同进程 IControlApplication v1 类型化端口与请求形状、默认角色、HTTP 错误映射辅助函数。它接收凭据而非调用者自报身份，真实会话验证/查询/管理实现由 T08b 完成；控制端口与 PluginSession 权限不混用。API/WS 决策以 OpenAPI/AsyncAPI 为准，Core 不依赖 HTTP 框架或面板类型。

T07a tests/acceptance/run.sh 提供独立闭环验收入口。test_closed_loop.py 将真实 Core/SQLite/SDK 与协议 fake 平台相连，验证权限拒绝无平台副作用、重复事件、平台已执行但 ActionResult 丢失后的断线/重启恢复，以及按原 action_id 回放结果。与模型/任务验收一同执行；真实平台联调结果单独由 T07b 记录。

T06c TaskScheduler 实现 IScheduler，规则和事务属于 Core，IANA 时间转换通过 ITimeZoneResolver 端口注入。HostDispatcher 将任务操作交给调度服务；主进程在启动桥之前恢复领取状态，停机先停止并 join 调度线程再停桥。每个 tick 的入队与任务终态同一 RuntimeStore 事务，复用 RuntimeEngine 的 inbox/逐插件投递。

T06b HostDispatcher 通过 IModelProvider 注入模型实现，使用已认证 PluginSession 的 model.chat 权限；调用前提交审计，释放事务/存储锁后调用 Provider，再单独记录结果审计。该外部操作无法与数据库原子提交，不提供自动重试或结果幂等。Bridge 仅在 Provider 配置成功且客户端请求时协商 host.model.v1；基础设施实现和密钥只由启动组合根装配。

T06a HostDispatcher 使用已认证 PluginSession 和 RuntimeStore 事务处理存储请求，默认拒绝未获授 storage.namespace 的插件；业务判断在 Core，SQLite 只保存数据。限额和 TTL 由 Core 决定。该路径复用 RuntimeTransaction 作为统一原子工作单元，不调用彼此独立提交的存储/审计端口；旧 IUnitOfWork 接口仍是后续扩展端口，并非另一套已接线事务。

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
