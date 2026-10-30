# PluginSDK 依赖和当前实现边界

协议唯一来源为 `proto/kiarabot/v1/`。本文件记录已实现与后续任务，不另定义替代线协议。

## T03b 已实现

- `sdk/python/` 使用固定 Python 3.11.18、uv.lock 与生成的 Protobuf/gRPC 类型；不导入 Core、NoneBot、数据库或模型 SDK。
- 清单支持 `python-rpc` / `process`、静态事件订阅、`ordered` 与权限申请。依赖清单和非空配置 schema 显式拒绝；其他运行时尚未实现。
- `Plugin.on_start/on_event/on_stop`、协作超时、异常转换、事件 ACK、心跳、重连实例更新与完整权限重新校验。
- Core POSIX 启动器负责启动/停止硬超时、进程组回收与环境隔离；SDK 不假装能用 asyncio 中断同步阻塞的 Python 代码。
- `kiarabot_sdk.testing.FakeHost` 是本机 gRPC 测试夹具，仅模拟注册/事件/心跳，不替代 Core 权限、持久化和重试规则。验收同时运行真实 C++ Core 子进程。
- `plugins/event-observer/` 是通过 SDK 收事件的最小示例。启动和测试方式见 [Python SDK README](python/README.md)。

## 后续依赖

- T04a/T04b 已实现：Action 功能协商、结果/查询/取消、Core 持久化分发、适配器纯文本发送及 SDK 客户端；未知网络结果不报告平台失败。
- T06a 已实现：Host 存储分发、命名空间隔离、TTL/限额、事务幂等与审计、SDK 接线；使用 host.storage.v1 协商。
- T06b：已有 model_chat 非流式契约，待受控 Provider 和 SDK 接线；模型凭据留在基础设施。
- T06c：已有 task_create/cancel/get 与时区契约，待持久化调度执行和 SDK 接线。
- 配置应用确认、动态订阅、依赖解析、模型流、会话管理、权限查询、native/wasm 运行时尚未实现；不能由现有消息类型推定可用。

新增能力必须先冻结公共协议、错误/状态和协商方式，再分别实现各模块。新增 oneof 字段不等于旧客户端可理解；未协商功能必须明确失败。SDK 只能通过公开协议访问宿主能力，身份与资源命名空间由认证上下文决定。
