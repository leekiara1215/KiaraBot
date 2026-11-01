# KiaraBot 服务通信与方法设计

- 版本：v1.0（设计定稿，未实现）
- 日期：2026-11-01
- 决策来源：用户已确认 DD01、DD02.1–DD02.6、DD03.1–DD03.4；用户授权由助手决定后续详细设计
- 依赖：[方向基线](19-architecture-direction-baseline.md)、[详细设计总览](20-detailed-architecture-design.md)、[事务与迁移设计](22-transactions-and-migration-design.md)
- 边界：开发阶段按全新目标架构重建；本文不是已发布 proto/OpenAPI，本轮不实际修改契约或实现

## 1. 定稿决策与理由

| 编号 | 决策 | 主要理由 |
| --- | --- | --- |
| DD03.5 | 采用总览中的 Identity 方案 B，基础身份链路形成闭环 | 只搬登录不能支撑已确认委托及资源授权；不建设自定义角色或完整用户管理 |
| DD03.6 | Runtime 提供类型化资源命令、查询、注册流、投递与 Host 入口 | 保留事件、投递、Action/预算和 namespace 的本地一致性 |
| DD03.7 | Host、Scheduler、Model 各有独立方法；Control 仅路由与聚合 | 避免 Control 复制权威状态或 Runtime 接管其他服务数据 |
| DD03.8 | 设置有限 deadline、单层重试、持久交接、租约及版本恢复默认值 | 给出能验收的边界，避免重试放大、无限续租和断线丢工作 |
| DD03.9 | 全组件采用单一目标协议，无旧版兼容层和双写 | 用户明确开发阶段可无历史包袱，不引入过渡系统 |

选择未新增业务服务、broker 或多副本自动切换。生产首期每个权威域一个活动写入进程；多个账号/插件实例及多台 Host 不等于同一 Runtime 同时有多个活动写入者。扩容需未来另行评审，当前用行锁/唯一约束仍验证线程并发。

## 2. 逻辑拓扑与调用权限

| 直接调用方 | 接收方 | 允许用途 | 禁止替代 |
| --- | --- | --- | --- |
| Browser | Control | HTTPS/WSS 管理和状态 | 不访问业务数据库、Host 或供应商 |
| Control | Identity/Runtime/Scheduler/Model | 代表当前用户的受限管理/查询 | Control 服务身份不替代最终用户授权 |
| Runtime | Identity | 验证/委托/当前授权、失效订阅 | 不读 Identity 数据库 |
| Runtime | Host | 有版本的期望运行状态及执行命令 | 不传任意 shell 或远程数据库事务 |
| Host | Runtime | 登记、拉取/订阅期望、执行回执、实际观察 | 不替插件 ACK，不授予业务权限 |
| Runtime | Scheduler | 代表插件的任务 create/get/cancel | 不把 Runtime 设成任务第二写入者 |
| Scheduler | Runtime | 持久 TimerTriggered 交接 | 不直接调用插件/平台 |
| Runtime | Model | 受限插件模型调用 | 不转发插件自报权限或供应商密钥 |
| Adapter/Plugin | Runtime | 专用认证注册及协议流/SDK | 不直连平台数据库/Identity 管理 API |
| 各平台服务 | Identity | 仅获准的身份操作、授权及验签材料 | 默认无主体管理或全量授权读取能力 |

所有者提供操作查询；Control 聚合时携带来源、版本、观测时间和部分失败。跨服务查询默认不承诺一致快照。直接管理 Scheduler/Model 仍须验证资源归属及委托，不能只依赖入口 Control 已检查。

## 3. 方法约定

下表方法名为目标逻辑名，不是当前已存在的 RPC；线协议落地时仍需逐项消息定义、稳定权限词条和 descriptor/OpenAPI 校验。

所有写入使用 DD03.4 上下文及服务端 fingerprint。查询默认 20/最多 100 项；用户分页沿用现行十分钟不可变快照规则，绑定主体/权限/资源/页大小。长连接分页恢复使用单独游标，不能混用。

固定角色仍为 admin/operator/viewer；有效权限为固定角色能力与明确资源范围的交集，实例共享需额外授权。没有授予则拒绝；未知方法按高风险在线检查。表中的“查看/管理”是权限意图，不宣称已有字符串枚举。

### 3.1 Identity（DD03.5）

| 方法族 | 主要输入 | 类型化输出 | 权限及提交 |
| --- | --- | --- | --- |
| Login/RefreshSession | 登录/刷新凭据、幂等键 | token 对、有效期 | 初始登录限频；刷新验证当前轮次；会话/收据/审计本地提交，秘密收据加密 |
| ValidateSession/GetPrincipal | 当前会话凭据 | 当前主体/会话/固定角色和范围 | 当前权威状态；不可用拒绝，不以缓存延续浏览器会话 |
| ListSessions/RevokeSession | 分页/目标 session | 列表/撤销结果 | 本人或既有管理员范围，隐藏与不存在遵守既有 NOT_FOUND 规则 |
| RegisterPrincipal/GetPrincipalStatus | 类型、资源绑定、登记版本 | 主体/状态引用 | 明确身份管理权限；首次部署引导不开放匿名登记 |
| RotateCredential/RevokeCredential | 凭据引用、预期版本、幂等键 | 更新版本、交付回执 | 不返回已有秘密；新秘密一次性交付，响应丢失按加密收据恢复 |
| IssueDelegation/ExchangeDelegation | 主体证明、目标、范围、父委托 | 至多 60 秒的受限证明 | 验证调用服务与主体；只能收窄，不任意跨 audience 转发 |
| CheckAuthorization | 主体、方法、资源、委托 | 允许/拒绝、授权版本、检查时间 | 面向当前权威；不返回秘密，不使用复制滞后伪装最新 |
| ListAssignments/SetAssignment/RevokeAssignment | 固定角色或已登记权限、资源、版本 | 授予结果和版本 | 仅身份管理授权；事务含审计/收据/失效 outbox |
| WatchInvalidations/GetAuthorizationSnapshot | 范围、版本游标 | 顺序失效/范围快照 | 仅订阅获准范围；游标过期全量补齐，禁止永久离线缓存 |
| GetVerificationKeys | 可信服务上下文、键版本 | 公钥/版本 | 已有信任链访问；不能从 token 指定 URL 下载任意验签键 |

浏览器 access 15 分钟、refresh 30 天及原轮换语义不变。内部证明不是浏览器 token 的替代。Identity 初始可信管理员由显式离线部署引导，不能通过“向自己申请管理员”解决启动循环；引导材料安装后禁用。

### 3.2 Runtime（DD03.6）

| 方法族 | 输入与约束 | 结果及恢复 | 风险 |
| --- | --- | --- | --- |
| Create/Update/DisableAccount、Bind/UnbindAdapter | 平台引用、稳定账号、预期版本 | 本地配置版本及必要异步操作引用 | 高 |
| RegisterAdapter/Heartbeat | 认证主体、账号子集、元数据/能力 | 当前连接代次、有效能力；旧连接失效 | 高；能力不授予权限 |
| PublishEvent | 当前连接、原 event_id/源/sequence/载荷 | 本地提交后 PERSISTED，重复不再分配目标 | 高；去重恢复受版本与保留约束 |
| ValidateManifest、CreateInstallation、CreateInstance | 包声明、受管范围和部署目标 | 安装/实例 ID，期望配置 | 校验可纯读；新建/管理高 |
| SetDesiredState、UpdateConfig | 实例/安装、预期版本、目标状态/完整配置 | 持久版本+operation；实际进程/注册/应用状态分别查询 | 高 |
| RegisterPlugin、HeartbeatPlugin | 主体、安装/实例、执行代次、特性 | 有效连接/能力及配置/投递入口 | 高，不接受自报 owner |
| Claim/DeliverEvent、AckDelivery、RenewDelivery | 实例+事件+attempt+连接/执行代次 | 当前租约/明确过期或重复结果 | 每次受权；旧代次不得改变新尝试 |
| Submit/Query/CancelAction | 认证插件实例、Action、幂等/原引用 | 原请求所有者范围内结果，取消仅未派发 | 高；提交和派发前检查策略 |
| List/InspectActionMetadata | 明确账号范围及查询授权 | 元数据/结果，不返回消息正文或秘密 | 敏感读；显式资源授权，插件查询仍限制自身 owner |
| Get/Set/DeleteStorage、CompareAndSetStorage | 服务端解析的 namespace、key/value、expected_version | 值/新版本/冲突；同键重放不延长 TTL | 默认敏感；共享成员和当前权限检查 |
| SetSharedMembers/DisableSharedNamespace | 安装、成员读写范围、预期版本 | 版本+失效进度 | 高，与 Identity 授予共同约束 |
| AcceptTimerTrigger | Scheduler 主体、稳定 occurrence_id、owner 实例、调度版本 | Runtime 已提交接收/重复/明确拒绝 | 高；不接受 Adapter 伪造 TimerTriggered |
| GetResourceState/GetOperation、ListDeadLetters/ReplayDelivery | 资源/操作/投递目标及范围 | 版本化查询；重放 operation | 重放高；敏感读在线；普通脱敏状态才可缓存 |
| BeginDelete/RestoreResource | 版本和目标 | 删除阶段与参与者进度 | 高，不回收 ID、恢复不复活凭据 |

名称变化不自动批准插件市场、依赖解析、native/wasm、新平台 payload。只实现已批准的纯文本等当前 Action 子集；ScheduleTask 仍拒绝，调度仅走 Host 任务能力。

同安装多实例首期采用 fan-out：接收时每个获准、已注册、配置有效的实例各有投递；不是负载均衡副本。ordered 以实例+既有会话四元组排序。共享业务去重需要 CAS，不能假定多个实例只收到一次。后续竞争消费模式不在本轮实现范围。

### 3.3 Plugin Host（DD03.7）

| 方法族 | 输入 | 输出及限制 |
| --- | --- | --- |
| RegisterHost/ReportInventory | 证书主体、host_id、受控包摘要及能力 | Host 版本/心跳；不接受任意环境或路径 |
| ApplyDesiredProcess | command_id、实例、执行代次、期望版本、包摘要、允许参数 | 本地持久接收及执行状态，重复命令不重复启动 |
| GetExecution/WatchExecutions | 获准实例/版本游标 | 带执行代次的实际观察；断线不代表已停止 |
| StopExecution/DrainHost | 指定实例/执行代次、宽限期限 | 先停止新工作再停止进程；超时报告未知或未完成，不能假成功 |

Host 主动建立与 Runtime 的受控长连接并恢复命令，避免 Runtime 直接执行远程 shell；命令/报告的逻辑方法可映射到该流。部署包仅允许管理员预置的目录/摘要，首轮无自动下载执行。每实例同主机进程组互斥；跨主机迁移必须确认旧执行已停或资源级隔离完成后再激活新代次，网络超时不作为强制接管依据。

### 3.4 Scheduler（DD03.7）

| 方法族 | 输入 | 结果/权限 |
| --- | --- | --- |
| CreateTask | owner 实例由认证上下文派生，task_id、UTC 或 IANA 当地时间、目标、payload、幂等键 | 本地任务+收据/审计，task.schedule 且订阅 timer；拒绝歧义/不存在当地时间 |
| GetTask/ListTasks | owner/账号范围、分页 | 当前状态，敏感元数据按范围在线核验 |
| CancelTask | task_id、owner、幂等键 | 未交接可取消；已交接报告边界，不能说事件已撤回 |
| GetOccurrence/GetOperation | occurrence/operation 引用 | 触发意图、交接进度和取消竞态事实 |

只支持现有一次性任务，不新增 cron/周期、调度数据库服务或任意脚本。每逻辑实例默认 1024 个未清理任务，安装聚合8192个，防止实例扩容绕限；owner 绑定稳定实例，不能任意转移任务。

### 3.5 Model（DD03.7）

| 方法族 | 输入 | 输出/权限 |
| --- | --- | --- |
| Get/ListModelMetadata | 资源范围 | 脱敏模型别名、状态、配置版本，不返回端点凭据 |
| UpdateModel/RotateProviderSecret | 配置、预期版本、幂等键、机密输入 | 版本及生效阶段；只允许当前批准的 HTTP provider 范围 |
| Chat | 稳定 logical_call_id、插件/资源受限委托、文本 messages、期限 | 已知结果或未知执行语义；无自动供应商重发 |
| GetCallStatus | 原调用者或显式敏感查询范围、call_id | 元数据/已知执行状态，不新增正文历史接口 |

模型密钥更新只影响后续请求；在途使用固定配置版本，不静默换供应商重发。总文本/消息数及上游响应边界沿用现行 64 KiB/128 条、1 MiB 响应及 64 KiB 结果限制，先不扩展模型能力。请求正文不写普通日志；调用意图留 hash/引用及必要状态，不把对话历史功能悄悄加入范围。

### 3.6 Control（DD03.7）

浏览器唯一业务入口。现有认证接口映射 Identity，账号/插件/动作/白名单映射 Runtime，任务映射 Scheduler，模型映射 Model。普通权限页只展示固定角色与获准资源授予；不生成自定义角色。

状态与日志聚合不得跨库联表。REST 列表仍满足快照分页；首次 WS 认证、授权、SESSION_READY、resync 继续遵守已发布规则。新状态源使用所有者版本，推送断档后重新读取快照。现有 messages 仅 ID/会话/方向、显式 message.read、received 子集不扩大；普通 UI 流不是审计/业务 outbox。

Action 查询在目标契约中分两类：插件只能查询自己的 Action；控制用户持明确账号 action-inspect 资源授予后可查询范围内的结果/元数据。不得凭 admin 自动读取正文，默认角色不隐含 message.read；统一读取 Runtime 同一权威记录，不保留永远无生产者的控制 owner 分支。

## 4. 统一数据、权限与并发语义

### 4.1 标识与字段边界

新平台资源/operation/message/request ID 采用服务端 CSPRNG UUIDv4、小写标准文本；数据库可用原生 UUID。沿用来源提供的合法 event_id/action_id/task_id，验证当前长度及作用域，不集中重编号。平台账号原始 ID 保持字符串，不转换整数。不导入旧开发 ID；新平台资源与当前平台原始 ID 建立业务绑定。

资源版本用所有者单调版本，更新要求 expected_version；条件不满足 CONFLICT，不盲重试覆盖。空 expected_version 仅适用于明确创建语义。generation 由所有者在持久事务中单调增长，并与 run/connection 身份绑定；过期回执只返回重复/过期事实，不修改新状态。

namespace ID 不接受任意字符串 owner。SDK 使用 instance-private 或安装共享的受限 scope，Runtime 从认证上下文查表解析。共享写强制条件版本：CAS 期望不存在/某版本，冲突返回已知版本元数据；私有 Set 明确采用覆盖语义。CAS 保证存储更新原子性，不承诺与外部平台调用原子。

### 4.2 风险分级

普通缓存读仅允许健康状态、脱敏实例在线计数/基础版本等明确列出的非敏感元数据。配置、任务 payload、日志、审计、Action、namespace 及身份数据均敏感在线核验。所有变更、平台动作与模型调用在线核验。

Ack/Renew/记录已有平台结果是受当前认证流及 fence 约束的完成/控制消息，不反复查询已撤销用户会话来丢弃既有事实；停止新投递/取消租约另有明确事务。权限变更影响新的副作用和后续交接。无授权源可确认的高风险工作留持久暂停态，不发平台请求。

### 4.3 身份材料定稿

内部受限证明采用 JWT/JWS，固定允许 Ed25519 签名算法，验证 iss/aud/sub/exp/iat/nbf/jti、主体资源范围、委托 actor 链与 mTLS 客户端证书绑定；不接受算法自动降级、none、token 任意 jku/x5u 键地址。凭据仅在边界解析；具体成熟 JWT 库与补丁版本在实施依赖锁定时确定，不自行编写验签算法。

采用证书绑定与委托的标准语义，但不宣称内部 gRPC 签发方法实现了完整 OAuth HTTP 服务器。证书由受控部署签发能力提供，离线根和受限中间 CA，不将 CA 私钥放进 Identity 数据库；工作负载证书有效 24 小时，12 小时触发续签，验证登记 SAN/服务身份，不仅验证 DNS。无受信证书不进入 ready；轮换失败在原有效期内告警，到期拒绝并关闭失效服务连接。

主体撤销同时进入 Identity 当前状态和失效流，流只是加速器。内部委托最多两次业务转交（Control→Runtime→Model/Host），每次新 audience 且收窄；正常任务后台使用已受理任务的执行策略及当前服务证明，不无限保存人类 access/refresh。临时秘密由部署机制供给；浏览器不接触平台工作负载证书。

Adapter/Plugin 生产使用服务端验证 TLS，加逐实例可撤销 bootstrap token 首次登记，换取接收方受限 60 秒证明，主动在到期前刷新；流每秒核验到期/撤销，过期停止新副作用。受管 Host 通过一次性受限文件描述符/秘密挂载交付凭据，不透传所有 Core 环境变量。bootstrap token 不作为无限长期流授权。外部自管实例使用受控部署交付；不开放匿名注册或自动下载安装包。

证明内部 exp-iat 最多 56 秒，系统已知跨节点时钟差上限 2 秒、验签额外容差最多 2 秒，总有效预算不超过已确认 60 秒；超过已知时钟误差界进入不可信/拒绝，不因请求活动续期。普通只读缓存用本地单调时钟自权威验证起至多 60 秒，与证明更早到期者为准；重启清缓存。私钥与 token 不进入 outbox、日志或公开投影。

浏览器撤销承诺单独保留：每个 REST 向权威 Identity 验证，失败不放行；WS 每 250ms 发起/共享当前会话检查，单次预算 500ms，距最后确认超过 750ms 即关闭，调度余量到 1 秒；压测/调度延迟不能兑现时该实例不 ready。此为设计 SLO/验收约束，不声称异步网络具有无条件实时保证。

### 4.4 稳定错误与恢复矩阵

以下为目标契约的规范映射，实施时写入 proto/OpenAPI/AsyncAPI 和测试；当前仓库尚未按本文更新。HTTP 映射保留已确认的用户可见语义，gRPC 按目标状态明确表达。失败体保持稳定 code、request_id、trace_id，诊断文本不替代码。

| 业务码 | gRPC | HTTP | 默认恢复 |
| --- | --- | --- | --- |
| INVALID_ARGUMENT | INVALID_ARGUMENT | 400 | 修正输入，不重发原错误 |
| UNAUTHENTICATED | UNAUTHENTICATED | 401 | 重新认证，不能以无限旧凭据重试 |
| PERMISSION_DENIED | PERMISSION_DENIED | 403 | 不重试，不以换键绕过 |
| NOT_FOUND | NOT_FOUND | 404 | 不推断外部未执行，核查授权与原操作 |
| CONFLICT | ABORTED | 409 | 区分预期版本/异内容/在途，读当前状态后决定；不能盲目覆盖 |
| RATE_LIMITED | RESOURCE_EXHAUSTED | 429 | 在明确未受理或原幂等键下按 retry_after 退避 |
| TIMEOUT | DEADLINE_EXCEEDED | 504 | 结果可能未知，先同键查询，不盲目重发模型/平台 |
| UPSTREAM_UNAVAILABLE | UNAVAILABLE | 503 | 可恢复本地交接同 ID；有副作用请求先查结果 |
| PLUGIN_UNAVAILABLE | UNAVAILABLE | 503 | 保留操作/投递，等目标恢复或明确过期 |
| CAPABILITY_NOT_SUPPORTED | UNIMPLEMENTED | 503 | 不自动降级成丢字段的调用 |
| INTERNAL/未知码 | INTERNAL | 500 | 保留关联标识并诊断；不假定回滚 |

REST 方法不允许为405并返回Allow；不存在路径404；跨域首轮默认关闭，仅受控精确origin白名单可启用，凭据请求禁止通配origin。HEAD/OPTIONS仅按目标OpenAPI声明执行，不以框架默认暴露管理写方法。

错误之外用类型化失败上下文表示是否已持久接受（未接受/已接受/未知）、operation引用、恢复动作与retry_after，不把任意内部详情塞进公开Error.details。高风险状态未知时恢复动作不能是“换新键再执行”。调用中断可能没有错误体，SDK仍须按已知请求身份恢复。客户端取消产生gRPC CANCELLED时不额外创建业务FAILED；业务取消返回操作状态。

### 4.5 固定角色与资源范围

admin可在明确管理范围执行身份/角色分配、凭据、账号、模型/密钥、共享成员与删除恢复；operator在获授资源范围内操作插件、任务、白名单和查询状态，不默认管理身份或模型秘密；viewer只读获准脱敏状态。不以角色名自动生成全资源通配授予。

Action元数据检查、日志/审计、消息元数据等额外敏感能力须显式资源授权；不因admin角色自动含message.read。插件主体不套人类角色，按安装/实例授予的能力与业务资源交集执行。身份授予仍由Identity权威，资源门禁由业务所有者决定。

命令/查询的稳定权限标识在目标契约中建立封闭词表与上述方法族一一映射；未知标识拒绝，不能客户端自由新增权限。初始可信管理员通过离线引导获得明确管理范围，不能空角色自助创建全局权限。

## 5. 默认时间、容量与恢复参数（DD03.8）

所有上限是目标默认值/配置约束，尚未修改现行运行参数。新 API 可配置但不得放宽已确认安全上界。

| 项 | 默认/上限 | 行为 |
| --- | --- | --- |
| 普通查询 RPC | 2s / 5s | 遇依赖故障明确返回；敏感查询不退到缓存成功 |
| Identity 在线授权 | 500ms / 1s | 与调用总预算取小值，无法确认则拒绝/暂停 |
| 写命令受理 RPC | 5s / 30s | 只等待本地提交，网络等待不等于业务有效期 |
| 登录/刷新 | 5s / 30s | CPU 限额/限频，秘密重放沿既有规则 |
| Model 单次 Provider | 30s / 30s | 与调用剩余时间取小，不自动重试供应商 |
| outbox 每次交接 | 5s | 跨重启复用 message_id；不因一次超时删除记录 |
| outbox 退避 | 0.5s 起，指数到 30s，±20% jitter | 单层派发重试，不嵌套 SDK/gRPC 自动重发；永久拒绝记失败并告警 |
| outbox 批次领取 | 100 条、30s claim | 单记录可恢复；具体并发由容量限制，不锁住网络调用 |
| 插件投递租约 | 30s，10s 发起续租 | 每次延长至多 30s，从初次领取起总上限 300s；服务器时间裁决 |
| 插件重试 | 现行 5 次重试与 1/2/4/8/16s、±20% | 是首次之外重试数，持久记录每次尝试，最终死信 |
| Adapter/Host 心跳 | 5s，15s 判疑似离线 | 疑似离线不证明旧进程已停止；fence/恢复规则仍需满足 |
| 资源配置操作观测 | 30s 无进展标 stalled | stalled 非失败终态，不自动重复启动或回滚已生效配置 |
| DB lock/statement | 锁等待 250ms，普通事务 2s 预算 | 锁冲突重试受总预算，迁移另设运维窗口 |
| 内存/持久队列 | Runtime全域1024个未终态事件、每实例1024个未终态投递，服务待交接10000条 | 达限拒绝新增受理且不产生副作用；既有工作保留，硬容量覆盖未决数据 |

高风险授权判定到本地写提交/派发的间隔上限 1 秒；超过重新核验且不得持事务等待网络。撤销与副作用不是全局原子，审计记录检查版本和实际派发时刻。高风险持续流的新副作用每次检查，不以首帧认证代替。

只对明确未执行或具幂等键的远程操作在上层预算内重试；同步写最多 2 次同键尝试，由一个调用层控制。数据库 deadlock/serialization conflict 最多总 3 次、20/50ms 抖动退避，且仅重做纯本地事务；COMMIT 结果不明必须按原幂等键查询，不能判回滚后重跑外部调用。

交接记录不在承诺未兑现时静默过期；超过业务 deadline 转显式过期/人工处理。派发次数过多（100 次）停自动发送、标需处置，但不删除记录；管理员可同 message_id 恢复。失效/清理类控制记录独立保留服务容量，不能被普通事件挤掉；权限失效以可合并的持久授权版本/dirty标记收敛，普通outbox满不能静默丢失已提交撤销。依赖故障须保持拒绝/停用门禁。

## 6. 单一目标契约与恢复（DD03.9）

用户明确项目仍在开发、无需历史兼容包袱。采用一套目标契约和组件版本，直接重构 proto/kiarabot/v1 与 API v1 的定义并重新生成所有客户端；不提供 v1/v2 桥接、旧字段别名、历史数据导入、双写或混合版本生产运行。包路径中的 v1 表示新目标的首个统一契约，不是承诺兼容当前开发快照。

本轮仍只写设计。实施时统一修改唯一来源、模块说明、实现、fixture 和测试；删除失效接口应有同变更调用方替换，不为保留旧测试而接受旧身份或缺省租约。CI 核对目标 descriptor、跨语言往返及端到端语义，不把旧开发快照作为必须兼容的发布基线。未来首次正式发布后再建立发布兼容窗口，不预先建设通用协议路由系统。

版本化流恢复游标包含源服务、投影/流 epoch、范围与序号；服务重启/游标超出重放窗必须明确 resync，不能假装无事件。Identity 失效流维持最近 10 分钟/4096 条有界重放，断档重新建立授权快照；在线检查继续守住敏感操作与浏览器撤销。

统一操作状态采用 Accepted/Running/Blocked/Succeeded/Failed/Cancelled/Expired，Blocked 为可恢复非终态，须带稳定原因与下一次处理信息；结果未知另记 execution_outcome，不伪造 Failed。Action 保留 Accepted/Succeeded/Failed/Expired/Cancelled，平台执行是否已知单独表达，不将 HTTP 超时转成 Action 失败。

目标上线前，旧开发数据由维护者按需导出后显式清理；本轮不执行任何清库、删文件或 schema 变更。初始化命令仅负责创建空库到目标 schema，生产进程只校验；未来业务数据的可靠恢复与发布后升级仍是必要能力。

## 7. 验收与实施门槛

| 领域 | 必须证明 |
| --- | --- |
| Identity | 身份/受限委托、错误 audience、证书绑定、过期/撤销、Identity 断网及重启缓存不放大 |
| Runtime | 双模拟适配器、多插件实例隔离、四元组顺序、旧尝试/旧连接拒绝、重复事件不新增目标、预算并发 |
| Host | 重复 start 不多进程、崩溃后认领/查询、未知旧执行不跨主机接管、包/参数白名单 |
| Scheduler | 触发确认丢失、重复交接只入队一次、取消竞态、IANA/DST、重启恢复 |
| Model | 开始记录成功才调用、返回前后崩溃不重计费、配置轮换固定版本、调用未知分类 |
| Control | 统一目标路由到真实权威、显式 scope 检查、脱敏、不越权读 Action、WS resync/会话撤销/部分依赖失败 |
| 版本 | 统一目标 fixture、生成客户端同版、必需租约/身份、错误/状态矩阵、不兼容客户端明确拒绝，无旧版桥接 |

这些是实施验收要求，不是本轮已执行测试。协议字段号、具体接口文件、SQL DDL、驱动/依赖精确版本、证书签发工具及部署资源量在实施任务设计时锁定；不再保留本轮服务边界、数据库引擎、交接模式等方向选择为待确认。

## 8. 来源与文档影响

设计借鉴标准机制，不声称其自动实现业务保证：[JWT BCP](https://www.rfc-editor.org/rfc/rfc8725.html)、[证书绑定 token](https://www.rfc-editor.org/rfc/rfc8705.html)、[委托语义](https://www.rfc-editor.org/rfc/rfc8693.html)、[UUID](https://www.rfc-editor.org/rfc/rfc9562.html)、[gRPC deadline](https://grpc.io/docs/guides/deadlines/)。具体参数及能力边界是本项目自主设计决策。

本轮仅新增设计文档并更新总览/确认索引，不修改 proto/API/模块现行说明或实现。服务拆分、统一新契约、权限、新状态及恢复语义均是与现行实现的显式差异，实施前必须同步唯一来源与相关 doc/01–08；受保护 doc/18 保持原样。

## 9. 目标查询与精确管理目录补全（实施定稿，仍未实现）

本节落实已批准管理范围，不修改服务/数据库边界，也不新增自定义角色或资源继承。Model增加GetOperation（OperationRef→Operation，resource.read）与GetModelConfiguration（exact model/scope→无秘密配置、secret存在性和observed_at，model.manage），默认2秒/最多5秒、在线授权；操作查询校验存储owner/resource，不由客户端重贴scope或Control代裁定结果。后续目录/Runtime/Identity字段在同节随唯一契约逐能力补齐。线协议及生产实现状态仍分别以proto/API和execution_scopes.target为准。

Runtime增加五类typed Get/List（Account/Installation/Instance/Adapter/Host）及GetInstanceConfig，复用已有record并补注册metadata/配置应用投影；默认2秒/5秒上限、在线授权。Get选择exact正确kind；List的ResourceRef catalog必须封闭RESOURCE_CATALOG且为离线登记runtime/资源族UUID，无all/祖先继承，只返回当前本人另获具体child grant的集合。PageRequest输入与SnapshotInfo输出分离，10分钟快照仍逐页核验当前授权版本；目录登记/可靠创建与本人发现的Identity契约随后补齐，现均未实现。
