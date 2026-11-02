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
| Model（P11a 已实现，schema v8；复用扩展后的 model_intents 作为调用记录） | model_configs、encrypted_provider_secrets、model_intents（P11 调用事实） | model 别名/版本；调用 owner+logical_call_id 唯一，不存原始秘密于审计 |
| Host SQLite | command_journal、managed_executions、execution_observations | command_id 唯一；实例/执行代次唯一；受控命令 hash 防同键异内容 |
| Adapter SQLite | event_outbox、platform_sequences、action_execution_journal、platform_id_mappings | 未确认事件原 ID 稳定；action_id+规范化请求 hash；序号按平台会话四元组 |
| 各业务服务 | command_receipts、operations、audit_entries、outbox、inbox、schema_versions | 调用方/方法/资源/key 唯一；outbox source+message 唯一；inbox source+message 唯一 |

跨库只保存类型化稳定引用，不创建跨库 FK、共享视图或外部表。单库内 FK 与删除限制防误删；涉及收据、审计、未决动作不级联清空。Control 持有可重建 UI 技术状态，不再维护业务配置权威。

新资源 ID 用 UUIDv4，旧平台原始 ID 作为平台引用。不需要旧开发 ID 映射/导入；四元组顺序仍区分适配器、账号、会话 ID 和会话类型，内部稳定账号与平台账号由当前绑定显式解析，不能合并群/私聊。

P06 事件/投递表列定稿（Runtime schema 在 P05 资源表之后合并为 v9：A1 移除 adapter_bindings→adapters 外键并保留 account 外键；空库初始化，无迁移）：

- `events(adapter,event_id)` 主键；`fingerprint bytea(32)`、`conversation_key`（长度前缀四元组编码，存储内部键）、`payload_kind`、`has_conversation`、`sequence kiarabot.uint64`/`has_sequence`（两者同存同缺）、`serialized_envelope bytea<=1MiB`、`occurred_at`、`retain_until`、`state`（0 open / 1 terminal）、`received_at`；保留索引 `(state,retain_until)`。
- `conversation_watermarks(conversation_key)` 主键；`has_sequence`/`last_sequence` 同存同缺、`degraded` sticky、`updated_at`。按会话键而不是四列主键，避免拼接歧义。
- `delivery_targets(adapter,event_id,instance)` 主键；`ordered`、`trace_id`、`state`（0 persisted/1 retrying/2 dispatched/3 acknowledged/4 dead_letter）、`attempts`、`next_attempt_at`、`current_attempt`、`dead_letter_reason`、`updated_at`、`retain_until`、`ordinal bigint GENERATED ALWAYS AS IDENTITY UNIQUE`（有序组稳定顺序）；FK 指向 events 且事件删除级联；到期索引 `(instance,state,next_attempt_at)` 与 `(state,retain_until)`。
- `delivery_attempts(attempt_id)` 主键；event+instance 引用目标并级联，保存 `runtime_epoch`/`execution_id`/`execution_generation`/`connection_id`/`connection_generation` 完整 fence、`started_at`/`lease_until`（后者>前者）、`attempt_number`、`state`（0 active/1 acknowledged/2 cancelled/3 expired）；`(event,instance,attempt_number)` 唯一。
- `ordered_delivery_heads(instance,conversation_key)` 主键；`head_ordinal` 可为 NULL（组内无 pending），新目标到达时仅在原 head 为 NULL 时填充，ACK/死信后推进到下一非终态 ordinal。

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

P07 目标契约把上述规则落到 wire（2026-11-02 冻结）；P07a-A C3 已实现 typed PG 仓储：提交事务写 `actions/action_idempotency/quota_windows/daily_budgets`（窗口持久化，不因重启重置；配额与 Action 行同 UoW，取消/失败/到期不退款）；派发用单行条件 UPDATE 记录 `dispatch_attempt_id/dispatch_sequence` 后才写 Adapter 流；适配器结果走 `record_terminal` 条件更新，重复相同结果幂等、异结果 CONFLICT 且追加 `adapter_result_evidence`、迟到结果在 EXPIRED/CANCELLED 时只追加证据不覆盖终态；`expire_due` 用 `FOR UPDATE SKIP LOCKED` 单次条件 UPDATE claim，`prune_terminal` 只删除已到期终态行（CASCADE 清理幂等/窗口/证据）；Control 查询必须显式账号 scope + `action.inspect`（`list_control` 只按 account_id 过滤，不越权）。P07a-C 验收验证了上述事务顺序在真实 PG16.4 + Runtime 进程下成立：Action 行先于 quota_windows 插入、配额拒绝回滚、派发条件更新与帧写出分离、terminal/evidence 竞争与重连补报均不破坏终态。P07a-B 应用接线补充：Action 行必须先于 `quota_windows` 插入（后者外键引用 actions.action_id），配额准入失败回滚同一 UoW 的 Action 行；提交 Unknown 只在新 UoW 按 `(owner_instance, idempotency_key)`/action_id 复核原记录，不自动重试写。派发 claim 的 `list_due` 在 UoW 内 `FOR UPDATE SKIP LOCKED` 选中已受理未派发且未过期行，逐行条件 `mark_dispatched` 后同事务提交；动作帧在提交后由 Bridge 写出，写失败只导致该 Action 保持 UNKNOWN/EXPIRED，绝不二次派发。适配器结果/证据各走自己的 UoW，终端结果条件更新与证据追加同事务。
全部时间谓词在 SQL 内以数据库 UTC 重核；写入和配额准入都在调用方 Runtime UoW 内，unique 冲突/容量/依赖故障分别映射 CONFLICT/RATE_LIMITED/UPSTREAM_UNAVAILABLE。审计目标准列 target_account_id/target_conversation_id 由 P07 application 在写 action 同事务的 journal 审计中填充（C3 只验证表列与仓储边界，application 接线属 P07a-B/P07b）。真 PG16.4 矩阵见 `p07_postgres_action_records`。

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

P08 目标把该规则细化为（契约冻结，未实现）：Runtime PG 保存 L1 期望（`plugin_instances`+outbox/version/审计）与 L3/L4 投影（`HostRegistration`/`InstanceConfiguration`，`applied_version/apply_state` 持久、`delivery_id` 易失）；Host 本地 SQLite `user_version=1` 三表 `command_journal`（`command_id` 主键、目标代次、payload hash、state/attempts/retain_until，按实例代次与 state 建索引）、`managed_executions`（`(instance_id, execution_generation)` 主键、pid/pgid/marker/spawn_result/exit 信息/保留期）、`execution_observations`（`observation_id=<host_id>:<connection_generation>:<sequence>` 主键、command_id、payload hash、reported/acked 状态；待上报索引）。收据/观察保留七天；Unknown/未决不静默删除，超 30 天转人工处置标记（§169 规则）。恢复：加载 `Persisted/Executing` 命令→比对 marker→存活认领 Running 并补观察；已 spawn 但 marker 缺失记 `Exited(unknown)` 观察；无法确定转 Unknown 并拒绝同代次再次 spawn，等 Runtime 提升 generation。崩溃于“spawn 后、receipt 写前”时启动回读 journal 重建确定 receipt，不重复 spawn。

P09/P10 目标表口径（契约冻结，未实现）：Runtime PG 新增 namespaces/namespace_members/namespace_values/namespace_quota_counters 四表，锁序 namespace 行→成员/版本→namespace 配额→installation 聚合配额→key 行，READ COMMITTED 下用条件写/行锁；写收据复用通用 command_receipts（*storage_receipts* 在 doc/22:31 为逻辑别名，不建新表），容量同事务计数。**P09a 阶段 1/2 状态**：阶段 1 已落 domain/ports/`RuntimeUnitOfWork` 访问器与纯 application/单测；阶段 2 已落上述四表加 `namespace_receipt_results` typed replay 载荷表（Runtime schema v11，真 PG16.4 实测 descriptor=09c7176f032043f3bc7ba18f8c87b281、relation_count=134），`namespace_values` 条件写/DB UTC 惰性过期清理与计数调整、quota 两个计数器按固定锁序条件准入、`command_receipts` typed 存储/管理收据（同键同 fingerprint 重放原快照不续 TTL，容量按实例/安装计数且不淘汰有效行）与真 PG 集成矩阵；事务组仍为调用方单个 Runtime UoW，READ COMMITTED 条件写不依赖隔离级别。共享 namespace 管理 RPC 已适配；P09b 另把 PluginBridge `HostRequest` storage 帧接入同一 `RuntimeStorageApplication`：bridge 会话负责认证与 ConnectionAuthorization/注册 profile 授权，应用层仍以 `StorageCommandContext.bridge_authorization` 校验绑定后在同一 Runtime UoW 内执行成员/禁用/配额/收据，错误映射为稳定 wire 码；HTTP 操作面仍未接线。Scheduler 既有 tasks/occurrences 关系表增列（tasks：created_at/updated_at/cancel_requested；occurrences：attempts/last_error_code/created_at/updated_at）并新增 scheduler_quota_counters 与 occurrences_due/tasks_owner_state 索引；trigger_outbox/trigger_receipts 为通用 outbox/inbox/command_receipts 的逻辑别名，Scheduler 用 source=scheduler,message=occurrence_id enqueue，Runtime inbox 同键去重。**P10a/b 状态（2026-11-02，P10f 基线 d508d67）**：上述 Scheduler 增列/表/索引与条件写 claim 已实现并经 `p10_postgres_scheduler` 真 PG16.4 fixture 实测；Runtime AcceptTimerTrigger 以 inbox(source=scheduler,message=occurrence_id) 去重后持久产生 TimerTriggered，Scheduler 收到持久确认后同事务标 COMPLETED，三进程真 mTLS `p10_timer_e2e` 覆盖交接、崩溃恢复、取消/到期竞争与确认丢失恢复。target override 尚未登记（待 P10g），真实 7 天 EXPIRED/多 installation 容量/真实平台触发/生产委托来源仍未验收；证据矩阵见 [architecture-p10-scheduler.md](../reports/modules/architecture-p10-scheduler.md)。各阶段独立 commit 顺序 bump schema/descriptor（P03→P05→P06→P07→P09→P10），common.hpp snapshot 列定义同步更新。

P11a 已实现（契约冻结）：Model 调用为两次事务——txn1 在线授权/解析 model/并发 admission/服务端 request_hash，写 `model_calls`(owner,logical_call_id,state=STARTED,secret/config 版本,request_hash,may_have_billed=false,retain≥7d)+audit 后 commit；txn2 在 Provider 调用后写 SUCCEEDED/KNOWN 或 FAILED+明确 4xx/429，5xx/TLS/超时/畸形响应写 BLOCKED+UNKNOWN+may_have_billed=true，不得写成未调用/未计费。commit=Unknown 不回滚 txn1；未决 UNKNOWN 30 天内不得清理，只能经 `ResolveModelCall`（expected_version CAS、幂等）转 FAILED 终态，处置后保留 ≥7 天防重窗口。P11a 实现：Model 私有表 `model_configs`/`encrypted_provider_secrets` + 原 `model_intents` 就地扩展调用事实（不新建独立 model_calls，避免删除 P03 证据关系；schema v8、四服务 descriptor/relation_count 按真实 PG16.4 实测）；根密钥部署注入、不入库，秘密 XChaCha20-Poly1305 AEAD 只在 Model 进程解密；管理命令复用 command_receipts/operations/audit，Chat 不造 operation。P11e 以真实 TLS PluginBridge fixture 验证 `HostRequest.model_chat` 的 target wire 与 logical_call_id/UNKNOWN/429/超时/撤销委托语义（HostResponse 错误码与 retryable），streaming 不做。HTTP Provider 已由 P11b 接入（4xx/429 已知失败不记 billed；5xx/超时/畸形/超大响应保持 UNKNOWN 且 may_have_billed 保守；不自动重试）。P11c 已接入 Model 调用者校验（delegation 绑定实际 mTLS peer + 每操作 Identity.CheckAuthorization；Runtime 二跳=Chat，Control 一跳=管理面）与 Runtime→Model/PluginBridge ingress（固定 logical_call_id、UNKNOWN=QUERY_SAME_INTENT、稳定错误）；REST 与 Control 管理命令由 P11d 接线。
P11a 已实现（契约冻结）：Model 调用为两次事务——txn1 在线授权/解析 model/并发 admission/服务端 request_hash，写 `model_calls`(owner,logical_call_id,state=STARTED,secret/config 版本,request_hash,may_have_billed=false,retain≥7d)+audit 后 commit；txn2 在 Provider 调用后写 SUCCEEDED/KNOWN 或 FAILED+明确 4xx/429，5xx/TLS/超时/畸形响应写 BLOCKED+UNKNOWN+may_have_billed=true，不得写成未调用/未计费。commit=Unknown 不回滚 txn1；未决 UNKNOWN 30 天内不得清理，只能经 `ResolveModelCall`（expected_version CAS、幂等）转 FAILED 终态，处置后保留 ≥7 天防重窗口。P11a 实现：Model 私有表 `model_configs`/`encrypted_provider_secrets` + 原 `model_intents` 就地扩展调用事实（不新建独立 model_calls，避免删除 P03 证据关系；schema v8、四服务 descriptor/relation_count 按真实 PG16.4 实测）；根密钥部署注入、不入库，秘密 XChaCha20-Poly1305 AEAD 只在 Model 进程解密；管理命令复用 command_receipts/operations/audit，Chat 不造 operation。HTTP Provider 已由 P11b 接入（4xx/429 已知失败不记 billed；5xx/超时/畸形/超大响应保持 UNKNOWN 且 may_have_billed 保守；不自动重试）。P11c 已接入 Model 调用者校验（delegation 绑定实际 mTLS peer + 每操作 Identity.CheckAuthorization；Runtime 二跳=Chat，Control 一跳=管理面）与 Runtime→Model/PluginBridge ingress（固定 logical_call_id、UNKNOWN=QUERY_SAME_INTENT、稳定错误）。P11d 已接线 Control 管理 REST：每次 Update/Disable/Enable/Rotate/GetConfiguration/GetCallStatus/Resolve/GetOperation 先为 audience=model + 单一 RESOURCE_MODEL scope 发一次性 Identity 委托，再转发精确 owner RPC；Control 不做本地授权/事务，不读回密钥，UNKNOWN 查询超时不断言执行失败，target 消息/字段/语义已按证据翻转，REST operations 仍单独登记 not_implemented。

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

## P10a Scheduler schema 版本（2026-11-02）

Scheduler 私有库 schema 在合并 P07a/P11a 后版本为 9（P10a 第一阶段曾以 v7/v8 做增量标记）。
合并 schema 同时含 P07a 共享 audit 目标列、P11a Model 表/扩展与 P10a tasks/occurrences 观察列、
scheduler_quota_counters 关系及 due/owner 索引；record codec 同步 P10a 字段映射。由于
`result_*` snapshot 表由 `common_sql()` 为所有服务创建，四服务的 schema descriptor 都随合并
变化；Identity/Runtime/Scheduler/Model descriptor 与 relation_count（129/120/77/71）均在真实
PG16.4 上重新实测写回 `manifest.hpp`，不预写猜测值。`database_test`/initializer 对 descriptor、
关系计数和权限的校验已通过。后续任何关系变更仍必须同时更新 descriptor、relation_count、
record codec 与测试，不得只改代码或版本号。

## P10b TimerTrigger 交接事务（2026-11-02）

Runtime 保存时在单个 Runtime UoW 内插入事件（`events`，event_id=occurrence_id）与唯一 owner
instance 的 delivery target；重复交接依赖事件 id+fingerprint 的既有唯一/比较语义返回 duplicate，
不会重复写 target。Scheduler 不把 Runtime 的 UoW 拉入本地事务：Scheduler 先持久化 outbox
claim（通用 outbox），在事务外调用 Runtime.AcceptTimerTrigger，成功后再用独立 UoW confirm
occurrence/task（COMPLETED）。因此存在“Runtime 已入队但 Scheduler confirm 丢失”的窗口：
claim 到期后以同一 occurrence/event id 重发，Runtime 返回 duplicate，Scheduler 正常 confirm；
不会产生第二个事件或 target。Runtime 永久拒绝时 Scheduler 将 occurrence/task 置 FAILED 并
confirm outbox 以结束该交接；UNAVAILABLE/超时/限流保留 claim 重试。Scheduler→Runtime 请求由
部署配置 `runtime_endpoint` 提供，使用服务 TLS mTLS；授权链/真实 e2e 仍需后续增量验证。

## P10b-2 平台交接与 Scheduler 授权事务补充（2026-11-02）

Scheduler→Runtime `AcceptTimerTrigger` 的平台服务主体路径不写 Runtime 授权事务以外的资源：
Runtime 在一个 UoW 内持久化 TimerTriggered 事件与 owner target，平台 peer 身份只由边界
mTLS 叶证书 SAN 建立（Identity platform_peers 登记/assignment 是部署前置条件），handoff
完整性字段仍由应用校验；该路径不消耗用户 delegation，也不写入用户授权版本。Scheduler 的
RPC 授权要求 delegation 的权限绑定包含 Installation resource scope（发放方契约），
CheckAuthorization 通过 `scope_kinds` 注释确定方法 scope 种类（Scheduler 方法声明 TASK+
INSTANCE，无新权限）。多进程 e2e 验证了创建→到期→交接→COMPLETED、重复交接、过期 TIMEOUT
与撤销授权拒绝；容量/EXPIRED/崩溃恢复矩阵未覆盖。

## P13a 生命周期事务与保留清理（实现记录，2026-11-02）

删除/恢复沿用“所有者驱动、本地事务、无跨库级联”：`BeginDelete` 的资源状态/operation/participants/tombstone/receipt/audit 同 Runtime UoW 提交，跨服务参与者（Host 停止、Scheduler 任务取消）不在 UoW 内调用，由维护循环在提交后按 command/receipt 幂等推进；依赖离线记 `BLOCKED` 并可重试，不把超时当 stopped。恢复只允许 30 天窗口内的 `DELETED` 资源，不复活凭据/任务/租约；超过 180 天转 `PURGED` 并清墓碑。清理使用索引化有界扫描（`state,retain_until`、`occurred_at`、`valid_until`），每 60 秒一批；receipts/inbox/outbox/audit/operations/storage receipts 各自保留期，未决义务不清理，清理失败仅 `cleanup_failed` 告警不撤销删除 gate。COMMIT 结果未知仍按原幂等键读取，不假定回滚。P08b-3/P10b 真实参与者客户端接入前，Host/Scheduler 参与者在真实环境保持 BLOCKED（fake 端口单测覆盖成功/阻塞/恢复路径）。 **P08b-3（部分）**：Runtime 新增 `host_commands` outbox + `host_observations` inbox（schema v14）；P05 实例 desired 变更与命令 outbox 同 UoW 提交，命令快照含 execution_generation/desired/package/profile/payload hash，重复 command 幂等、同 id 异 payload CONFLICT；命令是否下发/回执属于传输状态，未确认时保持 Pending/Unknown，不因超时写 stopped。HostBridge 服务端下发与 receipt/observation 持久化接线仍未完成，故 P08 事务闭环未验收。
删除/恢复沿用“所有者驱动、本地事务、无跨库级联”：`BeginDelete` 的资源状态/operation/participants/tombstone/receipt/audit 同 Runtime UoW 提交，跨服务参与者（Host 停止、Scheduler 任务取消）不在 UoW 内调用，由维护循环在提交后按 command/receipt 幂等推进；依赖离线记 `BLOCKED` 并可重试，不把超时当 stopped。恢复只允许 30 天窗口内的 `DELETED` 资源，不复活凭据/任务/租约；超过 180 天转 `PURGED` 并清墓碑。清理使用索引化有界扫描（`state,retain_until`、`occurred_at`、`valid_until`），每 60 秒一批；receipts/inbox/outbox/audit/operations/storage receipts 各自保留期，未决义务不清理，清理失败仅 `cleanup_failed` 告警不撤销删除 gate。COMMIT 结果未知仍按原幂等键读取，不假定回滚。P08b-3/P10b 真实参与者客户端接入前，Host/Scheduler 参与者在真实环境保持 BLOCKED（fake 端口单测覆盖成功/阻塞/恢复路径）。 **P08b-3/3a（已实现传输闭环）**：Runtime 新增 `host_commands` outbox + `host_observations` inbox（schema v16，含 installation 快照）；P05 实例 desired 变更与命令 outbox 同 UoW 提交，重复 command 幂等、同 id 异 payload CONFLICT；HostBridge 服务端在注册/心跳后按当前 generation 读取 pending 并下发，写成功后才标记 dispatched；receipt 用 (command_id,connection_generation) 条件更新，旧 generation 拒绝；observation 按 (host,connection_generation,sequence) 去重落 inbox，重复/resync 帧幂等；30s 未回执转 Unknown，7d/30d 保留清理在 60s 批次。P08b-3b 已用真 PG+真 Runtime+fake Host 验证 transport/PG 闭环；真实 Host 二进制 e2e 与 bootstrap 自动预置仍未完成，P08 未退出。
## P10b-3 残余矩阵与 Installation-scope 复用结论（2026-11-02）

容量、EXPIRED、Scheduler 崩溃恢复、confirm 丢失/重复、撤销授权后 maintenance 已纳入
`p10_timer_e2e`（真 PG16.4/mTLS 三进程）。EXPIRED 通过在 owned DB 内回拨 occurrence handoff
窗口验证，无需等待真实 7 天；崩溃恢复依赖 outbox claim lease 与重启后 maintenance 收敛，
UNKNOWN 不伪完成。容量用 owned 表直接填充到 1024/8192 后经 RPC 断言 RATE_LIMITED。
Installation-scope 签发约定只被 Scheduler 消费；Model/Action 当前不读取 installation，登记为
P11d/P07 后续（届时按同一 fail-closed 规则补充派生与测试，不改 proto）。

## P13b Host/SDK/Adapter 优雅停止与恢复顺序（实现记录，2026-11-02）

Host 本地 journal 是进程事实的唯一本地权威，不冒充 Runtime 业务账本。优雅停止顺序固定为：停止接受新命令 → 活动 execution 有界 TERM（grace≤10s，TERM→KILL）→ flush 最终 observation → 有界 prune → close journal → 停 HostBridge；写盘与观察在同一次锁内完成，关闭后不再接受命令。无法确认的终止保留 UNKNOWN，重启恢复按 instance+execution_generation/marker 判断，不重复 spawn、不把 UNKNOWN 写成成功。SDK 的 ShutdownNotice 不丢已受理事件：在 drain 窗口内继续处理回执，窗口结束后停止但不重连；Adapter 未确认事件保留本地 outbox 并在窗口后按退避重连（Runtime 新代次重注册）。跨服务故障矩阵由 p13_host_sdk_drain（真实 Host+SDK）与 p08 单测覆盖；P08b-3a Host 命令下发接好后，Runtime BeginDelete 的 Host 参与者可复用同一 drain 语义。
## P12e 只读审计/日志投影事务边界（2026-11-02）

P12e 不新增表、不改变业务事务写入：Identity/Runtime 的 `audit_entries` 是各自权威事实，仍由原
业务 UoW 在同事务提交。新增的 `ListAudit`/`ListLogs` 只在独立的短只读 UoW（2s 预算）内执行
keyset 查询，提交/回滚不产生写、审计或 outbox；查询失败不接触业务事务。日志是审计事实的
脱敏投影，不复制到 Control、不延长保留期：审计最小 180 天、运维日志默认 7 天仍由各 owner
保留策略决定，Control 只投影授权且已存在的行。

## P13c 生命周期 participant 与 target 登记（实现记录，2026-11-02）

P13a 的 Host 参与者端口现接到 P08b-3 Runtime host_command outbox：维护轮次在 UoW 外调用 participant，participant 自己开短 UoW 写/读取 StopExecution 命令（确定性 command_id，重放幂等），只把 `receipt_persisted && receipt_outcome∈{KnownSucceeded,KnownFailed}` 当 Done；Pending/Dispatched/Unknown/拒绝保持 blocked，避免把 Host 未确认状态写成 stopped。P08b-3b 下发/回执接通后无需改 Runtime 应用层即可收敛。Scheduler 参与者仍通过实例委托的 ListTasks/CancelTask 端口预留，真实 delegation/endpoint 未配置时 BLOCKED。target 登记按证据翻转 Runtime 生命周期 RPC 与 Operation/OperationProgress/OperationState；未覆盖的 Host/Scheduler 真实完成、Control REST delete/restore 保持 not_implemented。

## P08d bootstrap 凭据文件与 Delete 命令快照（2026-11-02）

受管 bootstrap 明文只出现在 Runtime CreateInstance/BindAdapter 的一次性响应；部署 helper
在 Host 侧原子写 `credential_directory/<instance_id>.bootstrap`（0600），不属于数据库事务，
也不进入 Runtime 存储/outbox/audit。Delete 生命周期参与者在同一 UoW 写 host_command
StopExecution 快照（command_id 确定性、installation/package/desired state、Pending），
receipt/observation 仍由 HostBridge 后续事务推进；Runtime 重启只重放同一快照，不新增命令，
无已知 receipt 时保持 BLOCKED/UNKNOWN，绝不把文件清理或命令缺失当作 stopped。

## P13d 清理批次失败与观测边界（2026-11-02）

60 秒保留批次在同一次 owner UoW 内执行多个 prune（receipts、journal、outbox、audit、host_commands、operations、storage receipts）：任一 prune 返回错误只置 `LifecycleMaintenance.cleanup_failed=true` 并让整轮提交（计数/其他 prune 成功项保留），不删除未决/未终态记录；Runtime dependency hook 不因单次清理失败退出，仅在日志写 `operation=lifecycle_maintenance outcome=degraded cleanup_failed=true` 供告警检索。operation 的 blocked/UNKNOWN 仍以 GetOperation participants 为准，清理失败不会把 operation 改成成功；真实满盘/长时积压需 P14c 环境手动注入验证，应用测试用故障 prune 覆盖代码路径。

## P09f storage 事务/证据边界（2026-11-02）

Runtime storage 以 owner 私有 PG 四表与 `command_receipts` 同 UoW 实现：私有 namespace 惰性建行、条件写 CAS、TTL 过期同事务调整 namespace/installation 计数器、配额准入按 namespace→installation 固定顺序条件更新、共享 namespace/成员 gate 与收据重放（同键同 fingerprint 回原快照，不续 TTL）。真 PG16.4 与真 TLS bridge 矩阵已覆盖上述路径（p09_postgres_storage_records、p06_bridge_acceptance）。边界：unary storage RPC 仅 internal_dispatcher，对外 storage 面为 PluginBridge 帧；多 writer 配额横压、跨连接补报专项与共享成员删除不误删他人数据的直接集成断言尚未实测，需后续 P09/P13 交叉测试；target 未翻转项不因本证据自动改变。
