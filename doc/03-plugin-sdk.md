# PluginSDK 与插件运行时设计

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
sdk/python/             Python RPC SDK
sdk/cpp/                C++ RPC / C ABI SDK
sdk/typescript/         控制面板客户端类型
tools/kiara-plugin/     创建、校验、测试和打包插件
plugins/<plugin_name>/  插件清单、配置、实现和测试
```

SDK 包按语言分别维护自己的包配置和测试目录；插件实现不能放入 `core/` 或 `sdk/`。

## 插件清单

每个插件必须提供 `plugin.json`：

```json
{
  "id": "ai-chat",
  "name": "AI Chat",
  "version": "1.0.0",
  "api_version": "kiarabot.plugin.v1",
  "runtime": "python-rpc",
  "entrypoint": "kiarabot_ai_chat:create_plugin",
  "capabilities": [
    "event.message.received",
    "action.message.send",
    "model.chat",
    "conversation.read",
    "conversation.write"
  ],
  "permissions": [
    "message.read",
    "message.send",
    "model.chat",
    "storage.namespace"
  ],
  "config_schema": "config.schema.json",
  "dependencies": [],
  "isolation": "process",
  "ordered": false
}
```

Core 在启用前必须检查：

- ID 是否重复；
- API 版本是否兼容；
- 依赖是否满足；
- 权限是否允许；
- 配置 Schema 是否有效；
- 运行时和隔离模式是否允许。

`isolation` 只能是 `process`、`native` 或 `wasm`，默认使用 `process`；`ordered` 表示是否要求同一会话内按事件顺序串行投递。该字段与 `proto/kiarabot/v1/plugins.proto` 保持一致。

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
configure(config)
start(context)
health()
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

```text
send(target, message, options) -> ActionResult
reply(event, message, options) -> ActionResult
```

所有消息发送都会经过权限、限流、审计和适配器能力检查。

### ModelService

```text
chat(request) -> ChatResponse
stream_chat(request) -> Stream<ChatDelta>
```

插件提交模型请求，但不能获得模型 API Key。Provider、模型、超时、配额和重试由 Core 管理。

### ConversationService

```text
get(conversation_id, options)
append(conversation_id, message)
clear(conversation_id)
```

插件数据必须按插件 ID 隔离命名空间。

### StorageService

```text
get(namespace, key)
set(namespace, key, value, ttl)
delete(namespace, key)
```

插件不能获得数据库连接，也不能修改 Core 的表结构。

### PermissionService

```text
check(subject, permission, resource) -> Decision
```

清单声明的权限只是申请，不代表自动获得权限。最终决策由管理员、群组策略和用户角色共同决定。

### SchedulerService

```text
schedule(task)
cancel(task_id)
list(owner_plugin_id)
```

每个任务必须记录插件 ID、目标会话和取消策略。

### ConfigService

Core 使用 JSON Schema 校验插件配置，并向插件推送带版本号的配置变更：

```text
on_config_changed(old_config, new_config)
```

### Host RPC 契约

具体消息以 `proto/kiarabot/v1/plugins.proto` 中的 `HostRequest` 和 `HostResponse` 为准；调用通过已注册的 `PluginBridge` 连接关联插件身份，不能由插件指定存储命名空间或任务所有者。v1 的模型接口是非流式聊天；流式接口仍是后续能力。任务接口支持一次性 UTC 或 IANA 时区当地时间，不接受含糊或不存在的夏令时时间。幂等、错误和断线语义见 `doc/01-contracts.md` 和 `doc/01-runtime-semantics.md`。

## 权限

第一批权限：

```text
message.read
message.send
message.delete
user.read
group.read
group.manage
conversation.read
conversation.write
model.chat
storage.namespace
scheduler.create
admin.config
```

权限必须绑定作用域：

```text
global
account:<id>
group:<id>
user:<id>
conversation:<id>
```

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

## Python 插件目标形态

下面是目标接口示意：

```python
from kiarabot_sdk import Plugin, MessageReceived


class AIChatPlugin(Plugin):
    async def on_start(self, context):
        self.model = context.models.default()

    async def on_message(self, event: MessageReceived):
        if not event.mentions_me and not event.is_private:
            return
        response = await self.model.chat(
            conversation_id=event.conversation.id,
            text=event.text,
        )
        await self.messages.reply(event, response.text)
```

这段代码不能导入 NoneBot2，也不能调用 OneBot API。

## 测试能力

SDK 应提供：

```text
FakeEvent
FakeMessageService
FakeModelService
FakeStorage
FakeScheduler
```

必须测试事件触发、权限拒绝、模型超时、重复事件、配置变更、插件停用和适配器能力不足等情况。

## 代码启动句

> 请只实现 PluginSDK v1 的插件清单校验、进程外生命周期、事件订阅、MessageService、ModelService、StorageService、PermissionService 和 SchedulerService 接口，使用 fake host 完成 Python 插件端到端测试，不连接 QQ 和真实模型。
