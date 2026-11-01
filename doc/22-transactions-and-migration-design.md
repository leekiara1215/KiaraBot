# KiaraBot 事务、数据模型与初始化设计

- 版本：v1.0（设计定稿，未实现）
- 日期：2026-11-01
- 来源：用户授权助手决定后续细节，并明确开发阶段无需历史兼容包袱
- 依据：[方向基线](19-architecture-direction-baseline.md)、[总览](20-detailed-architecture-design.md)、[通信与方法](21-service-contract-design.md)
- 范围：目标关系模型、原子组、恢复、初始化及验收；不执行 DDL、迁移、清库或修改实现

## 1. 统一事务决策（DD04.1）

采用每服务本地 Unit of Work + Repository + outbox/inbox，不使用跨服务数据库事务、2PC 或数据库共享表。PG 默认 READ COMMITTED，业务唯一约束、按固定顺序行锁与条件版本构成一致性保证；不假定提升默认隔离级别就自动修复所有竞态。

单用例只能由拥有数据的应用服务开启一个本地事务；Repository 不自行 commit。领域层只使用内部类型，不持 SQL、PG 连接、Protobuf 或 JSON。查询返回显式“无记录”与存储故障，不以空值掩盖故障。

事务内禁止网络/插件/供应商调用。高风险先在线授权，取得主体、资源、授权版本/时间；进入短事务校验当前业务版本、gate/fence、额度及目标状态。提交或实际派发检查点距授权超过一秒则退出事务重新核验，不能带着锁等待 Identity。这个间隔是有界竞态，不是承诺跨服务即时原子撤权。

所有并发路径有唯一键作为最后防线；先 SELECT 再无条件 INSERT 不算幂等。按资源 ID 排序取锁，数据库死锁/可安全回滚的冲突按通信设计有限重试；COMMIT 连接中断的结果未知只能同键查询或幂等恢复，不能据错误猜测已回滚。

首次部署采用每权威域一个活动进程，恢复前获得数据库内排他进程锁；第二个进程不得 ready。锁丢失立即停止派发、受理和租约更新。此锁防误启，不构成多副本 HA；异步副作用的旧执行还需实例/连接/动作去重隔离。

## 2. 数据所有者与逻辑表

以下为关系模型，不是可执行 DDL。字段使用内部值类型，载荷可有受限序列化列；禁止以单行全量快照替代全部关系表。

| 服务 | 核心表/集合 | 关键唯一与引用约束 |
| --- | --- | --- |
| Identity | principals、credential_versions、sessions、fixed_role_assignments、resource_grants、authorization_versions | subject 唯一；session 唯一；授予主体/权限/资源去重；凭据不可复用给新主体 |
| Runtime | bot_accounts、adapter_bindings、adapter_connections、plugin_installations、plugin_instances、plugin_connections、desired_configs、config_applications | 稳定资源主键；当前活动代次唯一；实例引用安装；配置版本由所有者递增 |
| Runtime | events、conversation_watermarks、delivery_targets、delivery_attempts、ordered_delivery_heads | event_id 在授权事件来源作用域唯一；目标为 event+instance 唯一；attempt 唯一；有序头每实例/四元组唯一 |
| Runtime | actions、action_idempotency、quota_windows、daily_budgets、adapter_result_evidence | action_id 全局唯一；owner+key 唯一；预算为动作/账号/UTC 日唯一；平台证据按 action/结果版本去重 |
| Runtime | namespaces、namespace_members、namespace_values、namespace_quota_counters、storage_receipts | 私有 scope 绑定稳定实例，共享 scope 绑定安装；namespace+key 唯一；CAS version 和限额同事务 |
| Scheduler | tasks、task_occurrences、trigger_outbox、trigger_receipts | owner_instance+task_id 唯一；task+occurrence 唯一；一次性任务只一个 occurrence |
| Model | model_configs、encrypted_provider_secrets、model_calls | model 别名/版本；调用 owner+logical_call_id 唯一，不存原始秘密于审计 |
| Host SQLite | command_journal、managed_executions、execution_observations | command_id 唯一；实例/执行代次唯一；受控命令 hash 防同键异内容 |
| Adapter SQLite | event_outbox、platform_sequences、action_execution_journal、platform_id_mappings | 未确认事件原 ID 稳定；action_id+规范化请求 hash；序号按平台会话四元组 |
| 各业务服务 | command_receipts、operations、audit_entries、outbox、inbox、schema_versions | 调用方/方法/资源/key 唯一；outbox source+message 唯一；inbox source+message 唯一 |

跨库只保存类型化稳定引用，不创建跨库 FK、共享视图或外部表。单库内 FK 与删除限制防误删；涉及收据、审计、未决动作不级联清空。Control 持有可重建 UI 技术状态，不再维护业务配置权威。

新资源 ID 用 UUIDv4，旧平台原始 ID 作为平台引用。不需要旧开发 ID 映射/导入；四元组顺序仍区分适配器、账号、会话 ID 和会话类型，内部稳定账号与平台账号由当前绑定显式解析，不能合并群/私聊。

## 3. 公共收据与 outbox/inbox 原子组

### 3.1 本地命令模板

1. 边界验凭据并转换内部上下文；高风险查询当前授权，不持事务。
2. 开事务，锁定幂等 scope（或插入唯一占位再锁），检查资源 gate/预期版本。
3. 同键异内容拒绝；同内容已完成返回原业务结果并使用本次 request_id；先重新鉴权再重放，不把旧凭据响应当作永久有效。
4. 提交业务修改、成功收据、必要审计和待交接记录；失败全部不生效。业务拒绝需要审计时，用不含部分业务变更的事务记录拒绝。
5. commit 成功后才响应/由独立派发器发出。外部行为不能放到自动重跑事务回调。

成功内部命令至少七天；普通命令满额拒绝，不淘汰有效期记录。登录/刷新含秘密的收据加密并检查轮次有效，沿既有 24 小时控制收据等已确认作用域要求，不任意统一期限。模型调用收据不被普通命令模板误用成可无限重放的模型正文缓存。

### 3.2 可靠交接

发送方事务同时写 state+outbox；派发器短事务领取记录（行锁/SKIP LOCKED），保存 claim token/期限后提交，网络调用在事务外。接收方事务保存 inbox 唯一键、业务修改及结果/必要审计后确认；确认丢失可同 ID 重发。发送确认更新必须匹配 claim，过期工作者不能覆盖新领取者进度。

message 包含 source_service、message_id、operation/causation、资源版本、payload hash、created_at 和 valid_until，身份由认证绑定。普通交接有效窗口默认七天，inbox 去重保留至至少 max(首次接收+七天, valid_until+七天)；过期新消息即便去重记录已清理也拒绝执行。有效期限与发送方重试上限配套，人工恢复不能悄悄延长同一过期命令；确需新操作用新 ID 并重新验证业务意图。

实例期望同步以最新 desired_version 收敛，旧版本重到不会回滚；权限失效通知以持久授权版本水位+范围快照收敛，不依赖永存每个通知。撤销事务即使普通 outbox 满也能提交：保留单主体/范围可合并 dirty/version 标记，订阅者重新拉取，不丢权威撤销版本。

outbox 发送次数达到 100 次进入需处置状态，不丢记录；业务到期变明确过期/拒绝，清理/撤销等必须保持本地 gate，告警并通过状态协调恢复。数据库容量不足时停止新受理，不能伪造持久成功。失效/停止/审计保留控制容量，避免普通流量阻断安全收尾。

## 4. 事件接收与插件投递（DD04.2）

### 4.1 接收事务

Adapter 在 SQLite 中先持久 event 与分配的 sequence，再发 Runtime。Runtime 验证当前认证来源、账号绑定、连接代次、事件类型和大小；不得伪造 TimerTriggered。

本地事务按稳定顺序锁住来源/会话、相关实例目标及队列计数：

- 相同事件键且相同内容返回已入库；异内容 CONFLICT，不推进水位、不追加目标。
- 检查四元组 sequence；非重复旧序号拒绝，跳号/缺序号按既有规则标 degraded。
- 读取该事务内的获准已注册且配置有效目标快照；与实例停用/订阅变更用相同版本/行锁冲突控制。
- 保存 event、watermark、每目标 instance delivery、必要审计与队列计数，同一提交。

没有目标也保存 inbox，不因未来实例注册补投；fan-out 对每实例分别独立，有序组为实例+会话四元组。队列不足整体拒绝，不部分保存目标。Adapter 仅在持久/重复确认后释放上报 outbox；断线继续原事件重投。

### 4.2 尝试与租约事务

领取锁 delivery 与 ordered_head，验证实例 gate、当前执行/连接、配置版本；未终态的有序头阻止后续，包括退避。写 attempt_id、当前 fence、lease_until、attempt_start、次数与审计后提交，再网络发送。

ACK/Renew 以 event+instance+attempt+有效执行/连接匹配。ACK 与超时处理竞争同一行：成功先提交则终态不被 timeout 改写；timeout 先换代则迟到 ACK 返回过期，不确认新 attempt。重复成功 ACK 返回原确认且不重复业务审计。续租仅当前未过期且未取消 attempt，延长至 min(数据库当前时刻+30s, attempt_start+300s)，不能复活已过期租约。

数据库 UTC 为租约裁判，客户端只建议续租；调度工作者用单调时钟安排本地唤醒，恢复时以数据库时间重判。Runtime 重启换 process epoch 并关闭/失效旧流，再恢复在途；身份/连接换代使旧 SDK 新副作用被拒绝，但不声称撤销了已经开始的外部执行。

重试最多首次之外五次，失败间隔与 jitter 按通信设计；非重试错误直接死信。重放只重置目标实例，保留 event_id、新 trace 和新 attempt，不能回退其他实例或已完成顺序水位。

## 5. Action、配额与平台执行（DD04.3）

### 5.1 受理原子组

在线授权后锁账号业务 gate、目标策略、幂等 scope、账号/会话 quota 和 UTC 日预算，锁顺序固定。默认滑动窗口沿现行账号10/分钟、群3/分钟、私聊1/10秒；新实现将窗口占用持久化，避免进程重启重置策略。日预算保持持久化。

同键同内容先返回原 Action 不重复占额；异内容冲突。Mute/Kick 必须显式开关、群白名单、正预算；其他 payload 校验与能力满足后才原子写 Action、quota 占用、审计/收据和可派发项。

任何提交失败不调用平台。出站配额按一次被接受的业务意图计数，取消/明确平台失败不自动退额，避免失败/取消循环绕限；这是保守预算取舍，面板明确说明。首次提交已过期的 Action 仍持久化 EXPIRED 结果和幂等身份但不占额；被拒绝未受理的不占额。

### 5.2 派发与结果

派发前再在线核验插件权限及当前账号/白名单/启用策略；锁 Action 检查取消/到期，提交 dispatched/派发身份和审计，再发适配器。派发与取消竞争同一行：取消先成功不派发，dispatched 先成功则取消冲突，不谎称撤回。

适配器先在 SQLite 持久登记 action_id+request hash+准备执行，才调平台；完成后先保存平台结果再回传。同 action_id 同内容重发返回已知结果或未知，不重复执行；异内容拒绝。调用中断、平台超时、进程崩溃后不确定是否执行，不自动再调用平台。

Runtime 结果事务验证账号/Action 绑定、内容及允许的结果状态，写一次终态及证据/审计；重复相同结果幂等。迟到结果不能覆盖 deadline 已提交 EXPIRED，另存必要执行证据说明可能副作用。适配器重连可用当前凭据上报自己同账号的既有执行证据，不要求旧连接仍存在，不授予新动作执行权。

Action 查询直接读这份记录。插件仅自身 owner；控制用户需明确账号范围的元数据查询权，不返回正文/平台秘密，默认 admin 不是隐式跨 owner 放行。

账号/Adapter 迁移须先停旧出站并确认旧执行停止或人工隔离，再携本地未决执行日志切换；不能换一个空 SQLite 日志就重放待定平台动作。网络失联只标 unknown，不自动接管该账号。

## 6. Scheduler→Runtime 触发与取消（DD04.4）

CreateTask 在 Scheduler 事务写任务、owner_instance、规范化时间、收据/审计；DST 歧义/不存在拒绝。一次性 task 的 occurrence_id 在创建时确定并持久，不因重试换 ID。

到期领取事务锁任务，检查取消/owner gate 镜像版本和容量，写 occurrence、触发 outbox 与触发中状态。claim 不持锁等待 Runtime。

Runtime AcceptTimerTrigger 在线/受限上下文校验后，事务将 Scheduler message/occurrence 去重、有效目标实例、TimerTriggered 事件与 delivery 一并提交；同 occurrence 重发不会再入队。明确 owner 已停用/删除则拒绝且不创建事件。Scheduler 收到持久确认后事务标交接已确认/COMPLETED；COMPLETED 仍表示事件已持久入队，不是插件回调成功。

取消规则按本地事务线性化：

- 尚未创建触发意图时，取消成功，保证不会创建该 occurrence。
- 已进入交接阶段时，普通取消返回冲突，不承诺撤回。可停用目标或显式协同处置，但不把“发送取消请求”当成取消保证。
- Runtime 已接受但确认丢失，Scheduler 仍同 occurrence 查询/重发确认，不因超时改为已取消。

这是首轮明确取舍，避免实现不可靠的跨库抢取消。界面显示触发交接中/已入队的区别；接口文档明确取消窗口。

## 7. 插件期望、宿主进程与配置（DD04.5）

Runtime 修改 desired_state/config 时锁实例/安装和 expected_version，同事务写版本、审计/收据、operation 和 Host/Plugin 待交付项，提交即“期望已保存”，不报告进程已启动。

Host SQLite 事务先登记 command_id/目标代次/hash，再在事务外 posix_spawn。启动参数来自已登记包摘要和允许参数集，环境最小化；用本地进程组与执行 marker 记录 PID+启动标识，不靠 PID 数字本身判断旧执行。崩溃恢复发现“已准备、无确认”时先检查/认领受管进程，无法确定时阻止重复启动并报告需处置。

执行报告由 Runtime 事务比较实例 desired_version/execution_generation 后应用；旧状态只作历史证据，不将最新状态回退。Host 接收 newer stop 时先持久期望再停止进程，10s 宽限后 TERM/KILL 按现有平台能力回收；无法确认终止不显示成功。不因 Host 离线自动在别机启动同一逻辑实例。

插件配置携完整版本、delivery_id 和应用期限，SDK 回调完成才 ACK。Runtime 只接受当前连接/执行代次和 delivery_id；旧 ACK 不盖新版本。配置保存、进程启动、协议注册、配置应用是独立字段。新配置未 ACK 的实例不接新事件；失败保留原实际版本并报告错误，不悄悄将旧配置标为新版本。

## 8. Namespace、共享与 CAS（DD04.6）

Runtime 根据认证实例解析私有 namespace 或已授权安装共享 namespace，插件不能任意指定 owner。事务锁 namespace gate、成员/版本、quota counter、目标 key 及 idempotency scope，原子完成授权本地门禁、CAS、限额、数据、收据/审计。

私有 Set 允许覆盖；共享写使用 expected_version，缺省只允许 expected-absent create，更新/删除要求当前版本；冲突不覆盖。多 key 原子事务不在首轮开放，单 key CAS 足够支撑明确协调；不能据此实现平台端到端 exactly-once。

默认每实例私有1024键/1MiB、共享空间1024键/1MiB、每值64KiB；安装聚合8192键/8MiB，包含所有私有/共享，增加实例不能绕过。写收据每实例10000条、安装聚合80000条，七天保留；配额不足拒绝，不能提前删收据。限额可向上配置但需容量验收，不默许无限。

TTL 沿现有可选永久或未来不超过30天；读取过期返回不存在，删除过期/不存在幂等成功；重放不续 TTL。共享成员撤销事务改变 namespace gate/成员版本，与写事务竞争同一行；已撤销成员的新写被拒绝。私有数据按稳定实例保留，不因 execution/connection 换代重建。

业务幂等 scope 包括认证主体、方法、有效 namespace 和 key；跨实例同幂等字符串不会返回别人的响应。每次重放先重新验证当前可见范围。敏感共享读取也在线检查 Identity，不凭旧委托绕过撤销。

## 9. 模型意图、外部调用与未知结果（DD04.7）

Model 本地第一次事务：在线授权通过后，检查 owner+logical_call_id、配置版本、容量和服务并发，写调用意图 STARTED、必要审计和固定供应商配置引用，提交后才调用 Provider。

第二次事务：保存已知成功/失败或 UNKNOWN、计量元数据及审计，再响应。两个事务之间有网络/计费，不能回滚成“未调用”。用户取消/Runtime 断开仅尝试取消上游并最终记录事实，不保证未计费。

同 call_id 重到不再调供应商：在途返回正在处理/查询引用，已终态返回受限元数据或稳定冲突/结果不可回放说明。首轮不承诺持久重放模型正文；Runtime 活连接可转发首次结果，丢失则查询已知执行状态并由插件业务决策，不自动用新 call_id 重跑。调用意图、状态和去重保留至少七天；未决 UNKNOWN 先人工/规则处置，再只保留最小防重摘要，不能清掉后盲重发。

配置/密钥轮换只影响后续调用；密钥密文与加密根分离，提供商访问令牌不进入审计/outbox/插件。密钥销毁需等在途配置引用释放或显式取消，不以恢复旧配置自动复活已撤销秘密。

## 10. Identity、授权与生命周期事务（DD04.8）

会话刷新锁 session 与刷新轮次，业务变化、加密成功收据、审计和失效版本同事务；不同键使用已轮换 token 拒绝，同键响应须检查当前会话/代次仍有效。注销/撤销不会等待远程 WS 关闭才提交，Control 每次权威验证守住浏览器语义。

授权/凭据变更锁主体/资源 grant version，预期版本不符冲突，变更和失效水位同事务。快照先取得权威水位再消费之后增量；缺口时阻止未知范围缓存放行，敏感操作依旧在线核验。公开验签公钥可缓存但不等同授权状态。

资源停用与删除由所属服务 gate 先提交，随后发送版本化撤权、停止/取消、清理命令；每个参与者同 command_id 幂等。恢复不能取消已完成外部副作用、重新有效旧 token 或恢复已取消任务。操作状态区分受理/执行/阻塞与终态，各阶段可查询，Control 不自行裁决业务完成。

目标默认恢复窗口：非秘密配置、插件私有/共享内容逻辑删除后30天；原 TTL 先到则先过期，不续期。最小资源墓碑、操作/必要审计默认180天，ID永不复用；普通消息内容不因墓碑保留180天。凭据立即失效，密文不以普通30天恢复自动复活；秘密清理按在途引用和轮换要求单独执行。

内部/Host 收据七天、Control 收据24小时、Action终态168小时等各自保留；投递/inbox未满足交接恢复条件不清理，未决记录超过30天进入人工处置和最小化归档，而非无声删除。集中日志默认7天、无正文和秘密。清理每60秒按批执行，清理失败告警，不影响 gate 生效。

## 11. 开发阶段初始化与发布（DD04.9）

采用全新空库初始化与统一客户端重建，不做旧 SQLite 快照导入、旧协议兼容、双写、逐行对照或历史数据自动保留。旧开发数据如有价值由维护者单独导出；本轮不执行删除。保留 doc/18 首版原文作为设计历史，不把它当作新运行路径。

每服务有独立 schema version、初始化命令和迁移身份；目标首次上线前使用空库/测试数据反复验证。运行身份没有 DDL 权限，启动只查匹配版本/必要表/基础可写性，不自动升级。

就绪依赖顺序：数据库/证书与信任根 → Identity → Runtime/Scheduler/Model → Host与Adapter → 插件 → Control业务就绪；Control可先启动但依赖不就绪不报告业务可用。Identity 不依赖其他服务启动；服务各自状态清楚，不以进程存活当作全链路 ready。

首次部署一套统一目标组件；不兼容客户端明确拒绝。未来首次正式发布后才维护版本升级/回滚兼容窗口。首次初始化失败可重跑，必须有版本与对象一致性检查，不能跳过半建库状态。

已产生新业务数据后的恢复仍需：停止新副作用并隔离旧实例 → 协调中央各逻辑库与Host/Adapter日志的备份点/水位 → 恢复 → 更换运行epoch、失效旧连接/旧凭据视情况 → 校验收据、未决动作和任务交接 → 再开放。不能恢复一侧日志就自动重发平台动作。备份默认每日、保留7天，RPO目标24小时/RTO目标4小时是首轮演练目标，不是已验收承诺；密钥根单独安全备份，恢复不能重新激活已撤销身份。

## 12. 必须通过的故障验收

| 用例 | 故障注入 | 断言 |
| --- | --- | --- |
| 入站事件 | 提交前/后断线、同 ID 异内容、并发停用 | 无部分目标/水位，重复不增投，拒绝不伪造成功 |
| 投递 | ACK丢失、续租与超时并发、旧执行复活 | 新尝试不被旧ACK确认，有序头不并发，续租有总上限 |
| Action | 并发预算、取消竞态、平台执行后结果丢失、两侧重启 | 不超额、不重复平台副作用、未知不伪失败、EXPIRED不被迟到覆盖 |
| 任务 | 意图提交后宕机、接收入队后确认丢失、取消到期竞争 | 同occurrence入队一次，COMPLETED只在持久接受后 |
| Host | start前后宕机、PID复用、网络分区后迁移 | 不盲目二次启动、不将未知旧执行视为已停 |
| 配置 | 并发版本写、旧应用ACK、重连 | 冲突不覆盖、新版本未应用不投新事件 |
| namespace | 两实例CAS、成员撤销、限额并发、重放 | 无越权和超额，同键不串owner，重放不续TTL |
| Model | 上游已执行后崩溃、结果提交失败、密钥轮换 | 不自动再调用供应商，UNKNOWN持久，配置版本固定 |
| Identity | 刷新并发、撤销丢通知、未知键、分区 | 保持60秒总界与高风险拒绝，浏览器撤销SLO，秘密不泄露 |
| 清理/初始化 | 半建库、满盘、跨库部分失败、重复删除/恢复 | 不静默丢收据，不伪造全局删除完成，不以清理替代撤权 |

真实 PostgreSQL 验证 Repository/锁/COMMIT结果不明，不以 SQLite 或内存测试替代。SQLite验证Host/Adapter恢复。使用协议模拟平台与本机/跨主机隔离环境，不需要真实QQ或外部计费模型才能验证核心正确性；真实平台另做已有验收。

## 13. 实施顺序与文档门禁

1. 定义目标 proto、OpenAPI/AsyncAPI、错误/状态/权限及内部值类型，配当前目标 fixture 和跨语言检查。
2. 建立各服务关系模型/Repository/UoW、初始化与数据库真实集成验证；先实现 Identity 可信引导，再实现 Runtime 关键事务。
3. 实现 outbox/inbox、租约与受限SDK、Scheduler可靠交接、Model双阶段记录、Host/Adapter日志和fence。
4. 接 Control 目标管理接口、真实状态源与观测；删除不可达旧分支，不做兼容桥。
5. 在清洁初始化环境完成故障矩阵、跨主机信任、断网恢复和成对备份恢复，再做部署和真实平台验收。

所有实施遵守“唯一契约→模块说明→实现与测试”。必须同步的模块文档为 doc/01-contracts、01-runtime-semantics、02-core、03-plugin-sdk、04-nonebot-adapter、05-infrastructure、06-control-api、07-control-panel、08-operations，涉及工具链/CI再更新对应文档。当前不批量改写它们以冒充已落地。

## 14. 技术依据与本轮检查范围

锁和隔离机制参考 [PostgreSQL 隔离](https://www.postgresql.org/docs/18/transaction-iso.html)、[显式锁](https://www.postgresql.org/docs/18/explicit-locking.html)、[SELECT 领取语法](https://www.postgresql.org/docs/18/sql-select.html)。READ COMMITTED不能替代显式业务约束，SKIP LOCKED仅用于队列领取，不用于证明任意业务查询一致性。

本项目参数与业务原子组为设计决策；本文无可执行DDL或代码。检查只覆盖文档链接、锚点、状态/选择一致性、边界与故障路径审查；未执行构建、协议生成、数据库测试或压测，不能将故障矩阵写作已通过。
