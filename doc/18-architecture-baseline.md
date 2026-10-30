# KiaraBot 目标架构基线

<!-- PROTECTED-DOC: human-owned baseline; AI agents MUST NOT edit this file. -->

> **受保护文档 · 人类维护 · AI 只读**
>
> 本文档由 KiaraBot 项目人类维护者拥有和修订。**任何 AI 代理、代码生成工具或自动化流程不得修改本文档**——包括正文、标题、表格、代码块、链接、文件路径、措辞和格式，也包括以“同步文档”“修正链接”“统一术语”“批量整理”为理由的改写。
>
> | 项 | 内容 |
> | --- | --- |
> | 文件 | `doc/18-architecture-baseline.md` |
> | 状态 | 冻结基线（Frozen） |
> | 维护者 | KiaraBot 人类维护者 |
> | 版本 | v1.0 |
> | 生效日期 | 2026-10-30 |
> | AI 约束 | 只读；禁止编辑；发现差异时向人类报告建议 |
> | 修订入口 | 仅人类维护者；修订后更新版本与日期，并在文末追加“变更记录” |
>
> AI 入口登记见 [`AGENTS.md`](../AGENTS.md) 的“受保护文档（AI 只读）”。本文档不适用 AI 强制文档同步规则：相关契约或实现发生变化时，AI 只报告差异，由人类维护者决定是否修订本文档。

**文档定位**：本文档是 KiaraBot 的目标架构设计基线，回答“系统应该长什么样、为什么这样分层、未来如何替换组件”。它描述设计方向，不代表当前完成度：跨语言契约的唯一来源是 `proto/`，HTTP/WebSocket 契约的唯一来源是 `api/openapi.yaml` 与 `api/asyncapi.yaml`，模块责任以 [`doc/00-boundaries.md`](00-boundaries.md) 及 [`doc/01-contracts.md`](01-contracts.md) 至 [`doc/08-operations.md`](08-operations.md) 为准。本文与上述契约或实现不一致时，由人类维护者裁定；撰写时的仓库落地差异见附录 A。

## 核心原则

建议把项目设计成一个 **协议优先、核心独立、适配器可替换** 的单仓库工程。

核心原则是：

```text
C++ Core 不依赖 NoneBot2
Python NoneBot2 只实现适配器
TypeScript 面板只调用公开 API
未来的 C++ 适配器、C++ 插件和 C++ 面板都复用同一套协议
```

## 总体架构

```text
                         ┌────────────────────┐
                         │ TypeScript 控制面板 │
                         └─────────┬──────────┘
                                   │ REST / WebSocket
                         ┌─────────▼──────────┐
                         │   C++ Control API  │
                         └─────────┬──────────┘
                                   │
┌────────────────────┐   gRPC   ┌─▼─────────────────────┐
│ Python NoneBot2    │◄────────►│       C++ Core         │
│ OneBot 适配器      │          │ 事件、权限、插件、数据 │
└─────────┬──────────┘          └─┬───────────┬──────────┘
          │ OneBot WebSocket       │           │
          ▼                        │           │
      NapCatQQ              C++ 原生适配器   C++ 插件
```

## 分层设计

### 第一层：外部平台

包括：

- QQ；
- NapCatQQ；
- 未来的 Telegram、Discord 或官方 QQ 接口。

这些都不进入核心业务代码。

### 第二层：渠道适配器

定义统一的 `ChannelAdapter` 接口。Python NoneBot2 适配器和未来 C++ QQ 适配器都实现同一套能力：

```text
connect()
disconnect()
publish_event()
send_action()
get_status()
get_capabilities()
```

Python 适配器内部可以使用：

```text
NoneBot2
nonebot-adapter-onebot
OneBot V11
```

但 C++ Core 不应该知道这些库的存在。

### 第三层：统一事件协议

所有平台消息先转换为统一事件：

```text
MessageReceived
MessageSent
GroupMemberChanged
FriendRequest
TimerTriggered
AdapterStatusChanged
```

建议使用 Protocol Buffers 定义：

```text
proto/
  kiarabot/v1/
    events.proto
    actions.proto
    adapters.proto
    plugins.proto
    common.proto
```

事件至少应包含：

```text
event_id
schema_version
adapter_id
account_id
channel
user_id
conversation_id
timestamp
message_segments
trace_id
```

这样 Python 和 C++ 不共享实现代码，只共享协议定义。

### 第四层：C++ 核心服务

C++ Core 是整个系统的中心，负责：

- 事件接收和分发；
- 命令路由；
- 权限检查；
- 插件调度；
- 会话管理；
- AI 请求编排；
- 任务调度；
- 统一消息发送；
- 配置管理；
- 日志和审计；
- 控制面板 API。

C++ Core 只依赖自己的接口和协议，不依赖 NoneBot2。

### 第五层：插件运行时

插件不直接调用 NapCat、NoneBot 或具体模型 SDK，而是使用 Plugin SDK：

```text
PluginContext
MessageService
ModelService
StorageService
PermissionService
SchedulerService
ConfigService
```

插件清单建议包含：

```json
{
  "id": "ai-chat",
  "version": "1.0.0",
  "api_version": "kiarabot.plugin.v1",
  "permissions": [
    "message.read",
    "message.send",
    "model.chat",
    "storage.conversation"
  ],
  "config_schema": "..."
}
```

插件必须声明权限，控制面板可以启用、禁用和配置插件。

### 第六层：基础设施适配器

核心通过接口调用基础设施：

```text
IMessageBus
IModelProvider
IConversationStore
IUserRepository
IPermissionStore
IScheduler
IFileStorage
IAuditLogger
```

具体实现可以是：

```text
SQLiteAdapter
PostgresAdapter
OpenAICompatibleModelAdapter
RedisAdapter
CronSchedulerAdapter
```

未来更换数据库或模型供应商时，不需要修改核心业务。

### 第七层：控制面板 API

控制面板后端直接由 C++ Core 提供，或者初期单独作为 C++ API 服务。

建议分成：

```text
REST API：
  登录、配置、插件管理、权限管理、机器人管理

WebSocket：
  实时日志、连接状态、消息流、任务状态
```

接口定义应独立保存为：

```text
api/
  openapi.yaml
```

这样未来 TypeScript、C++ 或其他语言都可以根据 OpenAPI 生成客户端。

控制面板至少需要管理：

- 机器人账号；
- NoneBot2 适配器连接；
- 插件安装、启用、停用；
- 插件配置；
- AI 模型和 API Key；
- 群组白名单；
- 用户和角色权限；
- 会话和上下文；
- 定时任务；
- 日志和审计；
- 运行状态和健康检查。

### 第八层：未来的 C++ 适配器

未来替代 NoneBot2 时，不修改 C++ Core，只新增：

```text
adapters/
  cpp_qq/
```

这个适配器直接实现：

```text
QQ / OneBot / 官方协议
        ↓
统一 EventEnvelope
        ↓
C++ Core
```

核心只判断它是否实现了 `ChannelAdapter`，不关心它是 Python 进程、C++ 动态库还是远程服务。

### 第九层：未来的 C++ 插件

这里建议同时支持两种模式：

```text
进程外插件：gRPC / RPC
进程内插件：C ABI 动态库
```

第一阶段优先进程外插件，因为它具备：

- 崩溃隔离；
- 独立升级；
- 多语言支持；
- 更容易限制权限；
- 不受 C++ ABI 影响。

如果以后需要高性能，再提供 C++ SDK：

```cpp
extern "C" PluginHandle* plugin_create();
extern "C" void plugin_destroy(PluginHandle*);
extern "C" int plugin_get_manifest(char* buffer, size_t size);
```

不要直接用 C++ 类作为插件 ABI，因为不同编译器、标准库和编译参数可能造成 ABI 不兼容。

## 推荐仓库结构

```text
KiaraBot/
  core/
    domain/
    application/
    runtime/
    services/

  contracts/
    events/
    actions/
    ports/
    plugin_api/

  proto/
    kiarabot/v1/

  adapters/
    nonebot_python/
    cpp_qq/
    ai/
    storage/

  plugins/
    ai_chat/
    group_admin/
    reminder/

  control/
    api/
    auth/
    websocket/

  panel/
    web/

  sdk/
    cpp/
    python/
    typescript/

  deploy/
    docker/
    systemd/

  docs/
  pyproject.toml
  CMakeLists.txt
```

## 进程拆分

进程建议先拆成：

```text
kiarabot-core
kiarabot-adapter-nonebot
kiarabot-panel
NapCatQQ
```

其中：

- `kiarabot-core`：C++；
- `kiarabot-adapter-nonebot`：Python；
- `kiarabot-panel`：TypeScript 构建后的静态前端；
- NapCatQQ：独立安装和运行。

## 通信协议

通信协议建议这样定：

```text
Python Adapter ↔ C++ Core：gRPC 双向流 + Protobuf
控制面板 → C++ Core：REST + JSON
控制面板 ← C++ Core：WebSocket
插件 ↔ Core：先使用 RPC，后续再提供 C ABI
```

## 开发顺序

开发顺序建议分五期：

1. **协议期**：先定义 `events.proto`、`actions.proto`、插件清单和 OpenAPI。
2. **核心期**：实现 C++ Core、事件路由、权限和消息发送接口。
3. **适配器期**：把当前 Python NoneBot2 代码改成独立适配器进程。
4. **面板期**：实现机器人、适配器、插件和模型配置页面。
5. **替换期**：增加 C++ 原生适配器，验证它可以完全替代 Python NoneBot2。

## 最关键的决策

**不要先围绕 NoneBot2 设计核心，而要先定义自己的事件协议和适配器协议。** NoneBot2 只是第一种接入方式；未来 C++ 适配器、C++ 插件和 C++ 控制面板都应该复用同一套 `proto`、OpenAPI 和 Plugin SDK。

## 附录 A：当前仓库落地对照（2026-10-30 快照）

> 本附录只描述撰写时（2026-10-30，工作区 `8819bba` 之后）的仓库现状，用于发现基线与实现的差异，不构成任何契约变更。跨语言契约以 `proto/` 为准，HTTP/WebSocket 契约以 `api/` 为准，任务完成状态以 [`doc/16-development-tasks.md`](16-development-tasks.md) 为准。本附录与正文同样受保护，仅人类维护者可修订。

### A.1 目录对照

| 基线建议 | 当前仓库 | 差异说明 |
| --- | --- | --- |
| `core/{domain,application,runtime,services}` | `core/include/kiarabot/core/{domain,application,ports,runtime}/`、`core/src/{application,runtime}/`、`core/app/main.cpp` | 按头文件、实现和组合根拆分，没有独立的 `services/` 目录；服务编排位于 `application/`，端口单独放在 `ports/`（基线把端口放在 `contracts/ports/`）。 |
| `contracts/{events,actions,ports,plugin_api}` | 仓库没有 `contracts/` 目录；跨语言契约在 `proto/kiarabot/v1/`，Core 端口在 `core/include/kiarabot/core/ports/` | 基线中的 `contracts/` 目录未落地。[`doc/00-boundaries.md`](00-boundaries.md) 将 `contracts` 定义为协议模块的概念名称，不另设与 `proto/` 重复的实现目录。是否保留该概念名由人类维护者决定。 |
| `proto/kiarabot/v1/`（5 个文件） | `proto/kiarabot/v1/{common,events,actions,adapters,plugins,services}.proto`，另有 `proto/tests/` | 多出 `services.proto`（AdapterBridge / PluginBridge 的 gRPC 服务声明）；额外提供 descriptor 兼容性检查与跨语言 round-trip 测试。 |
| `adapters/{nonebot_python,cpp_qq,ai,storage}` | 仓库没有 `adapters/`；Python NoneBot2 适配器暂存于同级检出 `../kiarabot-adapter-nonebot/`，规划迁入 `adapters/nonebot/`；没有 C++ 适配器 | 适配器尚未入仓。基线的 `adapters/ai`、`adapters/storage` 在当前设计中属于 `infrastructure/`（模型 Provider、存储实现），不放在适配器目录。 |
| `plugins/{ai_chat,group_admin,reminder}` | `plugins/{event-observer,ai-reply}/`（Python 进程外插件，清单字段 `api_version=kiarabot.plugin.v1`、`runtime=python-rpc`） | 示例插件的命名和数量不同：`ai_chat` 对应 `ai-reply`；`group_admin`、`reminder` 尚未实现。 |
| `control/{api,auth,websocket}` | `control/api/`，认证、RBAC、REST、WebSocket 和健康映射在同一服务库内 | 采用 `control/api/` 单目录；控制 API 当前由 `kiarabot-core` 进程内托管（同进程 C++ 应用端口 `IControlApplication`），不是独立进程。基线允许“直接由 C++ Core 提供”，方向一致。 |
| `panel/web` | `panel/`（React + Vite + TypeScript；默认 mock，`VITE_API_MODE=live` 时连接真实 API） | 目录层级不同，功能对应。 |
| `sdk/{cpp,python,typescript}` | 只有 `sdk/python/`（进程外插件 SDK），没有 `sdk/cpp`、`sdk/typescript` | C++ / TypeScript SDK 尚未落地；面板通过 `api/openapi.yaml` 与 `api/asyncapi.yaml` 生成类型（`npm run generate`），尚未独立成 SDK 包。 |
| `deploy/{docker,systemd}` | `deploy/`（Docker Compose、Docker、systemd 骨架） | 职责一致；生产部署、备份恢复和发布镜像尚未验收。 |
| `docs/` | `doc/` | 仓库统一使用单数 `doc/`；另有根 `README.md`、`AGENTS.md`、`THIRD_PARTY_NOTICES.md`。 |
| `api/openapi.yaml` | `api/openapi.yaml` 与 `api/asyncapi.yaml` | WebSocket 契约单独拆为 AsyncAPI，是对基线的细化。 |

### A.2 分层对照

- **协议层**：`proto/kiarabot/v1/` 已作为跨语言唯一契约落地。事件使用 `EventEnvelope` 外层（`event_id`、`schema_version`、`occurred_at`、`trace_id`、`source`、`subject`、`payload oneof`、`sequence`、`delivery_mode`），覆盖基线要求的事件字段语义，但采用 oneof 判别而非扁平字段。
- **Core**：C++20 实现已存在，包括运行时、适配器接入、插件调度、Action、Host（存储/模型/任务）、任务调度、控制应用端口和 gRPC 桥接；契约与实现状态以 [`doc/02-core.md`](02-core.md) 和开发台账为准。
- **适配器**：`AdapterBridge` 已声明并经跨语言验收使用真实 gRPC；Python 适配器在独立检出中，尚未迁入本仓库。
- **插件运行时**：Python 进程外 SDK 与 `PluginBridge` 已落地，清单权限、事件 ACK 和 Host 能力协商已接线；C ABI 进程内插件尚未实现，与基线“第一阶段优先进程外插件”一致。
- **基础设施**：Core 端口定义在 `core/include/kiarabot/core/ports/`，`infrastructure/` 提供 SQLite RuntimeStore、HTTP 模型 Provider、时区解析和文件存储；基线的 PostgresAdapter、RedisAdapter、CronSchedulerAdapter 尚未实现（当前调度由 Core 的 `TaskScheduler` 承担）。
- **控制 API**：REST + WebSocket 已实现，由 `kiarabot-core` 进程内托管；契约拆分为 OpenAPI（REST）和 AsyncAPI（WebSocket），认证、RBAC、幂等和事件流语义见 [`doc/06-control-api.md`](06-control-api.md)。
- **控制面板**：TypeScript/React 面板已实现，默认 mock，可切换 live；尚未提供独立 TypeScript SDK。
- **未来 C++ 组件**：C++ 原生适配器、C++ 插件 C ABI、C++ SDK 和 C++ 控制面板客户端均未实现，属于延期或可选方向（参见 [`doc/14-implementation-assessment.md`](14-implementation-assessment.md)、[`doc/15-cross-layer-gaps.md`](15-cross-layer-gaps.md)）。

### A.3 需要人类维护者裁定的差异

1. 是否保留 `contracts/` 作为独立概念目录，还是继续以 `proto/` + `core/ports/` 承担其职责。
2. 基线的 `adapters/{ai,storage}` 与 `sdk/{cpp,typescript}` 目标目录，是否按基线调整现有 `infrastructure/` 和面板类型生成方式。
3. 基线只列 `api/openapi.yaml`，当前已增加 `api/asyncapi.yaml`；后续是否保持两份契约来源。
4. 基线建议的进程清单中未单列控制 API；当前控制 API 与 Core 同进程，是否长期保持。
5. 基线的事件字段是扁平列表，当前 `EventEnvelope` 采用 source/subject/oneof 分组；是否需要回到扁平结构（涉及契约兼容，不能随意改）。

## 附录 B：维护规则（仅人类维护者）

- 本文档的修订权属于 KiaraBot 人类维护者。AI 代理、代码生成工具和自动化流程一律只读。
- 允许 AI：读取、引用本文档；在交付说明中报告“实现与基线不一致”；向人类提出修订建议。
- 禁止 AI：编辑、重排、格式化、删除、合并、翻译本文档，或生成“合并版”“更新版”替换本文档；包括以修复链接、统一术语、批量文档整理为名义的改动。
- 本文档的结论不替代线协议：跨语言契约以 `proto/` 为准，HTTP/WebSocket 契约以 `api/` 为准。两者冲突时，AI 报告冲突，由人类维护者裁定。
- 人类修订时：更新文首“版本”和“生效日期”，在“变更记录”追加一行；若修订影响契约或模块文档，由人类维护者另行安排同步。
- 若发现 AI 已误改本文档，按版本管理恢复；不通过追加“更正”掩盖改动，以保证基线历史可信。

## 变更记录

| 版本 | 日期 | 维护者 | 变更 |
| --- | --- | --- | --- |
| v1.0 | 2026-10-30 | 人类维护者 | 冻结基线初版：正文由人类维护者提供；AI 在人类明确指示下完成排版整理，并补充附录 A 的当前仓库落地对照。此后仅人类维护者可修订。 |
