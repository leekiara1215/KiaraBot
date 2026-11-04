> 执行范围：本文原v1闭环和历史限制是保留的reference断言来源，不能据此宣称六服务target已接线。当前target生产可用性以proto/tests/contract_status.json的execution_scopes.target为准；P02仅进程探针可用，业务RPC仍明确UNIMPLEMENTED。本文标为目标的租约、收据、交接和恢复语义是后续实现必须满足的规范，不是已通过生产验收。

# 事件与 Action 运行时语义

T07a 闭环回归发现派发事务与 ACK 竞争：Bridge 的事件/Action 发送循环遇到 retryable=true 的 CONFLICT 时等待 20ms 后重新读取并尝试派发，不断开流；此时尚未向流写出该帧。非可重试冲突及其他存储故障仍走原错误/断线恢复路径。此重试仅针对 Core 内部未提交的领取，不重发插件业务请求或模型调用。

T06b 模型调用在网络调用前后分别提交 STARTED/结果审计，网络期间不持有数据库事务。崩溃可能仅留下 STARTED，响应丢失也可能已计费；模型请求不提供幂等回放且 SDK/Provider 都不自动重试。结果审计故障返回不可重试存储错误；可通过 request_id/trace_id 关联两条审计，审计不保存对话、密钥或供应商原始错误。

T03b SDK 回调成功才发正 ACK，异常发 INTERNAL 负 ACK，协作式超时发可重试 TIMEOUT。错误不携带插件异常正文。传输中断和未发出的 ACK 保持未知，重连重新注册并允许重复事件；插件负责业务幂等。本地初始化完成后才注册；停止断开流后执行清理，强制终止由 Core 宿主兜底。

T03a 将逐插件状态机接入 PluginBridge。订阅来自已授权活动连接的 capabilities；在事件事务中确定目标，不事后追加。每插件当前最多一个在途事件，ACK 只改变本插件记录。断线处理先取消并 join 旧发送线程，再将 DISPATCHED 转入重试；旧连接未停止时拒绝新注册。传输故障不覆盖已提交 ACK，重连恢复也不重复确认。重放使用投递记录的 trace_id 覆盖序列化事件中的旧 trace。活动订阅在重连时重新声明，不修改既有投递目标。

字段定义在 `proto/kiarabot/v1/`，本文件冻结传递、重试、顺序和确认规则。

## 当前 reference runtime

`core/include/kiarabot/core/application/runtime.h` 提供与本语义对应的可替换状态机。`RuntimeStore` 和 `RuntimeTransaction` 是 Core 持久化端口，保存 inbox、会话顺序状态、Action 和插件状态；`InMemoryRuntimeStore` 只用于测试和本地开发。Core 运行时不直接访问数据库，基础设施实现该端口后，Core 重启仍可恢复去重、会话状态、重试、死信和 Action 幂等状态。端口实现须保证事务内写入原子提交，失败时回滚，且按事件 ID 和 Action 幂等键提供唯一性。当前参考 Runtime 及内存存储由调用方串行访问；并发访问需由宿主或存储实现提供隔离。

接收事件和提交 Action 的内部结果分别区分重复、输入拒绝、背压或过期及存储失败；事务内状态机插入结果区分唯一键冲突，外层 StorageResult 区分存储失败，存储失败不得伪装成重复。事件及 Action 的检查和写入在同一事务执行；提交返回失败时端口必须保证可回滚且不会留下已提交写入。内存端口允许独立快照，但拒绝旧 revision 的重叠提交。T05a 后公共批量操作整体提交；外层 StorageError 表示整次提交失败，不能将内部计数当作已持久化数量。跨进程边界由服务层按现有协议映射错误，不新增协议错误码。时钟和范围为 `[0, 1]` 的随机样本可注入；默认使用系统时钟和随机源。
持久化时间点使用 UTC 系统时钟而非进程单调时钟；系统时间发生调整时，已保存的绝对到期时间不变。

每个运行时操作都必须通过未来的日志端口记录 `operation`、`outcome` 以及相关事件、Action 或插件标识。日志不会替代状态存储，也不会承载业务命令。日志实现可以评估腾讯 [BqLog](https://github.com/Tencent/BqLog)，但当前 Core 不直接依赖它。

## 事件投递

- Core 与适配器使用至少一次投递；
- 适配器只有在 Core 持久化 inbox 记录后才认为事件已确认；
- inbox 保存传入的序列化 `EventEnvelope` 字节，Core 将其视为不透明数据；恢复和重放保留这些字节，由传输层在投递时解码并使用记录中的当前 trace ID；
- 事件可能重复，插件必须用 `event_id` 幂等；
- Core 默认保留已确认事件和幂等键 7 天，配置不得低于 24 小时；
- 过期事件不自动重放，人工重放生成新的 `trace_id`。

## 顺序和背压

- 不提供全局顺序；
- 同一会话的排序域 `(adapter_id, account_id, conversation_id, conversation_type)` 内按 `sequence` 单调递增；`conversation_type` 取自消息载荷 `ConversationRef.type`，没有会话载荷的事件（如 `AdapterStatusChanged`）取 `TYPE_UNSPECIFIED`，因此群号与私聊号相同时两个会话各自独立；
- 会话键是上述四元组的带长度前缀编码，只存在于 Core 与持久化快照中，不是线协议字段；它同时决定会话水位和逐插件有序投递的分组；
- 不同会话可以并行；
- 缺少 sequence 或跳号只影响该会话，并报告 `DEGRADED`；
- 跳号事件仍可接收并推进该会话的最大 sequence；较小或相同的非重复 sequence 被拒绝。会话的最大 sequence 和 degraded 状态独立保存，不因 inbox 保留期清理而丢失；
- 插件声明 `ordered=true` 时，Core 对该插件按会话串行投递；
- Core 在接收事件的同一事务中按当时的订阅快照为每个目标插件创建独立投递记录，重复事件不会追加目标；`ordered=true` 时，同一插件、同一非空会话的较早记录在 ACK 或死信之前阻塞后续记录，即使正在等待重试；不同插件及不同会话互不阻塞。缺少会话标识时无法建立会话顺序，宿主应提供稳定会话键；
- 队列达到上限时暂停读取非关键事件并返回 `RATE_LIMITED`。

  P06 target Runtime 的 bridge 事件入口在容量/存储背压和旧 sequence 时返回 `REJECTED`（`RATE_LIMITED`/`CONFLICT`、`retryable=true`、`retry_after_ms=1000`）并关闭本次流；适配器按“确认丢失 = 未知结果”处理：保留未确认事件，退避后重连重投，不能因不可重试标记丢弃事件。reference-era 的 `retryable=false`/不关流行为只作为历史说明。

## 确认和重试

```text
RECEIVED → PERSISTED → DISPATCHED → ACKED
                         └────────→ RETRYING → DEAD_LETTER
```

- 插件 ACK 表示接受事件，不表示 Action 成功；
- 默认最多重试 5 次，延迟 1s、2s、4s、8s、16s，并在每次基础延迟上均匀加入 ±20% 抖动；第 5 次重试仍失败后进入死信；
- 非重试错误直接进入死信；
- 死信可查询和人工重放，重放保留事件 ID、重置重试次数并使用新的 trace ID；重放也受队列上限约束；
- 同一事件不会向同一插件并发投递。
- 逐插件投递以 `(event_id, plugin_id)` 标识，ACK、重试次数、下次尝试时间、死信和重放 trace ID 均各自持久化；同一事件其他插件的状态不受影响。事件只有在所有目标插件均 ACK 或进入死信后才成为终态并开始保留期清理。逐插件死信重放只重置选定插件的记录；
- 人工重放不能撤销已经完成的后续事件；有序插件重放时仍等待同会话中当前投递结束，避免并发；
- 未运行的插件不会获得新投递；断线后未确认的投递保留，宿主确认旧连接停止后调用逐插件恢复，将 `DISPATCHED` 记录转入退避或死信。ACK 的插件身份来自已认证连接；当前协议没有投递尝试编号，延迟 ACK 无法区分不同尝试，应按至少一次投递处理；
- 宿主确认旧投递者已停止后，在启动恢复阶段调用 `recover_inflight_events()`；残留的 `DISPATCHED` 记录按一次可重试失败进入退避或死信。`dispatchable_events()` 返回已持久化事件和到期重试，恢复和投递必须由同一串行宿主管理，避免对仍在运行的投递者重复投递。

## Action 语义

T06a Host 存储在同一 RuntimeTransaction 内完成权限判断、namespace 更新、成功写幂等响应和审计；提交失败整体回滚，读取故障不伪装 NOT_FOUND。幂等 fingerprint 忽略每次调用的 request_id/trace_id/deadline，保留 operation、键、值、过期时间和幂等键。重放返回原结果但使用本次 request_id；同键不同内容拒绝，不能通过重放刷新 TTL。过期键按访问/写入惰性清理，幂等记录保留七天。

T04a 插件 Action 使用 action.result.v1 显式协商和 request_id 关联。幂等键按认证调用方隔离，重连不改变作用域；内容冲突为 CONFLICT。按 ID 查询也必须校验调用方归属。查询错误和未知网络结果不更新 Action 状态；客户端重新注册后查询，不能因超时生成 FAILED。ACCEPTED 是持久化接收，不承诺平台已执行或自动推送终态。取消只在未派发前生效；派发后平台结果未知时返回 CONFLICT，不声称撤销了平台副作用。已到期优先 EXPIRED，已有终态不可覆盖。

- Action 必须带 `idempotency_key`；
- Core 保存 Action 结果 7 天，重复请求返回原结果；
- 内部提交结果包含已有 Action 的状态和结果；终态包括 `SUCCEEDED`、`FAILED`、`EXPIRED` 和 `CANCELLED`，终态记录从最后一次状态更新起保留，默认 7 天且不得少于 24 小时；清理只删除到期终态记录；
- 首次提交时 deadline 已到的 Action 也保存 `EXPIRED` 终态和幂等键；之后使用同一键的请求返回原记录，不会重新执行；
- 适配器可先返回 `ACCEPTED`，最终返回 `SUCCEEDED` 或 `FAILED`；
- 到达 `deadline` 后标记 `EXPIRED`，适配器不得再执行；
- 参考 Runtime 不自带调度线程：宿主须定期调用 `expire_actions()` 并在执行前检查状态和 deadline；
- 取消只能作用于尚未到期的 `ACCEPTED` Action；到期后的完成或取消不会覆盖 `EXPIRED`；
- 网络超时不等于平台执行失败，必须通过幂等键查询最终结果。
- Core 在受理可派发 Action 前按 `(Action 类型, 账号)` 与 `(Action 类型, 会话)` 检查进程内滑动窗口配额；默认单账号 10 次/分钟、单群 3 次/分钟、单用户私聊 1 次/10 秒，由组合根环境变量覆盖。超限返回可重试 `RATE_LIMITED` 并写入审计；重复幂等提交与 deadline 已过的提交不消耗配额。配额不是平台限频的替代：平台侧限频/风控错误仍由适配器分类上报。
- 危险 Action 可配置**持久化每日预算**：按 `(Action 类型, 账号)` 与 UTC 日计数，配置项 `KIARABOT_ACTION_DAILY_LIMITS`（`<action kind>=<limit>`）；超限返回可重试 `RATE_LIMITED` 并写审计，被拒绝的请求不消耗预算。计数器与 Action 记录同一快照事务提交，跨重启保留；维护循环删除过期日期的计数器。默认不配置预算（空），只为危险动作开启。
- Action 审计记录包含目标账号与会话（`target_account_id`、`target_conversation_id`）；控制面 `/audit` 对非空目标输出 `account_id`/`conversation_id` 字段。
- 查询接口返回 `ACCEPTED` 表示尚未终结；`SUCCEEDED`、`FAILED`、`EXPIRED`、`CANCELLED` 是终结状态。终结结果保留 7 天；查询超时不改变 Action 状态。按幂等键查询受已认证调用方的授权范围限制。

### ReactMessage（表情回应，W6）

`react_message` 在 v1 只支持群/私聊消息，`conversation` 必填且 `type` 只能是 `GROUP`/`PRIVATE`；`conversation.account_id` 必须等于 `target_source.account_id`，`target.conversation_id` 必须等于 `conversation.id`。`reaction` 使用平台中立 token：`like`、`laugh`、`surprise`、`sad`、`angry`，由适配器映射到平台表情 ID；Core 不写平台 emoji ID。

- 权限：需要 `message.react`，与 `message.send` 相互独立（`message.send` 不隐含 `message.react`）。适配器必须协商能力 `action.react_message.v1`，否则提交返回 `CAPABILITY_NOT_SUPPORTED`。
- 校验与错误：非法 ID/账号/会话不一致返回 `INVALID_ARGUMENT`；不支持的会话类型、未知或空 reaction token 分别返回 `CAPABILITY_NOT_SUPPORTED`/`INVALID_ARGUMENT`；缺少权限或账号未授权返回 `PERMISSION_DENIED`。
- 成功可以不返回平台消息 ID：适配器回传 `SUCCEEDED` 且 `platform_message_id` 为空是合法终态；不得因此伪造 `ACCEPTED`、视为失败或重试平台调用。
- 幂等、配额、deadline 与 `send_message` 相同；配额按 Action 类型使用独立滑动窗口。平台限频/风控拒绝由适配器分类为稳定错误码。

### DeleteMessage（撤回，W6c）

`delete_message` 在 v1 只允许撤回**本机器人账号自己发送过**的消息；`conversation` 必填且 `type` 只能是 `GROUP`/`PRIVATE`，账号与 `target.conversation_id` 校验与 `react_message` 相同。适配器维护自己的已发送消息索引（send 成功后按平台消息 ID 记录，保留七天），只有索引中且会话匹配的消息 ID 才能撤回；未索引或会话不匹配返回 `PERMISSION_DENIED`，不把平台消息 ID 当作可任意撤回的凭据。

- 权限：需要 `message.delete`，与 `message.send`/`message.react` 独立；适配器必须协商能力 `action.delete_message.v1`，否则提交返回 `CAPABILITY_NOT_SUPPORTED`。
- 校验与错误：非法 ID/账号/会话不一致返回 `INVALID_ARGUMENT`；不支持的会话类型返回 `CAPABILITY_NOT_SUPPORTED`；未索引/非本账号消息返回 `PERMISSION_DENIED`；平台明确拒绝按稳定码 `FAILED`。
- 成功可以不返回平台消息 ID：适配器回传 `SUCCEEDED` 且 `platform_message_id` 为空是合法终态。
- 幂等、配额与 deadline 与 `send_message` 相同；配额按 Action 类型使用独立窗口。撤回窗口由平台决定，适配器不因窗口外失败重试。

### MuteMember / KickMember（群管理，W6e）

`mute_member` 与 `kick_member` 在 v1 只支持群聊：payload 的 `group_id` 必须等于 `ActionEnvelope.target.conversation_id`，`user_id` 必须是平台数字 ID。`mute_member.duration_seconds` 取值 1–2592000（30 天），0 或超上限返回 `INVALID_ARGUMENT`；`kick_member.reason` 最长 256 字节，**只作为审计元数据**，OneBot V11 没有对应字段，适配器不下发。

- 权限与能力：分别需要 `member.mute` / `member.kick`，与 `message.send` 等互不隐含；适配器必须协商 `action.mute_member.v1` / `action.kick_member.v1`，否则提交返回 `CAPABILITY_NOT_SUPPORTED`。
- 默认门禁（Core 强制，fail closed）：组合根可配置 `KIARABOT_ACTION_ALLOWED_CONVERSATIONS`（`<action kind>=<会话>|<会话>...`）。某 kind 一旦出现在该配置中，只允许目标会话在白名单内的提交，其他一律 `PERMISSION_DENIED`；未出现的 kind 不受此配置限制，因此部署危险动作时必须显式配置白名单（见 `doc/08-operations.md`）。
- 配额：先检查进程内滑动窗口，再检查持久化每日预算（`KIARABOT_ACTION_DAILY_LIMITS`），超限统一返回可重试 `RATE_LIMITED`；被拒绝的提交不消耗每日预算。危险动作建议同时配置白名单、每日预算与速率上限。
- 适配器映射：mute → OneBot `set_group_ban`（`group_id`/`user_id`/`duration`）；kick → `set_group_kick`（`group_id`/`user_id`/`reject_add_request=false`）。成功可以不返回平台消息 ID；平台明确拒绝按稳定码回传 `FAILED`。
- 幂等、deadline 与审计与其他 Action 相同，审计记录目标账号/会话；平台限频/风控拒绝由适配器分类。

### FriendRequest 与 ApproveRequest（好友审批，W6f）

事件前置：唯一适配器把 OneBot V11 `post_type=request`、`request_type=friend` 映射为 `EventEnvelope.friend_request`，字段为 `user_id`、`comment`（≤256 字节）与 `request_id`。`request_id` 是适配器作用域的确定性安全 token（等于事件 ID），**不是平台 `flag`**；原始 `flag` 只保存在适配器本机 `requests` 索引（保留七天），不进入事件、插件或日志。事件 source 绑定当前账号，Core 走既有入库、去重与逐插件投递；插件须声明订阅 `event.friend.request` 且获授 `request.read`，否则注册阶段返回 `PERMISSION_DENIED`/`CAPABILITY_NOT_SUPPORTED`。

- 审批 Action：`approve_request` 需要 `request.approve`，适配器必须协商 `action.approve_request.v1`；`request_id` 必须回填事件中的 token，`approve` 表示通过/拒绝，`reason` 最长 256 字节且**只作为审计元数据**（OneBot `set_friend_add_request` 没有该字段）。
- 适配器解析 token 后调用 `set_friend_add_request`（`flag`、`approve`）；未知或过期 token 回传稳定码 `NOT_FOUND`（Action 失败），不猜测平台请求。成功可以不返回平台消息 ID。
- 幂等、deadline、配额与审计与其他 Action 相同；审批没有会话目标，因此不参与会话白名单，但账号滑动窗口与可配置每日预算（`action.approve_request.v1`）仍然生效。
- v1 只映射好友请求；群邀请/加群请求没有契约事件，仍不支持，不得用 `approve_request` 猜测审批对象。

### REST 查询与 Bridge 查询的保留/EXPIRED 语义（v1 状态）

Bridge 的 `ActionRouter::lookup` 是当前唯一实现的 Action 查询：按认证调用方作用域匹配 `action_id` 或幂等键；非 `ACCEPTED` 记录从最后一次状态更新起超过 168 小时返回 `NOT_FOUND`；`ACCEPTED` 且 `deadline <= now` 时先迁移为 `EXPIRED`（`error.code = TIMEOUT`）并提交审计，再返回结果；不存在/不可见/保留期外统一 `NOT_FOUND`。查询超时不改变状态。

REST 的 `GET /actions/{actionId}` 与 `GET /actions/by-idempotency-key` 必须读取同一份 Action 记录集并复用同一套 168 小时保留与到期 `EXPIRED` 规则，不得维护副本或另行定义保留期。v1 控制面查询尚未接线：Core 没有创建 `control/<user>` owner 记录的写入方，REST 分支也没有保留期/过期迁移，因此当前对所有真实 Action 返回 `NOT_FOUND`。该差异登记为 `semantic.rest.action_query_terminal_retention` / `semantic.rest.action_query_deadline_expired` = `not_implemented`；控制面接线时以本节作为契约测试断言，且不得把 `NOT_FOUND` 解释为平台执行失败。

## 插件重连

- 插件断开后进入 `UNAVAILABLE`，Core 暂停投递并保留未确认事件；
- 重新注册且 API 版本和权限一致后恢复；
- 断线时间从首次断线开始计，不因重复断线通知重置；达到 5 分钟仍未恢复则为 `FAILED`，由策略决定是否重启。
- 宿主须定期调用 `update_plugin_recovery()` 推进断线超时；Runtime 不自行启动后台线程。

## 优雅停机与 drain

v1 Core 关闭服务时取消等待中的流并等待处理器退出；两个 Bridge 都不会发送 `ShutdownNotice`（`CoreToAdapter.shutdown=5`、`CoreToPlugin.shutdown=8` 已定义，状态为 `not_implemented`）。客户端收到流结束、`CANCELLED`、EOF 或连接关闭时，一律视为**未知结果**：

- 不得推断事件未入库、插件 ACK 未生效或 Action 未执行；
- 重连后重新注册，按 `event_id` 幂等重投未确认事件、按认证调用方 + 幂等键查询未确认 Action；
- 正常运维重启与故障断开在 v1 不可区分，重试必须幂等。

将来 Core 在关闭前发送 `ShutdownNotice` 时，`drain_timeout_ms` 给出在途处理上限：客户端停止领取新工作，在上限内完成并确认在途请求，然后正常关闭。未收到该帧的客户端行为与 v1 相同；旧客户端必须忽略未知 oneof 成员。

## 控制面板流恢复

控制流使用 AsyncAPI 中的 `SESSION_READY` 会话 ID 和已处理的 `sequence` 恢复；服务端保留 10 分钟事件。服务端返回 `RESYNC_REQUIRED` 时，客户端丢弃旧会话，通过 `hello` 建立新会话并重新拉取 REST 快照。事件可能与快照重叠，客户端按事件触发刷新，不能将流视为业务状态的唯一来源。

## 插件任务和 Host 请求

调度只有一条受支持路径：`host.tasks.v1` + `task.schedule` 权限 + `event.timer.triggered` 订阅。旧的 `ActionEnvelope.schedule_task` 已 `deprecated`：它与 Host API 的时间语义、幂等和重试模型不等价（见 `doc/13`），v1 Core 与适配器继续以 `CAPABILITY_NOT_SUPPORTED` 拒绝；不得把它当作 `host.tasks.v1` 的别名实现，也不得用旧字段重新解释调度时间。

T06c：task.schedule 权限控制 task_create/get/cancel，功能名 host.tasks.v1；创建还要求插件声明 event.timer.triggered 订阅。同 owner/task_id 不同内容 CONFLICT，相同内容返回当前状态。Host 相同幂等键返回最初响应（查询当前状态使用 task_get），保留七天。创建和取消先校验 deadline，再在提交前复核；失败不修改任务/幂等记录。

Core 每 100ms 检查最多 32 个到期任务，领取原子置 RUNNING、增加 attempts 并生成随机 claim_token。完成事务验证 token，将唯一 event_id 的 TimerTriggered 和仅发给所有者的逐插件投递入队，再将任务置 COMPLETED；COMPLETED 表示已可靠入队，不表示插件回调已 ACK。任务的 payload、target_conversation 和原定时间进入事件，适配器不能伪造该事件。离线插件重连后按既有至少一次语义收取。

领取后入队失败重试间隔一秒，最多五次后 FAILED；写库失败保留 RUNNING，后续 tick 会重试相同 claim，进程重启 recovery 改为 RETRYING 并撤销旧 token。只能在上一调度进程停止后执行 recovery；不支持多个 Core 调度进程同时拥有同一数据库。旧 token 完成/重试返回 CONFLICT。PENDING/RETRYING 取消立即 CANCELLED；RUNNING 取消设置 cancel_requested，在尚未入队时阻止入队，完成收尾为 CANCELLED。已经 COMPLETED 的任务取消返回 CONFLICT，不能撤销已持久化事件；重复取消 CANCELLED 成功。取消后的旧领取不能创建第二条事件。

Host 请求按注册插件身份隔离。任务创建与取消使用幂等键；任务持久化成功后才返回成功。任务为一次性任务，启动时恢复 pending 和 retrying 状态；进程中断时 running 任务恢复为 retrying，投递至少一次，消费者按 task_id 幂等。每次领取生成新的 claim token；完成或重试只接受当前 token，旧进程的迟到结果不能覆盖新的领取。取消 pending 或 retrying 任务后不再投递；取消 running 任务只阻止后续重试，不撤销已开始的处理。终态任务不可重新运行。重试次数与退避由 Core 策略决定并随任务持久化。状态、取消和重试不得由插件直接修改数据库。

## 必须测试

重复、乱序、sequence 跳号、Core 重启后的持久化去重、插件重连、Action 超时、适配器断线、队列满、重复幂等键、指数退避、死信、保留期清理和结构化日志路径。

## 代码启动句

> 请只实现事件 inbox、event_id 去重、会话顺序、指数退避重试、死信、Action 幂等与插件重连状态机，使用 fake adapter/plugin 编写可重复的时间控制测试，不接入真实 QQ、数据库或模型。

## T05a 统一事务快照端口

当前公共 Runtime 通过 RuntimeStore::read/begin 和 RuntimeTransaction::snapshot/commit 访问基础设施。所有公共查询和写操作返回 StorageResult<T>（共享 Result<Value, Error> 即 std::variant）；成功的空 optional 表示不存在，StorageError 的 Unavailable/Conflict/Corrupt 明确表示依赖故障、并发冲突或数据损坏。边界服务将其分别映射到 UPSTREAM_UNAVAILABLE、CONFLICT、INTERNAL，不允许以空记录或 false 吞掉存储错误。Host PortResult 使用同一个 Result 约定及已有 Protobuf Error。

事务提供独立快照，销毁未提交事务丢弃修改；提交失败不改变持久状态。完整快照包括事件、逐插件投递、会话水位、Action、插件和投递序号。Runtime 单次调用以一个快照提交，不自动重试冲突；批量调用现在整体提交，不再部分持久化。调用方须检查外层 StorageError 后才能使用内层业务结果。公共 Runtime 串行访问；存储实现须保护不同事务之间的并发冲突。

原内存状态机移至 detail::RuntimeEngine，仅操作事务私有内存。其 bool/optional 不是数据库端口，故障由外层显式返回。原 ports.hpp 的不完整 IRuntimeRepository 移除，Host UnitOfWork 直接暴露同一 RuntimeSnapshot；Host 存储/审计组合事务仍待 T06 实现。完整快照是首版正确性实现，会复制全部状态，规模较大时应以同样事务语义优化，不能作为吞吐性能保证。

## T02a 适配器 inbox 入口

事件必须经认证注册后接收，source.adapter_id、account_id 和当前 adapter_instance_id 必须匹配连接；账号权限来自宿主允许列表。事件支持 MessageReceived、MessageSent、GroupMemberChanged、FriendRequest、AdapterStatusChanged，适配器不能伪造 TimerTriggered/PluginStatusChanged。消息的 conversation/account/subject 必须一致，事件必须含有效 occurred_at、非空 event_id/trace_id、schema_version=kiarabot.v1 和 AT_LEAST_ONCE。sequence=0 作为未提供处理并按原规则降级会话。

入口串行提交，使用带长度的四元组会话键防止拼接冲突；第四个分量是会话类型（无会话载荷事件取 `TYPE_UNSPECIFIED`）。重投的内容 fingerprint 使用确定性 Protobuf + SHA-256，忽略 source.adapter_instance_id（重连后须填新实例），其余内容应保持相同。同 event_id 不同内容返回 CONFLICT，不覆盖已存记录；重复确认不重复推进水位/审计/副作用。成功返回前必须提交，连接在提交后断开时，重投得到 DUPLICATE。当前命令幂等元数据保留到显式清理，尚未实现清理调度。

schema v6 迁移把已经持久化的三元组会话键重键为四元组：Core 从每个事件保存的 serialized envelope 重新推导类型；同一旧键同时覆盖群/私聊同号会话时，派生出的两个新键继承同一水位（该水位只会让 Core 接受继续的 sequence，不会错误拒绝）；没有保留事件的旧水位无法归因，被丢弃，升级后该会话首个事件以新水位建立并可能报告一次 `DEGRADED`；envelope 无法解码的事件保留旧键。适配器本地 outbox 的 sequence 键同步升级（其独立数据库 user_version 1→2，旧共享计数器保守复制到群/私聊两类），两侧独立迁移但排序域语义一致。

队列上限默认 1024，满时回复可重试 RATE_LIMITED 并关闭本次流，不再读取后续业务消息；未确认事件仍属于发送方，退避后重连重投。应用层不缓存额外事件；gRPC 单消息上限 1 MiB。新注册在同一入口锁内替换旧实例，存储 I/O 后释放锁再发送响应。启用 T03a 后按活动订阅快照同步创建逐插件投递；未配置插件入口时只入 inbox。

## 控制面实时刷新（T09a）

REST 不可变分页用于一致列表；WebSocket 是资源状态刷新通知，Core 投影每 100ms 比较，可合并中间状态。先建立流收到 SESSION_READY，再读取 REST；读取期间来的事件需要标记并在加载后刷新。已发布事件按流保留十分钟并按序重放，序号不跨流或重启延续。无法补齐明确 RESYNC_REQUIRED；客户端重新 hello 并全量刷新。状态通知不是审计和业务投递 ACK，不得用于计算平台副作用次数。

## 配置下发闭环

配置通过 plugin.config.v1 / PluginConfig / PluginConfigAck 下发并按连接确认。REST GET 的 delivery 区分持久化与运行实例应用，PUT/幂等重放只确认持久化。状态、超时、重连、错误及事件门控的完整规则见 [PluginSDK 配置下发闭环](03-plugin-sdk.md#配置下发闭环t08b-后续)。协议唯一来源为 proto/kiarabot/v1/plugins.proto，REST 为 api/openapi.yaml；无新增错误码或 HTTP/gRPC 映射。

## P01 目标注册、代次和租约（未接线）

目标使用一套协议，不接纳旧事件专用客户端。注册验证安装/实例/账号绑定、当前执行代次和必需特性；服务端事务分配 connection_generation 与 runtime_epoch，Fence 在每个注册后帧必填且正代次，旧连接不能执行新工作。Adapter 声明能力仅协商，不授予权限。重启更换 epoch，失效旧流。

每个实例 fan-out 独立 DeliveryAttempt：event_id 保持，attempt_id 每次领取改变，绑定完整 Fence；started_at/lease_until 由数据库 UTC 裁决。租期30秒，建议10秒续租，每次最多延至 now+30秒且不超过 started_at+300秒。Renew/ACK 和超时竞争同一持久记录；过期返回 DELIVERY_STALE，不修改新尝试；重复成功返回 DELIVERY_DUPLICATE。ACK 需 event_id 与 attempt.event_id 相等；错误 ACK 需稳定 Error。配置 ACK 绑定 Fence、delivery_id 与 config_version；未应用最新配置不投新事件。正常/拒绝/过期/依赖故障均可由 DeliveryReceipt 表达；ACK/Renew 是当前认证流的收尾事实，不重新检查历史用户会话。新副作用仍在线授权。

内部成功命令收据至少七天；可靠交接使用 source_service/message_id/资源版本/hash/created_at/valid_until，默认有效七天。inbox 保留到 max(首次接收+七天, valid_until+七天)。已过期新消息拒绝，不盲延长有效期；重复提交不新增投递目标。断线/取消等待不回滚提交，不将未知平台/模型执行记成 FAILED。

### P06 外部 Bridge 入站与投递尝试（target 已实现）

- **监听与认证**：外部 Adapter/Plugin 只连 Runtime 进程的可选 `bridge_listen`（server-verified TLS，不请求客户端证书），`authorization: Bearer <bootstrap_credential>` 恰好一个；bootstrap 是逐实例一次性交付、可撤销的登记凭据（Runtime 只存 SHA-256），不是业务授权。首帧注册校验资源/代次后返回 Fence+`ConnectionAuthorization`；后续每帧带完整 Fence 和当前 proof。`grpc_listen` 的平台 owner mTLS 监听不变；unary RuntimeService 的注册/事件/ACK/续租方法为 internal_dispatcher，不对外。
- **bootstrap 交付**：Adapter 由 `RuntimeBindAdapterResponse.bootstrap_credential`（字段 3）返回，externally_managed 插件实例由 `RuntimeCreateInstanceResponse.bootstrap_credential`（字段 2）返回；Host 受管实例不返回。调用方只交付一次，不持久化/记录明文；撤销随实例停用/解绑发生，proof/refresh 摘要只驻留 Runtime 连接状态，重启即失效。
- **投递尝试**：沿用 P01 租约规则。`delivery` 身份为 `(event_id, instance_id)`，`delivery_id` 字段取 `event_id`；`ReplayDelivery.expected_version` P06 必须 0/未设置；重放只重置目标实例、保留 event_id、新 trace/attempt。ACK/Renew 必须以当前 attempt+fence 条件写；当前 active 成功 ACK=`DELIVERY_ACKNOWLEDGED`，同 attempt 重复成功=`DELIVERY_DUPLICATE`，旧/过期 attempt=`DELIVERY_STALE`（不确认新尝试），错误 ACK=`DELIVERY_REJECTED`（retryable 决定退避或死信），Renew 成功/重发=`DELIVERY_RENEWED`。
- **A1/B1 目标语义**：外部自管 adapter 的 `BindAdapter` 可在首次 bridge 登记前建立，binding 是登记前的权威身份；bootstrap 凭据在同事务签发。首次 `AdapterBridge` 登记用 bootstrap 的 resource_id 作为 `assigned_instance_id` 创建 AdapterRecord 并校验声明的每个 account binding 都属于该实例，之后重连只推进 connection_generation。外部自管 plugin 首次登记分配并持久 execution_generation=1，重连保持 execution_generation、仅推进 connection_generation；这是“外部进程执行由该登记连接代表”的目标语义，P08 Host 受管执行仍使用真实进程代次。
- **P06 target 验收**：真实 AdapterBridge（NoneBot bridge client + durable outbox）与真实
  PluginBridge（SDK Runner）已对真实 Runtime/PG/TLS 通过注册、授权、refresh、事件 lease/
  Renew/ACK、重连新 connection_generation 与有界 ShutdownNotice 的端到端测试
  （`ctest -R p06_client_e2e`）；容量背压路径仍由单元/契约测试覆盖，未做真实饱和压测。
- **超时与背压**：`check_dependencies` 每秒有界 sweep 把过期 attempt 记一次可重试失败（1/2/4/8/16s ±20%，5 次后死信）；执行不持有 UoW 等网络。入站容量不足按 target 语义返回可重试 `RATE_LIMITED` 并关闭本次流，Adapter 保留未确认事件重投；`semantic.bridge.*` 已由生产 Runtime Bridge 实现并登记到 target execution scope；HostBridge 与 P07 Action 面仍 not_implemented。

### P07 目标 Action 语义（已冻结，未实现）

P06 之后插件 Action 只走 `PluginBridge` 流帧、适配器执行只走 `AdapterBridge` 流帧；unary Action 方法为 internal dispatcher（`exposure=internal_dispatcher`），不对外。Runtime 受理必须原子写入 Action 记录/幂等索引/配额窗口/日预算/审计并提交后才派发；任何提交失败不调平台。派发前再复核权限、账号/白名单/开关、能力与 deadline，条件写 `dispatched`+`dispatch_attempt_id` 后才发流帧。适配器先本地 journal 登记再调平台，终态结果用 `action_result` 上报、未知/未执行用 `action_evidence` 上报、Runtime 用 `action_receipt` 确认；Action 已 EXPIRED 的迟到结果只追加证据不改终态，已派发未知不跨 adapter 实例重派。取消只对未派发 ACCEPTED 生效，派发后 CONFLICT；查询超时不改状态、`NOT_FOUND` 不证明未执行。终态 168 小时保留；控制查询必须显式账号 scope + `action.inspect`，插件查询限制自身 owner。详细状态×执行已知性矩阵、回执/证据语义、权限词表与配额/策略见 `doc/01-contracts.md` 的 P07 目标 Action 契约节。

### P08 目标 Host 生命周期与执行观察（已冻结，未实现）

HostBridge 只走 Runtime 内部 mTLS（Host 证书→预注册 host_id，无 bootstrap/proof）；注册后每帧带 Fence，同 host 新连接替换旧连接。Host 每 5s 心跳，15s 无心跳只把在线状态置 false、`process_outcome` 转 UNKNOWN，不写 FAILED/已停止、不 bump 版本。观察身份为 `(host_id, connection_generation, cursor.sequence)` + `command_id`，Runtime inbox 以 source=Host 去重，断线用 resync 补播。Host 先持久命令 journal 再 spawn，marker（boot id+pid+starttime）校验后才认领；同代次无法确认时不重复 spawn，等 Runtime 提升 generation。期望/进程事实/注册/配置应用四层分开：Host 离线不证明进程停止；配置只允许当前 fence+当前 config_version 的 ACK 推进持久 `applied_version`，旧 ACK 幂等 no-op，重连后 delivery 变 Unavailable 并重新应用。跨主机变更要求旧执行终止或操作员显式处置，否则 operation Blocked。详细字段/恢复/凭据部署见 `doc/01-contracts.md` 的 P08 目标 HostBridge 契约节。

### P09 目标 Storage/CAS/Namespace（已冻结，未实现）

外部插件 storage 只走 PluginBridge HostRequest.storage_* 帧，Runtime 按已认证实例解析私有/共享 namespace（`installation_shared` 由服务端解析，插件不自报 owner）；4 个 unary storage 方法为 internal dispatcher。共享 namespace 的 Set/Delete/CAS 必须带 condition（absent=create、version=update/delete），私有 Set 必须缺 condition；CAS 冲突 CONFLICT 且只回授权范围版本。TTL 为绝对 UTC、未来且 ≤30 天；命名空间 1024 键/1MiB、单值 64KiB、安装聚合 8192 键/8MiB；写收据复用通用 command_receipts（7 天、每实例 10000/安装 80000、满则 RATE_LIMITED 不淘汰有效记录）。共享成员访问需 storage.namespace + 成员权限双门禁，撤销与并发写同 gate 线性化。Create/GetSharedNamespace 是既有 Runtime owner 方法新增（namespace.manage/read + exact installation scope），插件首次写前必须已创建，否则 NOT_FOUND。详见 `doc/01-contracts.md` 的 P09/P10 契约节。

### P10 目标任务与 TimerTriggered（已冻结，未实现）

任务 owner_instance/installation 由 Runtime→Scheduler 实例委托 claims 派生，插件不可自报；task_id 为 owner 内业务身份，HostRequest.idempotency_key 为命令幂等。run_at 必须严格未来且在配置 horizon 内；local_time 用 IANA 时区、拒绝 DST gap/fold 与未知/路径型 zone，tzdata 不可用返回 UPSTREAM_UNAVAILABLE。每实例 1024/安装 8192 未终态任务，超限 RATE_LIMITED（明确未受理）。Create 返回 PENDING；到期 claim 生成 occurrence 并持久 enqueue TimerTriggered 后 occurrence SUCCEEDED、task COMPLETED，COMPLETED 仅表示事件已持久入队，不代表插件回调成功/ACK。取消仅在没有触发意图时成功，交接开始后/终态 CONFLICT。交接 valid_until 7 天、attempts 上限 100，超窗 occurrence EXPIRED、task FAILED(TIMEOUT) 不自动重建。TimerTriggered 只由 Runtime 产生，occurrence_id 由 Scheduler 生成持久且 event_id := occurrence_id，Runtime 以 inbox(source=scheduler, message=occurrence_id) 去重；AcceptTimerTrigger 仍是内部 mTLS owner 方法（非 Bridge）。详见 `doc/01-contracts.md` 的 P09/P10 契约节。

### P11 目标 Model 调用（已冻结，未实现）

Model 调用是“先意图→Provider→已知/UNKNOWN”两次事务：txn1 在线授权、解析 model/alias、per-model 并发 admission（满则 RATE_LIMITED 且不写记录）、服务端计算 request_hash、写 model_calls(STARTED)+审计并提交后才调用 Provider；txn2 根据 Provider 结果写 SUCCEEDED/KNOWN 或 FAILED+明确 4xx/429 证据，5xx/TLS/超时/畸形响应/无法确认一律 BLOCKED+UNKNOWN+may_have_billed=true，不得写成未调用/未计费。同 subject+logical_call_id 不重复调用/不重复计费：hash 相同回放（IN_PROGRESS/TERMINAL，无正文不重调），hash 不同 CONFLICT；只有 ResolveModelCall 能把未决 UNKNOWN 转 FAILED 终态。在途固定 config/secret 版本，轮换只影响后续；根密钥部署注入、秘密 AEAD 只在 Model 进程解密且不入库；错误映射不新增 ErrorCode，用既有 code+acceptance/recovery/retry_after_ms；不流式、不工具调用、不自动重试。详细 wire 形状/错误矩阵/密钥边界见 `doc/01-contracts.md` 的 P11 目标 Model 契约节。

### 目标管理、存储和异步交接

期望配置、Host进程事实、协议注册与配置应用分层；旧 desired_version 或 execution_generation 观察不能覆盖新状态。Host离线不是已停止证据，不允许盲跨主机接管。私有 namespace 绑定稳定实例，换连接不重建；共享写条件版本，TTL 重放不续期。默认私有/共享各1024键/1MiB、单值64KiB、安装聚合8192键/8MiB；成功收据实例10000/安装80000，七天内不淘汰。一次性任务实例1024/安装8192；COMPLETED 仅 Runtime 已持久入队，确认丢失重发同 occurrence/message ID。

模型先提交意图再调用，后提交已知结果/UNKNOWN，同 logical_call_id 不再次调用供应商。首轮不承诺正文重放；Chat 响应丢失查询状态，不能自动换 call_id。Action 已派发取消冲突；终态168小时，过期 ACCEPTED 迁 EXPIRED/TIMEOUT，迟到平台证据不得覆盖。未知结果与权限依赖故障保持事实/Blocked，不把未能授权等同明确拒绝。

### P01 调用副作用与幂等种类

`target_spec.json.mutating` 表示会改变持久/观测状态，不能为了复用普通收据开关而标成只读。Chat、ACK/Renew、心跳、注册、事件/Timer/Host交接均可变更状态，但分别以 logical_call_id、attempt/fence、registration/fence、event_id+source+hash、message_id/occurrence_id、command_id+hash 防重，不等同普通 idempotency_key 响应收据。descriptor 的 requires_idempotency 只表示必需普通业务幂等键；状态变更和此开关独立。断线未收到回执仍保留原稳定业务身份恢复。所有者RPC集合必须与规格双向相等，不能有未登记额外方法。

目标Model操作状态只由Model裁定；Control按owner路由，不用Runtime的投影或HTTP超时推断Model完成。GetOperation在线检查原操作绑定资源，查询超时不创建FAILED、不自动重发Provider。GetModelConfiguration是敏感读，当前Identity不可确认则503，不能用缓存或secret_configured布尔绕过授权，也不返回旧secret。

Runtime目录列表是按当前主体精确child授予过滤的不可变快照，不是目录内所有资源投影。目录创建/发现权限与child读/管理权限分别检查；角色、owner服务身份、安装关联均不隐含后代授权。每页绑定当前会话、授权版本、目录、页大小/排序及快照期限，撤销后不能借10分钟快照继续敏感读。Adapter offline仅当前连接失效，保留最后合法metadata；Host offline不证明进程停止。GetInstanceConfig区分已保存期望与SDK实际应用，applied=0合法，REJECTED必须有原因。

## Identity 当前投影与凭据状态（目标未实现）

Identity 的 ValidateSession/GetPrincipal 返回当前主体的逐条 AssignmentRecord、authorization_version 和权威 UTC checked_at。每条绑定独立计算权限与 exact scope，不能把显示 roles/permissions 与全部 scope 做笛卡尔积授权；PrincipalRecord.roles/display_permissions 仅是当前绑定的显示并集。固定角色只用于 human；显式权限使用 role=UNSPECIFIED 加非空、去重的封闭 Permission 列表，两种授予依据不能混用。message.read/action.inspect 等敏感权限不由默认角色或创建资源自动授予。空 scope 不代表全局。

SetAssignment 输入 AssignmentSpec：创建不带 assignment_id/expected_version，Identity 生成 UUID/version=1；更新二者同时提供且版本为正，主体不可更换，并核验旧、新 scope 并集上的当前授予权限。目标尚未发布，原开发快照的 AssignmentRecord 请求已统一替换为 Spec，不提供旧请求桥接。

SessionRecord.created_at 是 Identity 持久保存的首次创建时间，刷新/撤销不改写。会话状态以同响应 checked_at 判定：revoked 优先 REVOKED，其次 checked_at >= expires_at 为 EXPIRED，否则 ACTIVE。Login/Refresh 的剩余 access TTL 为 max(0,floor(access_expires_at-checked_at))；Control 不用本地时间、token iat 或占位时间补齐。缺失权威字段、非当前绑定或授权依赖不可用必须拒绝，不能伪造空权限/成功。

RegisterPrincipal 同事务创建无秘密的 PENDING CredentialStatus 引用（credential_id/principal_id/binding/version/created_at/checked_at）；尚未发放的 PENDING 不得伪造 expires_at。GetCredentialStatus 以 identity.principal.read 在线读取同一记录，默认2秒/最多5秒；ACTIVE/EXPIRED 必须有 expires_at，REVOKED 可保留原到期。状态查询不返回秘密、摘要或可重放材料。初次实际发放仍用 RotateCredential 单次 CredentialDelivery，未知结果不得从状态查询恢复秘密。当前未实现，不启用生产路由。

## 外部 Bridge 连接授权（目标未实现）

采用已批准的最小三载荷 ConnectionAuthorization / RefreshConnectionAuthorization / ConnectionAuthorizationResult，唯一来源 platform.proto 与 adapters.proto/plugins.proto。Adapter/Plugin 成功登记必须带 authorization，且与响应 Fence 一致；注册后入站帧必须带完整 Fence 和当前 connection_proof（续期正文 proof/Fence 也须与帧一致）。反向 refresh 结果按 request_id 返回恰好一个新 authorization 或稳定 Error，不能空成功。缺少 proof/授权不允许旧流回退。

proof/refresh_credential 是独立 CSPRNG 至少256位 opaque秘密，绑定实际已认证 TLS 流、principal、完整 Fence、Runtime epoch、credential_version；不能把客户端自报证书/Fence 当绑定。Runtime只保存摘要和当前受信连接状态，不把秘密写入公开投影、日志或outbox。bootstrap仅登记换取受限证明，不授权Action/Model/存储等业务。内部受限委托仍是Ed25519 JWT，平台owner监听仍mTLS；TLS-only外部Bridge传输接线属于P05/P06，不修改P02监听行为。

issued_at/expires_at 为受信UTC，名义寿命最多56秒；含既定2秒节点偏差和最多2秒处理容差总预算不超过60秒。服务端同时以单调期限和受信UTC约束，每秒检查到期、撤销、凭据版本和资源gate；客户端到期前10秒单飞续期。高风险操作仍逐次在线授权。Identity不可用不能刷新或无限延长旧流，停止新副作用并返回UPSTREAM_UNAVAILABLE；不伪造PERMISSION_DENIED。

refresh只接受当前实际流、未过期未撤销的proof/refresh及正refresh_generation，在线验证当前Identity主体/凭据状态与gate后原子轮换二者和generation；旧材料不可再次换发。材料/流/Fence失配或过期UNAUTHENTICATED并关闭流，已认证请求generation冲突CONFLICT，明确失权PERMISSION_DENIED，依赖故障UPSTREAM_UNAVAILABLE，期限TIMEOUT。失败不轮换、不复活失效授权。refresh无持久秘密收据；响应丢失视授权未知，关闭旧流，经仍有效的登记凭据重新bootstrap产生新connection_generation，禁止重放旧refresh猜测结果。

授权过期停止新投递、入站受理、续租和副作用，不把已有Action/Model事实变FAILED。auth刷新不续Delivery租约；ACK/Renew仍核验有效attempt、完整Fence及现有事实规则。适配器本地已执行结果在断线后通过新认证连接按原account/action ID交回，不能据失效proof新执行。所有新增字段/帧target均not_implemented，oracle和wire往返不证明生产授权或并发正确性。

## P04 短期委托证明的重试边界

Identity IssueDelegation / ExchangeDelegation 的唯一线契约 requires_idempotency=false。
两者只检查当前来源凭据、会话、调用服务和精确权限，然后签发短期证明；不受理业务命令、
不扣减配额、不消费一次性凭据，也不产生 Action/Model 等业务副作用。重试必须重新授权，
可以返回新 proof ID/签名/期限；不承诺返回同一 JWT，不把 JWT 写入普通七日收据或伪装成
CredentialDelivery。证明仍最多 56 秒正文期限与 2 秒时钟容差，业务跳数最多两跳。
Login、RefreshSession、RotateCredential、RevokeSession 等既有幂等/恢复要求保持原契约。
未来若引入配额扣减或一次性消费，必须先建立独立的幂等命令契约及事务收据。

### P05 Runtime 目录列表快照

CreateAccount、ValidateManifest 和 CreateInstallation 必须携带所属目录的 ResourceRef；CreateInstance 按父安装精确授权。新资源成功创建不派生任何 Identity grant。未配置目录映射时受影响方法返回 CAPABILITY_NOT_SUPPORTED，既有 exact 方法照常授权。

四种已有 typed 列表在一致只读事务中冻结候选记录、排序及观察时间，释放事务后逐项在线授权。分页仅移动不可变候选偏移，不重新读取实时行；原观察时间及十分钟到期时间固定。每页先检查目录授权，再检查 child；授权拒绝隐藏 child，依赖失败不能变为空页，授权版本变化、过期、重启或主体/目录/方法/页大小/委托范围失配返回 CONFLICT。快照保留纯数据及权限范围，不保留可复用 proof。预算及 fail-closed 端口说明见 doc/02-core.md 对应 P05 章节；达到容量返回 RATE_LIMITED，不能截断后宣称集合结束。ListHosts 的生产数据源仍待独立实现。

## Runtime 管理命令重放（2026-11-04）

五项管理写入（DisableAccount、BindAdapter、UnbindAdapter、SetDesiredState、UpdateConfig）将父回执、Runtime 专用附属回执、原业务版本、审计及已有 operation/Host 命令意图在同一 UoW 原子提交。附属表保存 format_version=1、方法/结果辨别信息、完整 Runtime OperationRef，以及实例提交状态；重试重验授权后按原身份恢复，原 operation 当前行被清理也不改变回执引用。提交未知沿用 UNKNOWN/同身份查询，不能换键重做。附属行在父回执过期清理或过期键替换时级联删除；retain_until 到期即不承诺完整回放，即使尚未物理清理。至少七天保留不保证过期 UNKNOWN 可安全重试，仍需人工核对原操作。

Runtime unary 指纹清除 RequestContext 后采用固定版本 protobuf deterministic 编码，再求 SHA-256；Struct map 的字段插入顺序和进程内 hash 顺序不参与幂等身份。配置等有业务意义的字段、expected_version 与完整资源仍参与指纹。该编码修复仅在已批准的隔离新库验收；不得直接切换仍持旧指纹回执的部署。
