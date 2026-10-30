# KiaraBot Protocol Contracts

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
