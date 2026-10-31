# PluginSDK 依赖和当前实现边界

协议唯一来源为 `proto/kiarabot/v1/`。本文件记录已实现与后续任务，不另定义替代线协议。

## T03b 已实现

- `sdk/python/` 使用固定 Python 3.11.18、uv.lock 与生成的 Protobuf/gRPC 类型；不导入 Core、NoneBot、数据库或模型 SDK。
- 清单支持 `python-rpc` / `process`、静态事件订阅、`ordered` 与权限申请。SDK 本地校验与 Core 对齐：capabilities 只能取 6 个已知事件名并映射所需权限，权限名/数量/重复、`module:factory` entrypoint 均校验；`ordered` 由 Core 持久化并在重连时比对，SDK 原样转发。依赖清单和非空配置 schema 显式拒绝；其他运行时尚未实现。
- `Plugin.on_start/on_event/on_stop`、协作超时、异常转换、事件 ACK、心跳、重连实例更新与完整权限重新校验。
- Core POSIX 启动器负责启动/停止硬超时、进程组回收与环境隔离；SDK 不假装能用 asyncio 中断同步阻塞的 Python 代码。
- `kiarabot_sdk.testing.FakeHost` 是本机 gRPC 测试夹具，覆盖注册/事件/心跳/Action/Host/配置帧的成功与故障交互，`heartbeat_acks=False` 可验证 io_timeout 重连；不替代 Core 权限、持久化和重试规则。验收同时运行真实 C++ Core 子进程。
- 运行时参数可通过 `PLUGIN_IO_TIMEOUT`、`PLUGIN_HOOK_TIMEOUT`、`PLUGIN_HEARTBEAT_INTERVAL`、`PLUGIN_RETRY_DELAY` 配置（默认 5/5/1/1），非法值 INVALID_ARGUMENT；Core 受管启动器尚未透传（交接 core）。
- 示例插件：`plugins/event-observer`（最小收事件）、`plugins/ai-reply`（模型+Action）、`plugins/group_admin`（群命令与命名空间存储；Mute/Kick/Approve 未实现）、`plugins/reminder`（Host 任务 + TimerTriggered），均只使用现有协议能力。启动和测试方式见 [Python SDK README](python/README.md)。

## 后续依赖

- T04a/T04b 已实现：Action 功能协商、结果/查询/取消、Core 持久化分发、适配器纯文本发送及 SDK 客户端；未知网络结果不报告平台失败。
- T06a 已实现：Host 存储分发、命名空间隔离、TTL/限额、事务幂等与审计、SDK 接线；使用 host.storage.v1 协商。
- T06b 已实现：HttpModelProvider → host.model.v1 → `Context.model_chat` 非流式接线，模型凭据留在基础设施；流式模型仍未实现。
- T06c 已实现：持久化调度 + `Context.task_create/get/cancel`；每个插件任务上限固定 1024。
- 配置应用确认已通过 plugin.config.v1 / PluginConfigAck 接线；REST 查询区分持久化与当前连接已应用，详见 doc/03-plugin-sdk.md。
- 动态订阅、依赖解析、模型流、会话管理、权限查询、native/wasm 运行时尚未实现；不能由现有消息类型推定可用。
- 插件发现/多实例托管、插件日志帧、健康通道、后台自动重启目前只在 Core/运维侧规划，本 SDK 无对应协议入口（交接 core/proto/ops）。
- `FakeEvent`/`FakeMessageService`/`FakeModelService`/`FakeStorage`/`FakeScheduler` 测试夹具、`tools/kiara-plugin` 脚手架、`sdk/cpp`、`sdk/typescript` 均未实现。

新增能力必须先冻结公共协议、错误/状态和协商方式，再分别实现各模块。新增 oneof 字段不等于旧客户端可理解；未协商功能必须明确失败。SDK 只能通过公开协议访问宿主能力，身份与资源命名空间由认证上下文决定。
