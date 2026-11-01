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

### P02 实际传输边界

`runtime/platform/service_rpc.cpp` 注册 IdentityService、RuntimeService、HostService、
SchedulerService、ModelService 的生成 Service 默认实现；Runtime 另注册 AdapterBridge、
PluginBridge、HostBridge。全部业务方法返回 UNIMPLEMENTED，不执行授权成功分支或写状态。
Control 不创建入站业务 gRPC。生成协议及 gRPC 只存在外层，五个 application 无协议依赖。

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

RegisterPrincipal 同事务创建无秘密的 PENDING CredentialStatus 引用（credential_id/principal_id/binding/version/created_at/checked_at）；尚未发放的 PENDING 不得伪造 expires_at。GetCredentialStatus 以 identity.principal.read 在线读取同一记录，默认2秒/最多5秒；ACTIVE/EXPIRED 必须有 expires_at，REVOKED 可保留原到期。状态查询不返回秘密、摘要或可重放材料。初次实际发放仍用 RotateCredential 单次 CredentialDelivery，未知结果不得从状态查询恢复秘密。当前未实现，不启用生产路由。

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
