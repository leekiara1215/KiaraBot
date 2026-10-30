# KiaraBot Protocol Contracts

T06a 使用同一 features 协商字段启用 host.storage.v1，T06b 启用 host.model.v1，承载已定义的 HostRequest/HostResponse 操作；未协商不发送，其他未实现 Host operation 显式拒绝。未改变 Host 字段编号或类型，详细限额/TTL/错误见 doc/03-plugin-sdk.md 和 doc/05-infrastructure.md。

T04a 兼容新增插件功能协商（supported_features=3 / enabled_features=5）、PluginActionRequest.request_id=2、PluginToCore.action_query=6/action_cancel=7、CoreToPlugin.action_response=7。action.result.v1 经双向确认后才能使用；旧端缺省不启用。查询可按认证调用方作用域中的 Action ID 或幂等键进行。响应 oneof 区分结果与操作错误，未知传输结果不能生成 FAILED。详细语义见 doc/01-contracts.md；服务接线由 T04b 实现。

T03a 兼容新增 CoreToPlugin.error=6，承载注册后的终止错误。注册拒绝仍在 register_result.error；客户端不能把流关闭当作 ACK。插件订阅使用现有 capabilities 中的事件能力名，与 doc/03-plugin-sdk.md 中权限映射一致，不新增私有跨语言类型。

本目录是跨语言契约的唯一来源。C++ Core、Python NoneBot2 适配器、C++ 插件和其他客户端不得复制这些消息定义。

## 依赖

- `protoc` 25.x 或更高版本；
- `grpc_cpp_plugin` 来自与 gRPC runtime 相同的 release 系列，并通过实际代码生成验证兼容性；它不一定提供可解析的 `--version` 输出；
- `protobuf` 运行库 25.0+、`grpc` 运行库 1.60.0+，由 CMake 配置阶段检查。

## 生成 C++ 代码

```bash
cmake -S . -B build -DKIARABOT_GENERATE_PROTO=ON
cmake --build build --target kiarabot_proto
```

该目标会调用 `protoc` 和 `grpc_cpp_plugin`，输出到 `build/generated/cpp/kiarabot/v1/`。如果本机没有这两个可执行文件，配置阶段应失败，而不是生成不完整的代码。

协议兼容性检查器由 `kiarabot_descriptor_compatibility` 构建目标提供。它读取当前 descriptor 和上一版本 descriptor，按文件名、语法/包名、消息字段编号及完整字段元数据、oneof、嵌套类型、扩展/保留范围、枚举值和 gRPC 方法签名与选项进行语义比较；不比较 descriptor 二进制序列化结果。新增字段和枚举值允许，删除或改变已发布定义会失败。

## 生成 Python 代码

```bash
python -m grpc_tools.protoc \
  -I proto \
  --python_out=build/generated/python \
  --grpc_python_out=build/generated/python \
  proto/kiarabot/v1/*.proto
```

生成文件只允许出现在 `build/generated/`，不直接编辑生成文件。

## 兼容性规则

- `kiarabot.v1` 只做向后兼容的新增；
- 已发布字段编号不得复用；
- 删除字段先标记 deprecated，再在下一个主版本移除；
- 枚举新增值必须由旧客户端安全忽略；
- 每次协议变更必须运行 descriptor diff 和跨语言 round-trip 测试。

v1 的稳定枚举词汇也由 REST 和 WebSocket 契约复用：`ErrorCode`、`HealthState`、`PluginState` 和 `ActionStatus` 在 `api/openapi.yaml` 与 `api/asyncapi.yaml` 中必须逐项保持一致。
## 插件 Host API

T06c 协商 host.tasks.v1 后启用 task_create/get/cancel；timer 事件只投递给任务所有者，任务状态值使用既有 ScheduledTaskState。没有新增线协议字段，运行时语义见 doc/01-runtime-semantics.md。

模型、命名空间存储和一次性任务使用 `plugins.proto` 中的 HostRequest/HostResponse，通过已注册 PluginBridge 传递。鉴权、幂等、时区、状态及错误语义见 `doc/01-contracts.md` 和 `doc/01-runtime-semantics.md`。

## 桥接认证

Connect 使用单个 bearer metadata，错误详情携带序列化 Error；身份绑定、失败及尚未实现行为见 [契约说明](../doc/01-contracts.md#桥接连接认证t01b)。T01b 未改变字段编号或 RPC 签名。

## 适配器持久化确认（T02a）

CoreToAdapter 新增 oneof event_ack=4，类型 AdapterEventAck；原字段编号保持不变。PERSISTED/DUPLICATE 才允许适配器移除本地待确认事件；REJECTED 必须检查稳定 Error.code/retryable。未知字段兼容旧客户端，但旧客户端必须升级支持 event_ack 后才能可靠清理缓存。注册 protocol_version 和事件 schema_version 为 kiarabot.v1。

配置能力 `plugin.config.v1` 使用 PluginConfig.delivery_id/apply_timeout_ms 与 PluginToCore.config_ack=8（PluginConfigAck）。旧客户端不协商则不下发；确认、失败、重连语义见 doc/03-plugin-sdk.md。
