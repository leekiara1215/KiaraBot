# PluginSDK 与插件运行时设计

## T06c 任务客户端

Context.task_create(task_id, target_conversation, payload, idempotency_key, run_at=UTC datetime 或 local_time=LocalTaskTime 字段字典)、task_get(task_id)、task_cancel(task_id, idempotency_key) 使用 host.tasks.v1，默认五秒 deadline。必须获授 task.schedule；创建还需 event.timer.triggered 订阅。返回生成的 TaskResponse；本地时间不猜测 DST，详见 doc/05-infrastructure.md。任务身份来自注册会话，不接受自报 owner；事件仅交给创建插件。COMPLETED 代表 TimerTriggered 已入队，回调仍可能重复，消费者按 task_id 幂等。

## T06b 模型客户端

Context.model_chat(model, messages, timeout=5.0) 返回生成的 ModelChatResponse；messages 使用 role/text 字段。只在 host.model.v1 协商成功后发送，必须获授 model.chat。请求限制与错误码见 doc/05-infrastructure.md。默认五秒、最多五分钟的 Host deadline 另受 Provider 最长三十秒限制。模型调用没有幂等回放，SDK 不自动重发；断线或 TIMEOUT 可能已经产生费用。插件不接触 HTTP 客户端、端点和 API Key。

plugins/ai-reply 示例仅对 /ai 前缀纯文本事件应答，通过 SDK 存储先记录调用意图，再调用配置模型别名 default，最后发送 Action。若调用完成前崩溃/超时，意图记录阻止重放再次调用模型；该事件可能不产生回复，需要人工处理。已经保存的回答可以重放提交相同 Action 幂等键。示例需要 message.read、message.send、model.chat、storage.namespace，事件回调超时应配置为大于模型调用时限。

## T06a Host 存储 API

注册协商 host.storage.v1 后，Context.storage_get(key)、storage_set(key, value, idempotency_key, expires_at) 和 storage_delete(key, idempotency_key) 使用现有 HostRequest/HostResponse。SDK 自动关联 request_id/trace_id，deadline 默认五秒且最多五分钟；写入/删除幂等键由插件提供（16–128 字符），重试必须保持原键和内容。namespace 由认证插件身份确定，接口不接受自报 namespace。获授 storage.namespace 才能访问。

键 1–256 字节，值最多 64 KiB；每插件最多 1024 个未过期键、1 MiB 有效值。expires_at 缺省永久，否则必须为未来 UTC、最多三十天；到期读取 NOT_FOUND，删除不存在/已过期键成功。写入超限 RATE_LIMITED。成功写操作与幂等响应、审计原子提交；同一插件同键不同规范化内容 CONFLICT，重放不延长 TTL。幂等响应保留七天，每插件最多一万条，满时拒绝写入。断线后同键重试可恢复结果，失败不会污染命名空间。

## T04b Action 客户端

Context 提供 submit_action(ActionEnvelope)、query_action(action_id 或 idempotency_key) 与 cancel_action(action_id)。使用生成类型，自动创建每次调用的 request_id，只有 action.result.v1 协商成功后才发送；未协商本地 CAPABILITY_NOT_SUPPORTED。读取响应独立于事件回调，回调中调用 Action 不会阻塞接收器。断开/超时抛稳定 PluginError，不能把未知结果包装成 FAILED；调用者重连后用原幂等键查询。SDK 不自动重发可能已有副作用的请求。

## T03b Python SDK 与进程生命周期

`sdk/python` 提供 `Plugin.on_start(context)`、`on_event(EventEnvelope, context)`、`on_stop(reason)` 和本机 gRPC runner，事件类型直接使用生成协议。插件自行实现业务幂等；SDK 不把网络 ACK 当作 Action 完成。清单由 protobuf JSON 解析并校验现行 T03a 子集，不导入 Core 或 NoneBot。

本地 on_start 成功后才注册；每次连接生成新 instance_id，重新验证 required_api_version 与 granted_permissions。完整授权成功才处理事件。正常处理发送正 ACK；`PluginError` 保留稳定码与 retryable，其他异常转换为不含自由文本的 INTERNAL，协作式处理超时返回可重试 TIMEOUT。断线保持结果未知，Core 按至少一次语义重投。停止先取消流，再调用 on_stop；阻塞解释器或拒绝取消的插件由宿主进程硬超时回收。

Core 可启动配置的 Python 插件子进程，使用 argv 而非 shell，显式传入插件凭据，不继承适配器 token、数据库或模型配置。注册前失败/超时阻止受管插件启动成功；运行中崩溃不终止 Core。停止对独立进程组发送 SIGTERM，超过宽限期发送 SIGKILL 并 waitpid。进程隔离不是操作系统权限沙箱。

SDK 提供真实本机 gRPC FakeHost，用于注册、事件、ACK、拒绝、断线、心跳不返回（`heartbeat_acks=False`）以及 Action/Host/配置帧的成功和故障交互；模型调用由 T06b、任务由 T06c、配置由 T08b 接线。依赖由 sdk/python/uv.lock 固定，生成代码进入忽略的 build 目录。验收入口见 sdk/python/README.md。

## 运行时参数（T03b 补充）

`python -m kiarabot_sdk --manifest <path>` 只从环境读取以下变量；缺失必填项或非法数值以稳定码 `INVALID_ARGUMENT` 退出：

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `PLUGIN_CORE_ENDPOINT` | 必填 | 仅接受 `127.0.0.1:<端口>` |
| `PLUGIN_AUTH_TOKEN` | 必填 | 32–4096 个可打印 ASCII 字符 |
| `PLUGIN_IO_TIMEOUT` | 5.0 | gRPC 单次写入/读取响应的秒级超时 |
| `PLUGIN_HOOK_TIMEOUT` | 5.0 | `on_start/on_event/on_stop` 协作超时；配置回调另受 `apply_timeout_ms` 限制 |
| `PLUGIN_HEARTBEAT_INTERVAL` | 1.0 | 心跳发送与读循环检查间隔 |
| `PLUGIN_RETRY_DELAY` | 1.0 | 可重试连接失败后的等待 |

Core 受管启动器当前只传递最小 PATH、endpoint、token 与可选 PYTHONPATH，尚未透传其余四项（交接 core）；受管插件使用默认值。宿主启动/停止硬超时由 Core 的 `KIARABOT_PLUGIN_START_TIMEOUT_MS`/`KIARABOT_PLUGIN_STOP_TIMEOUT_MS` 控制，与这组参数无关。

## 示例插件

| 插件 | 使用能力 | 权限 | 说明 |
| --- | --- | --- | --- |
| `plugins/group_admin` | `event.message.received`、`send_message` 纯文本、storage | `message.read`、`message.send`、`storage.namespace` | `/group help/info/rules/rule add/welcome`；`Mute/Kick/Approve` 在 v1 契约中未实现，本示例从不构造这些 payload |
| `plugins/reminder` | `event.message.received`、`event.timer.triggered`、`task_create/get/cancel` | `message.read`、`message.send`、`task.schedule` | `/remind <秒> <文本>`、`status`、`cancel`；定时到点由 TimerTriggered 回投递回复，task_id 与 Action 幂等键确定，重复 timer 不重复副作用 |
| `plugins/ai-reply` | `event.message.received`、非流式 `model_chat`、storage、`send_message` | `message.read`、`message.send`、`model.chat`、`storage.namespace` | `/ai` 前缀单轮回复；先记录调用意图再调模型，避免重放重复计费 |
| `plugins/event-observer` | `event.message.received` | `message.read` | 最小收事件示例 |

基线要求的三类示例插件中，`ai_chat` 由 `ai-reply` 对应；`group_admin`、`reminder` 现在以 v1 现有协议能力补齐（不含任何未实现 payload）。两个新示例的配置样例为 `plugins/<name>/config.example.json`，经 `plugin.config.v1` 下发（当前只支持空 schema，插件在 `on_config` 中自行校验）。插件测试：`sdk/python/.venv/bin/python -m pytest -q -c sdk/python/pyproject.toml plugins/group_admin/tests plugins/reminder/tests`。

## T03a 当前桥接子集

PluginBridge 首帧注册绑定认证主体，要求 api_version=kiarabot.plugin.v1、runtime=python-rpc、isolation=process。当前只支持无依赖、空配置 schema（空串或 {}）的事件插件；未实现的配置 schema/依赖/运行时明确返回 CAPABILITY_NOT_SUPPORTED。权限必须是宿主配置允许列表的子集，自报清单不会授予权限。

事件订阅由 capabilities 决定：event.message.received / event.message.sent 需要 message.read，event.group.member.changed 需要 member.read，event.friend.request 需要 request.read，event.adapter.status.changed 需要 adapter.read。未知 capability 拒绝。注册成功只表示该进程外连接已可收事件；受管启动器已由 T03b 实现，但注册本身不代表该连接由启动器管理。

同一插件只允许一个活动流。旧流尚未完成读取退出、发送线程取消和 join 时，新注册返回可重试 CONFLICT；旧流完全停止后才回收在途投递。重连重新协商权限/API/ordered，必须与持久化记录一致；订阅清单可重新声明，既有待投递目标不变。一次每插件最多一个在途事件，保证有序会话不并发；不同插件独立 ACK。事件 trace_id 使用逐插件投递的当前值，支持死信重放。

ACK 只能确认本连接身份的 DISPATCHED 记录；成功 ACK 不得带 error，失败 ACK 必须有有效稳定错误码及 retryable。重复成功 ACK 幂等，不重复审计；非法 ACK 或存储失败结束流并保留可恢复状态。未协商或尚未接线的 Action/Host 功能显式拒绝 CAPABILITY_NOT_SUPPORTED。传输关闭/取消不等于业务失败；当前协议无尝试编号，仍是至少一次语义。

## 责任

PluginSDK 是插件与 Core 之间的稳定边界。它负责：

- 插件清单和版本协商；
- 插件生命周期；
- 事件订阅；
- 消息和 Action 服务；
- 模型、存储、权限和任务服务；
- 插件隔离、心跳、日志和错误处理。

它不负责：

- QQ 登录；
- OneBot 解析；
- 具体 AI SDK；
- Core 内部数据库表；
- 控制面板页面。

## 三类扩展的边界

```text
渠道适配器：平台消息 ↔ 统一事件 / Action
插件：统一事件 ↔ 业务行为 / Action
基础设施提供商：统一服务接口 ↔ 外部 SDK 或数据库
```

NoneBot2 是渠道适配器，不是业务插件。AI 聊天、群管理和提醒才是插件。

## SDK 组成

```text
proto/kiarabot/v1/      跨语言协议
sdk/python/             Python RPC SDK（当前唯一已实现）
sdk/cpp/                C++ RPC / C ABI SDK（未实现）
sdk/typescript/         控制面板客户端类型（未实现，面板从 api/*.yaml 生成类型）
tools/kiara-plugin/     创建、校验、测试和打包插件（未实现，交接项）
plugins/<plugin_name>/  插件清单、配置样例、实现和测试
```

SDK 包按语言分别维护自己的包配置和测试目录；插件实现不能放入 `core/` 或 `sdk/`。

当前冻结契约的缺口、建议协议任务和其他模块的配合事项见
[`sdk/PLUGIN_SDK_DEPENDENCIES.md`](../sdk/PLUGIN_SDK_DEPENDENCIES.md)。该文件为待评审建议，
不改变现有协议或承诺未实现的 SDK 行为。

## 插件清单

每个插件必须提供 `plugin.json`。下面是当前**可注册**的最小示例（与 `plugins/group_admin/plugin.json` 一致）：

```json
{
  "id": "group-admin",
  "name": "Group admin example",
  "version": "0.1.0",
  "api_version": "kiarabot.plugin.v1",
  "runtime": "python-rpc",
  "entrypoint": "group_admin:create_plugin",
  "capabilities": ["event.message.received"],
  "permissions": ["message.read", "message.send", "storage.namespace"],
  "isolation": "process",
  "ordered": true
}
```

当前 v1 子集由 SDK（`load_manifest`/`validate_manifest`）与 Core（`PluginHub::attach`）双方校验：

- `id`/`version` 为 1–128 个 `[A-Za-z0-9_.-]`；`name` 非空且 ≤128；`api_version` 必须为 `kiarabot.plugin.v1`；
- `entrypoint` 必须是 `module:factory` 形式且 ≤256；SDK 需要可导入，因此该正则比 Core 的「非空/长度」检查更严，这是有意的本地约束；
- `runtime` 必须为 `python-rpc`，`isolation` 必须显式写 `process`（**没有默认值**）；`native`/`wasm` 与依赖解析尚未实现，返回 `CAPABILITY_NOT_SUPPORTED`；
- `dependencies` 非空返回 `CAPABILITY_NOT_SUPPORTED`（无依赖解析）；
- `config_schema` 只接受空串或 `{}`，非空返回 `CAPABILITY_NOT_SUPPORTED`（无 JSON Schema 解释器，配置值仍可经 `plugin.config.v1` 下发）；
- `capabilities` 只能取 `event.message.received`、`event.message.sent`、`event.group.member.changed`、`event.friend.request`、`event.adapter.status.changed`、`event.timer.triggered`，且必须同时声明其所需权限（映射见 T03a）；未知 capability 返回 `CAPABILITY_NOT_SUPPORTED`，缺少所需权限返回 `PERMISSION_DENIED`；
- `permissions`/`capabilities` 各 ≤128、不得重复、名称必须匹配 `[A-Za-z0-9_.-]{1,128}`；
- `ordered` 由 Core 持久化并在重连时与注册清单比对（不一致返回 `CONFLICT`）；SDK 原样传递，不在本地重新解释。

注意：早期草案示例中的 `config_schema: "config.schema.json"` 与非事件 capability（`action.message.send`、`model.chat`、`conversation.*`）在当前实现下会被拒绝，已按上表修正。

## 生命周期

```text
Discovered
  → Validated
  → Registered
  → Configured
  → Ready
  → Running
  → Stopping
  → Stopped
```

目标接口：

```text
register(manifest)
configure(config)   # 当前实现为 on_config 回调，无独立 configure RPC
start(context)
health()            # 未实现：插件健康目前只由注册/断线状态推断
stop(reason)
```

规则：

- `start` 必须有超时；
- `stop` 必须支持超时和强制终止；
- 插件异常不能让 Core 退出；
- 插件重连后必须重新注册和协商能力；
- 事件和 Action 必须支持幂等键；
- 停用插件时，Core 应暂停或取消其定时任务。

## Host API

插件通过 Host API 使用 Core 能力。

### MessageService

状态：**未实现该 service 形态**。v1 通过 `Context.submit_action(ActionEnvelope)` 发送 `send_message` 纯文本（群/私聊）、`react_message` 表情回应（群/私聊）、`delete_message` 撤回（仅限本 bot 自己发送的消息）、`mute_member` 禁言、`kick_member` 踢人（群管理，受白名单/每日预算门禁）与 `approve_request` 好友请求审批（`request_id` 回填 `FriendRequest.request_id`），语义见 `doc/01-runtime-semantics.md`，Action 会经过权限、限流、审计和适配器能力检查。

### ModelService

状态：**仅非流式已实现**（`Context.model_chat`，T06b）。`stream_chat` 未实现，协议没有流式消息。插件提交模型请求，但不能获得模型 API Key；Provider、模型、超时、配额和重试由 Core 管理。

### ConversationService

状态：**未实现**。`HostRequest` 没有会话读写操作，插件只能把会话相关数据写入自己的 storage 命名空间（`storage_get/set/delete`）并自行编码。会话管理需要先冻结协议（交接 proto）。

### StorageService

状态：**已实现**（`host.storage.v1`；签名以 T06a 章节的 `storage_get/set/delete` 为准）。namespace 由认证插件身份确定，不接受自报 namespace；插件不能获得数据库连接，也不能修改 Core 的表结构。

### PermissionService

状态：**未实现**。没有权限查询 RPC；插件只能读取 `Context.granted_permissions`（注册协商后的静态授予集合）做自省，最终决策仍由 Core 在实际操作时执行（交接 proto）。清单声明的权限只是申请，不代表自动获得权限。

### SchedulerService

状态：**部分实现**：`task_create`/`task_cancel`/`task_get` 已可用（T06c），`list(owner_plugin_id)` 未实现（协议无列表操作）。每个任务记录插件 ID、目标会话和取消策略；任务身份来自注册会话。

### ConfigService

状态：**已实现**（`plugin.config.v1`；仅当 Core 配置了控制面配置下发时协商）。当前只支持空配置 schema（空串或 `{}`），非空 schema 返回 CAPABILITY_NOT_SUPPORTED；JSON Schema 解释器仍为后续能力。配置值样例见 `plugins/group_admin/config.example.json` 与 `plugins/reminder/config.example.json`，插件在 `on_config` 中自行校验类型。Core 向协商 `plugin.config.v1` 的插件推送带版本和 delivery_id 的完整配置：

```text
on_config(PluginConfig, Context)
```

成功返回才确认应用；上一成功快照可从 context.config 读取。详见本页配置下发闭环。

### Host RPC 契约

具体消息以 `proto/kiarabot/v1/plugins.proto` 中的 `HostRequest` 和 `HostResponse` 为准；调用通过已注册的 `PluginBridge` 连接关联插件身份，不能由插件指定存储命名空间或任务所有者。v1 的模型接口是非流式聊天；流式接口仍是后续能力。任务接口支持一次性 UTC 或 IANA 时区当地时间，不接受含糊或不存在的夏令时时间。幂等、错误和断线语义见 `doc/01-contracts.md` 和 `doc/01-runtime-semantics.md`。

## 权限

当前 Core 实现实际使用的权限词表（`core/src/application/plugin_hub.cpp` 的 `required_permission`、`HostDispatcher`、`ActionRouter` 与 `plugins/*/plugin.json`）：

```text
message.read        订阅 event.message.received / event.message.sent
message.send        提交 send_message Action
message.react       提交 react_message Action
message.delete      提交 delete_message Action（仅限本 bot 自己发送的消息）
member.mute         提交 mute_member Action（受白名单/每日预算门禁）
member.kick         提交 kick_member Action（受白名单/每日预算门禁）
model.chat          host.model.v1 model_chat
storage.namespace   host.storage.v1
task.schedule       host.tasks.v1 与 event.timer.triggered 订阅
member.read         event.group.member.changed
request.read        event.friend.request
request.approve     提交 approve_request Action（好友请求审批）
adapter.read        event.adapter.status.changed
```

以下名称只出现在早期设计草案，当前没有协议入口或实现，不应写入可运行清单：`conversation.read`、`conversation.write`、`scheduler.create`、`user.read`、`group.read`、`group.manage`、`admin.config`。它们不会驱动任何已实现能力。

权限作用域（`account:<id>`/`group:<id>`/`user:<id>`/`conversation:<id>`）仍是后续能力；当前权限按插件身份全局授予，由 Core 在每次操作时校验，插件不能自报身份或作用域。

## 运行模式

### 默认：进程外 RPC 插件

```text
Core ── gRPC / Protobuf ── Python Plugin
Core ── gRPC / Protobuf ── C++ Plugin
```

优势：

- 崩溃隔离；
- 支持多语言；
- 可以单独升级；
- 可以限制资源和权限；
- 不受 C++ ABI 影响。

### 可选：进程内 C++ 插件

只对完全信任的高性能插件开放，并使用稳定 C ABI：

```cpp
extern "C" const char* kiarabot_plugin_manifest();
extern "C" int kiarabot_plugin_start(const HostApi* host);
extern "C" void kiarabot_plugin_stop();
```

进程内插件必须和 RPC 插件遵守相同的事件、Action、权限和错误语义。

## Python 插件形态

下面是当前实际可用的形态（完整示例见 `plugins/group_admin`、`plugins/reminder`）：

```python
from kiarabot.v1 import actions_pb2 as actions
from kiarabot_sdk import Plugin


class Example(Plugin):
    async def on_event(self, event, context):
        if not event.HasField('message_received'):
            return
        action = actions.ActionEnvelope(
            action_id=event.event_id + ':reply',
            idempotency_key=event.event_id + ':reply',
            trace_id=event.trace_id,
            requested_by='example',
            target_source=event.source,
            target=event.subject)
        action.send_message.conversation.CopyFrom(event.message_received.conversation)
        action.send_message.message.plain_text = 'hello'
        await context.submit_action(action)
```

早期草案中的 `MessageService`/`on_message`/`context.models.default()`/`messages.reply()` 尚未实现，不应作为现行 API 使用。这段代码不能导入 NoneBot2，也不能调用 OneBot API。

## 测试能力

当前 SDK 只提供 `kiarabot_sdk.testing.FakeHost`（真实 gRPC + 生成类型）：注册/事件/心跳、Action 请求与响应、Host 请求与响应、配置帧与 ACK 队列；`FakeHost(heartbeat_acks=False)` 可模拟心跳不返回以验证 `io_timeout` 重连。早期列出的 `FakeEvent`/`FakeMessageService`/`FakeModelService`/`FakeStorage`/`FakeScheduler` 尚未实现（交接项），插件测试直接构造生成消息或使用 FakeHost。

已覆盖：事件触发/ACK、注册拒绝、权限拒绝、模型超时/限流/畸形响应、重复事件、配置变更/拒绝/超时、心跳超时、队列满 `RATE_LIMITED`、Action 取消、storage TTL 帧、任务 local_time 转发、manifest 非法字段；真实 C++ Core 子进程验收见 `tests/acceptance/run.sh`。未覆盖：插件停用事件（协议无停用帧）与适配器能力不足（属适配器模块）。

## 代码启动句

> 请只实现 PluginSDK v1 的插件清单校验、进程外生命周期、事件订阅、MessageService、ModelService、StorageService、PermissionService 和 SchedulerService 接口，使用 fake host 完成 Python 插件端到端测试，不连接 QQ 和真实模型。

## 配置下发闭环（T08b 后续）

REST PUT 配置成功只确认 desired version/value 已加密持久化；GET 返回当前连接的 delivery：UNAVAILABLE（离线、停用或未协商）、PENDING、APPLIED、REJECTED，以及该连接最后成功的 applied_version。REJECTED 带脱敏稳定 Error。PUT 与幂等重放不携带瞬时 delivery，查询使用 GET；接口权限与 HTTP 错误映射不变。状态只在内存保留，Core 重启/断线清除应用证据，重新连接必须重新确认。配置值仍禁止秘密字段，只支持空 schema，不新增 JSON Schema 解释器。

PluginBridge 协商 `plugin.config.v1`，注册后下发最新完整 PluginConfig（初始 version=0、values={}）。每连接最多一条在途配置，delivery_id 随机且绑定会话；SDK 的 `on_config(config, context)` 成功后才发送 PluginConfigAck。SDK 保存配置副本为 `context.config`；该属性返回副本，失败保留上一成功快照。配置回调与事件回调串行，但 RPC 响应继续独立接收，因此回调可调用 Host API。回调异常 INTERNAL、协作超时 TIMEOUT；PluginError 保留稳定码和 retryable，无原始异常文本。

Core 等待 ACK 最多十秒，SDK 回调上限为 min(hook_timeout, apply_timeout_ms/1000)，宿主下发 5000 ms。当前版本被拒绝或超时后不在同连接自动重试；提交新版本或重连恢复。Core 发现新版本后暂停新的事件投递，直到该 desired version 应用成功；已经执行的事件不撤销。失败不是回滚插件自有副作用的承诺，回调应先校验后原子替换本地状态。拒绝取消的回调沿用进程隔离退出机制。

更新期间保留在途帧，随后合并到最新 desired version，允许跳过中间版本。旧版本 ACK 不会让新版本显示 APPLIED；重复当前 delivery ACK 幂等，超时后的 ACK 不改变结果；错误 delivery/version 为 INVALID_ARGUMENT 并关闭流，旧连接 PERMISSION_DENIED。关闭/重启可能发生“已应用但 ACK 丢失”，重连重新应用最新快照，插件应使配置回调幂等。旧客户端不请求该能力时仍可收事件；API 不宣称其已应用配置。存储读取失败沿用 UPSTREAM_UNAVAILABLE/INTERNAL，关闭流后等待恢复，不回退到空配置。

### 目标管理读取补全（未实现，非插件越权入口）

Runtime typed Installation/Instance查询及GetInstanceConfig供获准管理者/受限服务使用，明确安装/稳定实例/Host绑定与期望、已应用版本；SDK仍从认证配置投递接收自身配置并ACK，不因方法声明获得任意实例读取权。新目录scope不继承子实例权限，plugin.config.read是独立敏感权限，秘密不以明文配置查询回显。离线/applied_version历史与当前注册、Fence分开；typed方法尚未生产接线。
