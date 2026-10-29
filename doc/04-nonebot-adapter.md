# Python NoneBot2 适配器设计

## 责任

Python 适配器是第一种渠道适配器，负责连接 NapCatQQ，并把 OneBot V11 转换成 KiaraBot 统一协议。

它只做：

- 启动 NoneBot2；
- 注册 OneBot V11 适配器；
- 连接 NapCatQQ；
- 转换事件和消息段；
- 执行 Core 下发的 Action；
- 报告连接和平台能力。

## 目录

该适配器是独立 Python 项目，推荐结构如下：

```text
kiarabot-adapter-nonebot/
  pyproject.toml
  src/kiarabot_adapter_nonebot/
    onebot/
    event_mapper/
    action_executor/
    grpc_client/
    health/
    main.py
  tests/
```

本仓库只维护 `proto/` 契约，不把该项目的 Python 实现放入 `core/`、`adapters/` 或 `plugins/`。

它不做：

- AI 业务逻辑；
- 群组权限决策；
- Core 数据库访问；
- 插件调度；
- 控制面板页面。

## 进程边界

```text
NapCatQQ
  │ OneBot V11 / reverse WebSocket
  ▼
kiarabot-adapter-nonebot
  │ gRPC / Protobuf
  ▼
kiarabot-core
```

Python 适配器不与 C++ Core 共享数据库。它只保存本地连接缓存和必要的临时状态。

## 事件转换

OneBot 原始事件必须在适配器内完成转换：

```text
OneBot message
  → MessageReceived
  → Core
```

适配器需要处理：

- 文本、图片、回复、@、文件等消息段；
- 群聊和私聊会话 ID；
- 用户、群组和账号 ID 映射；
- 平台时间戳；
- 平台原始消息 ID；
- 不支持消息段的降级表示。

原始 OneBot JSON 只能出现在适配器日志或诊断接口中，不能进入插件 SDK。

## Action 转换

Core 下发统一 Action：

```text
SendMessage
MuteMember
KickMember
ApproveRequest
```

适配器负责把它们转换成 OneBot API 调用，并返回 `ActionResult`。权限由 Core 检查，适配器只检查平台能力和连接状态。

## 配置

适配器所需配置：

```text
CORE_GRPC_ENDPOINT
CORE_AUTH_TOKEN
ONEBOT_WS_URL
ONEBOT_ACCESS_TOKEN
ACCOUNT_ID
```

NapCat 的 WebUI 密码、OneBot Token 和 Core Token 必须分开管理。

## 故障处理

- Core 不可用时，适配器持续重连但使用退避；
- NapCat 断线时，适配器报告 `AdapterStatusChanged`；
- Action 超时必须返回 `TIMEOUT`；
- 不支持的 Action 返回 `CAPABILITY_NOT_SUPPORTED`；
- 适配器重连后重新注册能力和账号；
- 不在适配器内无限缓存消息。

## 替换标准

未来的 C++ QQ 适配器只要能实现相同的：

- 注册协议；
- EventEnvelope；
- Action；
- Capability；
- 心跳和错误码；

就可以替换本适配器，Core 和插件不应感知替换。

## 验收标准

- 能接收 NapCat 的文本、图片和 @ 消息；
- 能把统一 Action 发送回群聊和私聊；
- 断线可以自动重连；
- 不导入业务插件；
- 没有对 Core 数据库的直接访问；
- 可以用模拟 Core 独立测试事件转换。

## 代码启动句

> 请只实现独立的 Python NoneBot2 / OneBot V11 适配器进程，把 NapCat 事件转换为 `kiarabot.v1` 事件并执行 Core Action，使用模拟 gRPC Core 完成断线、重连、能力和消息段测试，不实现任何业务插件。
