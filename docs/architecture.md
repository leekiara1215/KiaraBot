# KiaraBot 架构设计

> **文档定位**：本文是 KiaraBot **唯一**的架构设计文档，描述目标架构、分层边界、六服务拓扑、通信契约、数据所有权、身份授权、失败语义与部署拓扑。原人类冻结的目标架构基线已按维护者授权并入本文：**协议优先、Core 独立、适配器可替换、插件只走 SDK、面板只走 API、基础设施实现端口**等冻结事实保留于下文。
>
> **唯一来源**：跨进程/跨语言契约是 `proto/kiarabot/v1/`，浏览器 HTTP/WS 契约是 `api/openapi.yaml` 与 `api/asyncapi.yaml`；契约登记与状态门禁在 `proto/tests/target_spec.json`、`proto/tests/contract_status.json`。模块行为细节见 §12 的模块文档，工程规则见 `docs/spec/`。
>
> **实现状态**：本文描述“应该长什么样”，同时标注当前落地状态。截至 HEAD `07bef038`（2026-11-06），六服务目标栈、streaming、Host 管理、bootstrap 受控交付与 owner 操作变化查询已按真实 PostgreSQL/TLS 端到端链路验收（见 `tests/architecture/`）；未验收与延期项集中在 §11。契约声明、测试注册或 `core/reference/` 历史结果都不等于生产验收。

## 1. 目标与范围

KiaraBot 是面向 QQ 的可扩展机器人平台。核心思路是把平台接入、业务核心与控制面解耦，使 NoneBot2 适配器、插件语言和面板客户端可替换，而公共协议与业务规则不变。

**设计目标**

- **协议优先**：先定义自己的事件、Action、管理与插件协议，再实现各进程；跨语言只共享 `proto/` 与 `api/`，不共享实现代码。
- **Core 独立**：领域规则、事件编排、权限判断、插件调度属于 C++ Core 能力，不依赖 NoneBot2、NapCat、模型厂商 SDK 或浏览器。
- **适配器可替换**：平台转换留在适配器；未来的原生适配器、其他语言适配器复用同一 Bridge 协议接入，不修改 Core。
- **能力受控**：插件只能通过 PluginSDK 访问消息、模型、存储、任务等能力；面板只能通过 Control API 访问业务数据。
- **可恢复**：跨服务异步操作使用本地事务 + 持久交接与去重；外部副作用保留 UNKNOWN 语义，不盲目重试。

**范围**：六类平台服务的职责与依赖、进程/部署单元、契约、数据所有权、身份与委托、事件/投递/Action/operation 语义、插件与适配器模型、可观测性与部署拓扑。目标方向要求服务可跨主机运行，但当前交付与验收以单机 Compose 为主，跨主机部署与故障演练延期（见 §11）。

**非目标**（本阶段明确不做）：高可用、多副本、多写入者、自动故障切换；消息中间件、服务发现与通用 RPC 路由系统；自定义角色管理、插件市场、文件服务、消息历史与会话上下文编辑；完整内容审核平台；旧开发数据导入、双写或旧协议兼容层。

## 2. 分层与模块边界

系统按“协议 → 领域/应用/端口 → 运行时/传输 → 基础设施/表现”分层；仓库目录与依赖方向如下。

| 层 | 目录 | 职责 | 依赖约束 |
| --- | --- | --- | --- |
| 协议 | `proto/`、`api/` | 跨进程消息/服务、HTTP/WS 契约 | 契约叶子，不含实现 |
| 领域/应用/端口 | `core/include/kiarabot/core/{domain,application,ports}/target/`、`core/src/` | 业务规则、用例、本地事务、端口接口 | 只用内部模型；协议类型仅在边界转换 |
| 运行时/传输 | `core/src/runtime/platform/`、`core/app/*_main.cpp`、`control/api/app/main.cpp` | gRPC/TLS/Bridge、组合根、进程生命周期 | 依赖应用/端口与基础设施 |
| 基础设施 | `infrastructure/` | PostgreSQL schema/UoW、SQLite、加密、HTTP Provider | 只实现 Core 端口，不决定业务规则 |
| 适配器 | `adapters/nonebot/` | OneBot V11/NoneBot2 → 统一协议、平台执行日志 | 只经 `proto/` 接入 Runtime |
| 插件/SDK | `sdk/python/`、`plugins/` | 进程外插件与 PluginSDK | 只经 PluginBridge/SDK 接入 |
| 控制面 | `control/api/`、`panel/` | REST/WS 服务与浏览器客户端 | 面板只经 Control API |
| 运维 | `deploy/`、`scripts/` | 构建、启动、迁移、备份、健康检查 | 不绕过 API 改业务数据 |

允许的依赖方向（箭头表示“被依赖”）：

```text
proto             ← core / adapters / plugins / sdk
api               ← control/api / panel
core/ports/target ← infrastructure（只实现端口，组合根负责装配）
proto（目标协议）  ← control/api 的远端 owner 适配
control/api       ← panel
```

禁止的依赖：Core 导入 NoneBot2/NapCat/模型厂商 SDK/浏览器代码；插件访问 OneBot、数据库连接或 Core 内部类；面板访问 C++ 源、数据库或平台端；基础设施实现业务规则；适配器携带插件业务逻辑；在多个模块复制同一类型。依赖检查入口是 `scripts/quality/check-boundaries.py`，规则全文见 [开发边界与协作规则](spec/boundaries.md)。基础设施端口（消息总线、模型 Provider、会话/用户/权限存储、调度器、审计等）定义在 `core/include/kiarabot/core/ports/target/`，由 `infrastructure/` 提供可替换实现；文件存储目前无生产消费方，不据此扩建文件功能。

`core/` 是领域/应用/端口代码库，**不等于单一进程**：六个平台进程分别由 `core/app/{identity,runtime,scheduler,model,host}_main.cpp` 与 `control/api/app/main.cpp` 组合根装配。历史一体入口隔离在 `core/reference/`，只用于 reference 构建，不链接进目标服务。

## 3. 六服务与进程/部署单元

| 服务 | 可执行文件 | 权威职责 | 入站接口 |
| --- | --- | --- | --- |
| Control 控制管理 | `kiarabot-control` | 浏览器唯一业务入口：会话代理、类型化 owner 查询/命令路由、WS 状态流、健康聚合；不拥有业务权威数据 | HTTPS REST/WS（`control/api/src/target/https_endpoint.cpp`） |
| Identity 身份与访问 | `kiarabot-identity` | 人类/服务主体、会话、凭据、固定角色与精确资源授权、委托签发/交换/在线判定、失效流 | gRPC mTLS |
| Runtime 运行时 | `kiarabot-runtime` | 账号与适配器绑定、插件安装/实例、事件 inbox/投递账本、Action/配额、插件 namespace、资源生命周期与 Host admission gate | 内部 gRPC mTLS owner 监听 + 外部 Adapter/Plugin Bridge 监听 |
| Plugin Host 插件宿主 | `kiarabot-plugin-host` | 本机受管插件进程启停、执行日志、维护/排空、Host 管理面 | 主动连 Runtime 的 HostBridge（mTLS）；HostManagementService（owner mTLS，由 Control 调用） |
| Scheduler 任务调度 | `kiarabot-scheduler` | 任务定义/occurrence、到期领取、向 Runtime 可靠交接 TimerTriggered、取消 | gRPC mTLS |
| Model 模型网关 | `kiarabot-model` | 模型配置与加密 secret、Provider 调用、两次事务、streaming、调用记录 | gRPC mTLS |

外围进程/单元：面板静态站（`panel/`，只访问 Control）、Adapter 实例（可部署在平台连接所在主机）、插件业务实例（受管或外部自管）、PostgreSQL 16.4、一次性初始化任务（`kiarabot-pg-initialize`、`kiarabot-identity-bootstrap`）。Control 与 Panel 都不拥有业务数据权威；反代/SSH 隧道只是访问入口。面板管理面覆盖机器人账号与适配器、插件启停与配置、模型与密钥、群白名单、任务、日志/审计与运行状态，全部受范围授权与敏感权限约束。

进程与数据规则：每个权威域同一时刻只有一个活动写入进程（数据库内排他进程锁，第二个进程不得 ready）；服务之间不共享进程内指针、锁或事务对象。拆分不改变业务归属——Identity/Runtime/Scheduler 仍是 Core 业务能力，Host 是 Core 运行支撑，Model 的 Provider 实现属基础设施，进程独立不等于把业务规则交给基础设施。首版基线的“单 Core + 适配器 + 面板”进程划分已由六服务目标架构取代（控制管理独立进程，Identity/Runtime/Scheduler/Model/Host 各自进程），但协议优先、Core 独立、适配器可替换等原则不变。六进程构建目标见 `deploy/docker/suite.Dockerfile`。

## 4. 通信与契约

| 契约 | 唯一来源 | 内容 |
| --- | --- | --- |
| 跨进程 gRPC | `proto/kiarabot/v1/*.proto` | 六服务消息、枚举、错误码、封闭权限与 deadline options；`proto/tests/target_spec.json` 登记 123 个类型化 RPC 的调用方、权限、幂等、期限与暴露面 |
| 控制面 REST | `api/openapi.yaml` | `/api/v1` 管理/查询操作、认证、分页、错误结构、`/health/live` 与 `/health/ready` |
| 控制面 WebSocket | `api/asyncapi.yaml` | 8 个 `/ws/*` 频道：首帧认证、`SESSION_READY`、游标重放、`RESYNC_REQUIRED`、关闭码 4400/4401/4403/1011/1013/1001 |

**服务间 RPC**：查询与短命令使用带 deadline 的 unary gRPC，写命令携带幂等键；长生命周期交互使用流——`AdapterBridge.Connect`、`PluginBridge.Connect`、`HostBridge.Connect` 三条双向流，以及 `IdentityService.WatchInvalidations`、`ModelService.ChatStream` 等服务端流。身份来自 mTLS 通道或受限证明，请求正文中的 actor/role/owner 不可信。Owner 监听（mTLS）与外部 Bridge 监听（server-verified TLS + bootstrap/连接授权）是两个不同的暴露面，不能互相替代。

**可靠异步**：首轮不部署消息中间件。发送方在本地业务事务内同时写业务状态、必要审计/收据与 outbox，派发器在短事务中领取记录后在事务外调用网络；接收方在自己的本地事务内写 inbox 去重键、业务变更与结果后再确认持久接受，确认丢失可同 message_id 重发。跨服务查询返回所有者快照或带版本/时效的投影，不承诺跨服务一致快照。

**公共契约类型**（`proto/kiarabot/v1/platform.proto`）：`RequestContext`（request_id/trace/deadline/幂等键）、`ResourceRef`（类型 + 稳定 ID）、`Fence`（实例/执行代次/连接代次/Runtime epoch）、`Operation`（所有者/资源/版本/状态/执行结果/阶段）、`FailureContext`（是否受理/恢复动作/operation 引用/retry_after）、`StreamCursor`（owner/epoch/scope/sequence）。`ErrorCode` 是跨语言稳定错误词表，自由文本不能替代稳定码。

**兼容与状态门禁**：`kiarabot.v1` 只做向后兼容新增，删除先 deprecated 后 reserved、编号永不复用；descriptor 兼容性、枚举基线、实现状态、语义规则与 API 解析分别由 `proto/tests/check_contracts.sh`、`check_enum_consistency.py`、`check_implementation_status.py`、`check_semantic_rules.py` 校验。未实现字段/方法在 proto 内联 `CONTRACT_STATUS` 与 `api/*.yaml` 的 `x-contract-status` 中显式登记，文档不得暗示其可用。

## 5. 数据所有权与存储

| 所有者 | 存储 | 权威数据 |
| --- | --- | --- |
| Identity | 私有 PostgreSQL 逻辑库 | 主体、凭据版本、会话、固定角色分配、资源授权、授权版本、失效记录、审计/收据 |
| Runtime | 私有 PostgreSQL 逻辑库 | 账号/适配器绑定、插件安装/实例/期望配置、事件 inbox/水位/投递目标/尝试、Action/幂等/配额/日预算、namespace 与收据、host_commands/observations、生命周期/operation/tombstone |
| Scheduler | 私有 PostgreSQL 逻辑库 | 任务、occurrence、触发 outbox/收据、容量计数 |
| Model | 私有 PostgreSQL 逻辑库 | model 配置、加密 secret、调用意图/结果（含 UNKNOWN/计费事实）、管理 operation |
| Plugin Host | 本地 SQLite（每主机/实例隔离） | command journal、受管执行、执行观察；进程事实的唯一本地权威 |
| Adapter | 本地 SQLite | 待确认事件 outbox、平台序号、Action 执行日志、平台 ID 映射 |
| Control | 无权威业务库（必要时自有可重建技术状态） | UI 聚合、流游标、可重建缓存，不重建业务权威 |
| 插件业务数据 | Runtime 托管 namespace | 默认实例私有；安装内共享 namespace 由获授权管理者显式开启，插件经 SDK 访问 |

规则：**单一所有者、单一本地 Unit of Work**。PG 默认 READ COMMITTED，用业务唯一约束、固定顺序行锁与 `expected_version` 条件更新保证一致性，不依赖提升隔离级别“自动修复”竞态。事务内不调用网络/插件/供应商：高风险管理在事务外在线授权，再在短事务内校验 gate/fence/版本/额度。禁止跨库外键、视图、联表、共享表与 2PC；跨库只保存类型化稳定引用；服务运行账户与显式迁移身份分离，常驻进程无 DDL 权限，启动只校验 schema；初始化是空库 + 每服务显式命令，不自动迁移、不导入旧快照。schema/descriptor 唯一来源是 `infrastructure/postgres/schema/v1.hpp` 与 `infrastructure/postgres/schema/manifest.hpp`。幂等、投递恢复、审计与未决 Action 等未决义务不清理，清理失败不撤销已生效 gate。Host/Adapter 的 SQLite 是本地可靠执行/发送日志，不是第二套中央业务权威，也不因中央库存在就取消平台副作用去重。

资源身份分层（`proto/kiarabot/v1/platform.proto`）：`plugin_type/plugin_installation/plugin_instance` 是稳定逻辑身份，`execution_generation`、`connection_generation`、`runtime_epoch`、投递 `attempt` 与配置版本各自独立、不能互相替代；资源版本由所有者单调递增并用于条件更新，资源 ID 使用 UUIDv4 且永不复用，展示名/地址/凭据不能替代资源身份。

## 6. 身份、授权与委托

| 主体/通道 | 机制 | 关键约束 |
| --- | --- | --- |
| 人类用户 | Identity 会话：Login/Refresh/Validate/Revoke；浏览器 access 15 分钟、refresh 30 天 | Control 每次 REST 向 Identity 校验当前权威状态，失败不放行；WS 距最后确认超 750ms（1 秒预算内）关闭；撤销后 REST 立即拒绝 |
| 角色与范围 | 固定角色 `admin/operator/viewer` × 精确资源授予，取交集；敏感权限（如 `message.read`、`action.inspect`、`log.read`、`audit.read`、`health.read`）必须显式授予 | 空 scope 不代表全局，不因角色名自动通配；权限词表是闭集（`proto/kiarabot/v1/policy.proto`，Permission 1–50） |
| 平台服务身份 | 生产服务间 mTLS，证书身份绑定登记服务，不用 hostname、IP 或请求字段确定身份 | 工作负载证书 24 小时有效、12 小时触发续签；无受信证书不 ready |
| 内部委托 | Identity 签发 Ed25519 JWT，绑定调用服务、主体、audience、操作/资源子集与父委托链 | 有效预算 ≤60 秒（exp-iat ≤56s + 节点时钟差/验签容差）；只能收窄；最多两次业务转交（Control→Runtime→Model/Host）；高风险操作逐次 `CheckAuthorization` 在线核验（默认 500ms/最大 1s） |
| 平台 peer | Runtime/Scheduler/Model 的 clientAuth 证书由离线 `identity-bootstrap` 登记为 platform principal 并授予显式 assignment；叶证书 URI SAN 约定 `spiffe://kiarabot/<service>` | Scheduler→Runtime 的 `AcceptTimerTrigger` 只接受平台 peer + handoff 完整性校验，不消费用户 delegation，该豁免不扩散到其它 Runtime 方法 |
| 外部 Adapter/Plugin | server-verified TLS + 逐实例可撤销 bootstrap 凭据登记，换取 `ConnectionAuthorization`/`Fence`/`connection_proof`（CSPRNG ≥256 位，绑定实际 TLS 流、主体、fence、Runtime epoch 与凭据版本） | 每帧携带当前 fence + proof；≤56 秒、到期前 10 秒单飞续期；refresh 失败停止新副作用；不开放匿名注册 |
| Host | 部署签发 mTLS 客户端证书，SAN/指纹映射到预注册 `host_id`；Host 主动连 Runtime 的 HostBridge | 新连接替换旧连接，旧 connection_generation 拒绝；心跳 5s、15s 判疑似离线（不等于进程已停） |
| Host gate | `RuntimeService.SetHostAdmissionGate` 前必须经 `IdentityService.CheckHostGateAuthorization`：请求携带 Runtime 实际入站 Host 叶证书 SHA-256 + 单 `RESOURCE_HOST` scope；Identity 校验 Active Host principal、当前证书与 `host.execute` 精确授权 | allow 不缓存、不覆盖撤销；Runtime 还要重查 TLS allowlist、当前 HostBridge fence 与 maintenance intent/version；未登记/撤销返回 UNAUTHENTICATED，缺 grant 返回 PERMISSION_DENIED |
| 引导与秘密 | 初始可信管理员与平台 peer 由离线 `kiarabot-identity-bootstrap` 显式安装；bootstrap 明文只在一次响应或受控 0600 文件中出现，Runtime 只存 SHA-256；CA 私钥不入 Identity 库 | 秘密不进数据库明文、日志、outbox、argv 或面板；受控交付由 Control 唯一写入固定离线目录，Host/Adapter 只读消费 |

失效传播：Identity 的失效流是**有界加速器**（最近 10 分钟/4096 条 + 快照补齐），不替代高风险在线检查、浏览器 REST 当前会话校验或 WS 1 秒检查。撤权与外部副作用不是全局原子，已发生的平台调用不能通过撤权撤回。

## 7. 事件、投递、操作与失败语义

**入站事件**：所有平台消息先转换为统一 `EventEnvelope`（event_id/schema_version/occurred_at/trace_id/source/subject/sequence/delivery_mode + payload oneof：MessageReceived、MessageSent、GroupMemberChanged、FriendRequest、TimerTriggered、AdapterStatusChanged、PluginStatusChanged；部分载荷当前无生产者，可用性以 `proto/tests/contract_status.json` 为准），平台差异只在适配器内消化。Adapter 先在本地 SQLite 持久事件与平台序号，再调用 Runtime `PublishEvent`。Runtime 校验认证来源、账号绑定、连接代次、事件类型与大小（拒绝伪造 `TimerTriggered`）；同一 `(adapter,event_id)` 同内容为幂等重复、异内容 CONFLICT；按会话四元组 `(adapter, account, conversation, type)` 维护 sequence 水位，缺号标 degraded；在目标快照事务内写入事件、水位、每实例投递目标、审计与队列计数。没有目标也保存 inbox，不因未来实例注册补投；有序投递按“实例 + 会话四元组”，不同实例互不阻塞。

**投递尝试**：`delivery_targets` 记录 persisted/retrying/dispatched/acknowledged/dead_letter，`delivery_attempts` 记录完整 fence、租约与尝试号。ACK/续租必须匹配当前 `event+instance+attempt+fence`：旧尝试的迟到 ACK 不能确认新尝试；续租只延长当前有效尝试，单次至多 30 秒、自初次领取总计至多 300 秒；重试为首试之外最多 5 次（1/2/4/8/16s ±20% jitter），不可重试错误直接死信；重放只重置目标实例并保留 `event_id`、新 trace 与新 attempt。

**Action 与平台执行**：插件提交 Action 后，Runtime 在本地 UoW 内原子写入 Action、幂等键、配额窗口与 UTC 日预算。危险动作（Mute/Kick）必须显式启用、命中群白名单且预算为正，并与权限、配额共同校验，任一必需配置缺失即拒绝，不产生平台副作用。派发前再在线核验权限/账号 gate/白名单，并以条件更新记录 dispatch 身份后才写出站帧；Adapter 先持久 `action_id + request hash` 再调用平台。调用中断/超时/进程崩溃时结果保持 UNKNOWN、不自动重发；迟到结果在 EXPIRED/CANCELLED 后只追加证据、不覆盖终态；取消只在派发前有效，已派发返回冲突，不谎称撤回。

**Operation 与终态**：统一状态 `Accepted/Running/Blocked/Succeeded/Failed/Cancelled/Expired`；`Blocked` 是可恢复非终态并带稳定原因与下一次处理信息；执行结果未知不伪造 `Failed`，由独立的 `execution_outcome`（KnownSucceeded/KnownFailed/UNKNOWN/NotStarted）表达。受理、持久接受与业务完成分离，失败体保留稳定 `ErrorCode`、`request_id`、`trace_id` 与 `FailureContext`。owner 操作变化查询（Runtime/Scheduler/Model/Host）以独立 dataset epoch、提交序 head 与低水位游标发布，保留 600 秒/最多 65536 条，跨代或超窗要求 resync；WS 通知不是完成证明，REST 快照仍是权威恢复源。

**幂等与重试**：内部成功命令收据至少保留 7 天，容量不足时拒绝新命令而不淘汰有效记录；同主体+方法+资源 scope 下同键同内容重放原结果（重放前重新鉴权），异内容 CONFLICT；浏览器控制面收据 24 小时、Host 收据 7 天；清理/撤销等安全记录不因普通 outbox 满而丢失。outbox 退避 0.5s 指数到 30s（±20% jitter），单层派发重试、不嵌套 SDK/gRPC 自动重发，发送 100 次进入需处置状态但不丢记录；数据库死锁只在纯本地事务重试（最多 3 次）；可能有副作用且结果未知的调用不套用通用自动重发，按原身份查询恢复。

## 8. 插件与 Adapter 模型

**三类扩展边界**：①渠道适配器（`adapters/`）把平台协议转换为统一事件/Action，拥有平台连接状态、原始 ID 映射与本地执行去重；②进程外插件（`plugins/` + `sdk/python/`）通过 PluginBridge 与 SDK 访问能力，是首期唯一插件形态；③进程内 C ABI 插件属延期方向——基线明确不把 C++ 类作为插件 ABI。

**插件身份与生命周期**：`plugin_type_id`（种类/包）、`plugin_installation_id`（逻辑安装与配置边界）、`plugin_instance_id`（获准独立运行的逻辑实例）、`execution_generation`（一次进程执行）分别标识、互不替代。插件清单（`api_version=kiarabot.plugin.v1`、`runtime=python-rpc`、`isolation=process`、capabilities、permissions、config schema）由 Runtime 校验；权限授予仍由 Identity 的精确 assignment 决定，清单自报不构成授权。SDK 向插件暴露 `PluginContext` 及消息、模型、存储、任务、配置能力，权限判断在服务端完成、SDK 不缓存授权决定。插件注册时协商能力（`host.storage.v1`、`host.model.v1`、`host.model.stream.v1`、`host.tasks.v1`、`plugin.config.v1`、`action.result.v1`），未协商不下发对应能力。

**受管与外部自管**：受管实例由 Host 在所在主机受控启动/停止：Host 先持久 command journal（command_id/目标代次/payload hash）再 spawn，启动参数仅来自已登记包摘要与允许参数，最小环境；使用执行 marker（PID + 启动标识）防止 PID 复用误判，崩溃恢复先认领/比对 marker，无法确定时阻止重复 spawn 并报告 UNKNOWN。`SetDesiredState/UpdateConfig` 的持久版本、Host 进程启动、插件协议注册、配置应用 ACK 是不同事实，分别有来源与版本，不能用一个 `running` 混合表示。外部自管实例可按获准身份直接注册 Runtime，不伪装为宿主管理。跨主机迁移必须在确认旧执行已停或资源级隔离后才激活新代次，网络超时不作为强制接管依据。所有 Bridge 帧都携带 `Fence` 与当前 proof；旧执行/连接代次与旧 attempt 的回执只能返回过期事实，不能修改新代次状态（fencing）。

**Adapter 模型**（`adapters/nonebot/`）：NoneBot2 + OneBot V11 正向 WS 连接 NapCat；事件映射为 `EventEnvelope`，Action 执行前写本地 action journal，平台 retcode 映射为稳定 `ErrorCode`（限频/权限/不存在/上游故障），断线后按原事件 ID 重发 outbox 并在新连接代次重新注册。适配器的实现类型/版本/能力来自已认证注册信息，服务端校验实例身份与账号绑定，不能根据实例 ID/名称推测实现类型，也不能把能力声明当权限授予。未来原生适配器复用同一 `AdapterBridge`，无需修改 Core。

## 9. 模型调用：两次事务与 streaming

调用链：插件 → PluginBridge `HostRequest.model_chat` / `model_stream_start` → Runtime（准入、委托与在线授权）→ Model `Chat`/`ChatStream` → HTTP Provider。Control 的管理面调用不携带插件正文，也不读取 Provider 密钥。模型调用分两次本地事务：

1. **txn1（调用前）**：在线授权通过后解析 model/配置与 secret 版本、做 per-model 并发 admission、以服务端 `request_hash` 防重（owner + `logical_call_id` 唯一），写调用意图 `STARTED`、审计与 `may_have_billed`（在途保守为 true）后提交，之后才调用 Provider。
2. **txn2（调用后）**：已知成功写 SUCCEEDED，明确 4xx/429 写 FAILED；5xx、TLS、超时、畸形或超大响应写 `Blocked + execution_outcome=UNKNOWN + may_have_billed=true`，不得写成“未调用/未计费”。COMMIT 结果未知不回滚 txn1，也不能据此重跑外部调用。

同 `logical_call_id` 重到只返回元数据/查询引用，不重调 Provider，也不持久重放正文；UNKNOWN 在 30 天内只能经 `ResolveModelCall`（expected_version CAS、幂等）转终态，处置后保留至少 7 天防重窗口，未决 UNKNOWN 不清理。配置/密钥轮换只影响后续调用，在途使用固定配置与 secret 版本；Provider 密钥以 XChaCha20-Poly1305 AEAD 加密，根密钥由部署注入且不入库；请求正文与密钥不写普通日志。Provider 只允许已批准的 HTTP 实现（libcurl），不引入厂商 SDK；文本/消息数与响应上限沿用 64KiB/128 条/1MiB。

**streaming**（`proto/kiarabot/v1/model_stream.proto`）：`ModelService.ChatStream` 返回服务端流；插件侧通过 `host.model.stream.v1` 与 `model_stream_start/cancel` 帧消费。事件序 `started=0 → delta=1..N → terminal=N+1`，严格解码器校验序号、UTF-8、累计上限与 replay（replay 零 delta）；重复 logical id 在 Chat/ChatStream 间共享防重，异内容 CONFLICT。在线授权在准入、首输出前、最长每 1 秒及终帧前复查；撤权/到期/连接关闭停止输出并尽力取消 Provider，但已发文本保持 partial、可能已计费。背压为每流 32 帧/64KiB、每连接 4 流/每进程 64 流；超时/取消/丢帧结果未知时保持 UNKNOWN，不伪造 FAILED。该能力已在真实受管插件整链与 SDK 上验收。

## 10. 可观测性、运维与部署拓扑

**健康检查**：每进程提供匿名 `/health/live`（不访问存储）与 `/health/ready`（可接收请求）；Control 另有 `/api/v1/health`（需 `health.read`）按组件返回 `identity/runtime/scheduler/model`。语义：Identity/凭据链/撤销链不可证明 → 整体 UNHEALTHY（503，ready=false）；Runtime/Scheduler/Model 单点故障 → DEGRADED（200，对应功能返回 `UPSTREAM_UNAVAILABLE`），不整体摘流；Control 自身存储/进程故障 → 未就绪。告警不得用本地时钟重算组件 ready。

**可观测性**：结构化 JSON 日志（timestamp/level/component/operation/outcome/request_id/trace_id/event_id/action_id/resource_id/error_code）只记录标识和状态，不记录 token、密钥、消息正文或未脱敏私聊。审计与日志是不同权限（`audit.read`/`log.read`）的敏感读：审计是业务事实、日志是脱敏投影，各自保留。WS 提供 health/logs/tasks/messages 等授权状态流；owner 操作变化来自持久 journal，不依赖易失广播。保留默认：审计 ≥180 天、运维日志 7 天；清理失败只告警且不撤销业务 gate。

**部署拓扑**：默认单主机 Docker Compose（`deploy/compose.target.yml` = `compose.infra.yml` + `compose.core.yml` + `compose.exec.yml`，Docker Compose ≥2.24）。PostgreSQL 16.4 单实例承载四个私有逻辑库（独立角色/权限/迁移身份）；Identity/Runtime/Scheduler/Model/Control/Panel 与 Plugin Host/Adapter 各自独立容器，非 root、只读根文件系统、独立 TLS/凭据/配置/卷；所有发布端口只绑定 `127.0.0.1`。初始化必须显式执行：`--profile init` 的四个 `init-*`（空库建表）+ `identity-bootstrap`（管理员、平台 peer、证书引导）；daemon 不执行 DDL、不自动迁移、不清库。启动顺序为数据库/证书/信任根 → Identity → Runtime/Scheduler/Model → Host/Adapter → 插件 → Control 业务就绪。备份按服务库协调 + Host/Adapter 卷；恢复需停止新副作用、协调备份点、轮换运行 epoch 并校验收据/未决动作。首期跨主机部署（两主机拓扑、断网/重连演练）已延期并需单独记录；单机 Compose 不构成高可用。`core/reference/` + SQLite + `scripts/local/start.py` 是独立的本机 MVP/reference 轨，不是目标六服务部署，不能当作新架构验收。

## 11. 关键设计取舍、已知限制与残余项

**关键取舍**

- **协议优先于现有框架**：先定 `proto/`/`api/` 再实现，避免把 NoneBot2 或单进程实现细节固化为业务核心。
- **六进程而非单核**：按业务能力、状态所有权与故障隔离拆进程，但保持每权威域一个活动写入者，不引入分布式事务。
- **本地事务 + outbox/inbox 而非消息中间件**：可靠交接由业务库内事务与接收方持久去重保证，首轮不部署 broker。
- **每服务私有 PG + 本地 UoW，而非共享表/跨库联表**：通过类型化引用与 owner RPC 组合；Host/Adapter 保留本地 SQLite 事实。
- **进程外插件而非 C ABI**：崩溃隔离、独立升级、多语言与权限限制优先于极限性能；C ABI 属延期方向。
- **开发阶段统一目标契约、空库初始化、无历史兼容层**：不为过渡系统承担双写/桥接成本。

**已知限制与残余项**

| 领域 | 限制/残余 | 状态 |
| --- | --- | --- |
| 部署 | 单机 Compose 为主；两主机与跨主机断网/重连未演练；无 HA/多副本/服务发现，升级为计划内停机窗口 | 延期 |
| 协议面 | `/ws/control` 聚合频道仍 `not_implemented`（7 个专用 target 频道已实现）；bootstrap 交付无 cancel 入口（冻结契约缺口，需契约变更） | 已登记 |
| 功能范围 | 自定义角色、插件市场、文件服务、消息历史/会话上下文编辑不在范围；最小内容策略与平台异常闭环（D06）尚未完整闭环 | 后续排期 |
| 平台验收 | 真实 QQ 仅完成纯文本群聊首轮验收（私聊与真实平台表情/撤回等未全覆盖） | 待环境 |
| 模型 | 真实外部 Provider/计费未验收（当前以本地 HTTP SSE 验证）；无工具/多模态；未决 UNKNOWN 需人工/规则处置 | 待环境 |
| 扩展 | C ABI 进程内插件、C++ 原生适配器、C++/TypeScript SDK 与 C++ 面板客户端未实现 | 延期 |
| 规模/运维 | 大目录 WS 轮询有运营规模限制（建议单目录 ≤25–30 行、授权 child 上限 126）；真实大目录逐 child 在线授权受 500ms 预算约束 | 已记录 |
| 迁移 | 不导入旧开发数据、不做双写；首次正式发布后才建立发布兼容窗口 | 明确非目标 |

**残余风险**：外部平台副作用没有 exactly-once 保证，attempt/租约/幂等只保证不重复确认与不盲目重发；撤权是有界收敛而非全局原子；UNKNOWN 可能长期存在直到查询或人工处置；数据库 COMMIT 结果未知时只能按原身份查询恢复。这些语义必须在模块文档与验收矩阵中逐项验证，不能由本文宣称已满足。

## 12. 模块文档索引

| 模块 | 文档 | 主要内容 |
| --- | --- | --- |
| Identity | [identity.md](modules/identity.md) | 会话/凭据、固定角色与资源授权、委托、失效流、审计 |
| Core Runtime | [core-runtime.md](modules/core-runtime.md) | 事件与投递、Action、namespace、资源生命周期、Host gate |
| Model | [model.md](modules/model.md) | 模型配置/密钥、两次事务、UNKNOWN、Provider、streaming |
| Scheduler | [scheduler.md](modules/scheduler.md) | 任务/occurrence、可靠触发交接、取消语义 |
| Plugin Host | [host.md](modules/host.md) | 受管插件进程、HostBridge、维护/排空、SQLite journal、bootstrap |
| Control API | [control-api.md](modules/control-api.md) | HTTPS REST、WS 频道、owner gateway、错误映射、限流分页 |
| Control Panel | [control-panel.md](modules/control-panel.md) | React/Vite 面板、API 客户端、scope 订阅与恢复 |
| Plugin SDK | [plugin-sdk.md](modules/plugin-sdk.md) | Python SDK、PluginBridge、ACK/租约、storage/task/model streaming |
| NoneBot Adapter | [nonebot-adapter.md](modules/nonebot-adapter.md) | OneBot V11 映射、Action 执行、fence/重连、平台错误 |
| Infrastructure | [infrastructure.md](modules/infrastructure.md) | PG schema/UoW/初始化、SQLite、TLS/加密、迁移与语义 |

工程规范见 [开发边界](spec/boundaries.md)、[工具链](spec/toolchain.md)、[C++ 编码规范](spec/coding-standard.md)、[CI 与门禁](spec/ci-gates.md)、[任务与文档同步流程](spec/task-workflow.md)。契约与 API 的唯一来源是 `proto/` 与 `api/`；模块文档与本文冲突时，以契约为准并向维护者报告。
