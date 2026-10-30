# C++ Core 设计

T06a HostDispatcher 使用已认证 PluginSession 和 RuntimeStore 事务处理存储请求，默认拒绝未获授 storage.namespace 的插件；业务判断在 Core，SQLite 只保存数据。限额和 TTL 由 Core 决定。该路径复用 RuntimeTransaction 作为统一原子工作单元，不调用彼此独立提交的存储/审计端口；旧 IUnitOfWork 接口仍是后续扩展端口，并非另一套已接线事务。

## T04b Action 接线范围

ActionRouter 通过 RuntimeStore 原子保存请求、认证所有者、规范化 fingerprint、派发标志和结果；插件申请 message.send 且获授后才可提交。当前实现 send_message 的群/私聊纯文本子集，其余 payload/消息段明确 CAPABILITY_NOT_SUPPORTED。适配器须声明 action.send_message.text.v1；账号和实例由注册上下文约束。提交、取消、派发和结果更新均记录事务审计；任何提交失败不调用平台。

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

```text
core/
  CMakeLists.txt
  include/kiarabot/core/
    domain/
      entities/
      value_objects/
      policies/
    application/
      event_router/
      command_dispatcher/
      services/
    ports/
      model_provider.hpp
      conversation_store.hpp
      scheduler.hpp
      audit_logger.hpp
    runtime/
      grpc_server/
      plugin_host/
      adapter_registry/
  src/
    domain/
    application/
    runtime/
    main.cpp
  tests/
    unit/
    integration/
    core_smoke.cpp
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

### 其他模块接入需求

- 插件宿主应在接收事件时确定订阅插件，传入其 ID，并使用注册清单中的 `ordered`；从已认证插件连接确定 ACK 的插件身份，将 ACK、拒绝及可重试错误映射到逐插件操作。连接断开或进程重启后，确认旧投递者停止再恢复未确认投递；重放时使用逐插件 trace ID 更新解码后的 envelope。
- 基础设施需实现新的逐插件投递、持久化接收顺序和原子事务端口，保证 `(event_id, plugin_id)` 唯一以及事件删除时投递记录级联删除；提供隔离以使队列和会话顺序判断与写入原子化，失败提交必须可回滚。现有内存实现不提供生产持久化。
- 控制 API 和面板未来接入死信查询、重放时需携带目标插件身份，并保留其他插件的状态；当前任务不修改其 API 契约或实现。
- PluginSDK 与插件应按 `event_id` 幂等处理重复投递。当前 ACK 协议没有尝试编号，不能区分旧尝试的延迟 ACK；若未来需要精确区分，须先评估并更新公共契约。

## 基础设施端口契约

`core/include/kiarabot/core/ports.hpp` 定义 Core 内部端口，直接使用生成的 `kiarabot.v1` Host 消息和 `Error`，不复制跨语言类型。端口返回 `PortResult<T>`：成功值或已有协议 `Error`，不可把依赖故障伪装成未找到或重复。Core 应用服务负责鉴权、业务策略和事务边界；命名空间及任务所有者由 Core 从认证身份传入，基础设施不能接受插件直接访问。

Unit of Work 将 Repository、命名空间、任务、幂等结果和审计写入绑定到同一事务，显式 commit 或 rollback；销毁未提交事务须回滚。模型端口不暴露凭据，超时与结果通过端口表达。调度端口的领取和完成/重试操作必须持久化，重启恢复与取消语义见 `doc/01-runtime-semantics.md`。审计端口只接受明确的标识和状态字段，不接受消息正文或密钥。文件存储以逻辑键访问，不向调用方暴露文件系统路径。

## 结构化日志

日志端口将在后续日志任务中实现。每个公开运行时操作和每条拒绝、重复、超时、重试、死信、存储失败及状态转换路径都必须写一条结构化记录。记录至少包含：

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

信号在主线程初始化时阻塞并通过 sigwait 同步消费，避免异步处理器修改业务对象。未来工作线程必须在信号屏蔽后创建，并在结束前停止和 join。无参数模式没有业务工作线程、端口监听或存储接线；显式 gRPC 模式见下节。进程级测试在 POSIX 上用 Python 3 驱动独立子进程，验证持续存活、两个停止信号和参数拒绝；Python 不参与产品运行。

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

SQLite 从 v2 起保存审计和命令幂等记录，当前 v4 还保存 Action 路由和 Host 命名空间/幂等响应；模型与任务尚未接线。T02a 适配器桥和 T03a 插件桥已使用此入口；控制 API 仍须在后续任务接线。

## T02a 适配器接收应用入口

AdapterIngress 依赖 RuntimeStore 和宿主提供的 AdapterPolicy；负责注册账号权限、实例替换、输入校验、命令授权审计、事件去重与事务提交。BridgeServer 仅绑定认证身份并转换流消息，不从注册载荷生成权限。实例状态按连接生命周期管理，事件/幂等/水位持久化到 RuntimeStore。无持久化入口的 BridgeServer 继续显式拒绝业务，不发送假确认。订阅路由、插件执行及 Action 分发由后续任务实现。
