# 开发工具链版本

本文件区分“仓库要求”和“本次检查到的环境”。检查日期：2026-10-29，时区：Asia/Shanghai。

## 仓库要求

| 工具 | 要求 | 用途 |
|---|---:|---|
| CMake | 3.24+ | 配置 C++ 工程、CTest 和协议生成目标 |
| GNU Make | 3.81+（或等效 CMake generator） | 默认 Unix 构建后端 |
| C++ 编译器 | C++20；AppleClang 15+、Clang 16+ 或 GCC 13+ | C++ Core |
| Protocol Buffers | protoc 25+ | 生成 C++/Python 契约代码和 descriptor |
| gRPC C++ plugin | 与 protoc 同代版本 | 生成 gRPC C++ 代码 |
| protobuf runtime | 25.0+，遵守 protoc 的官方跨版本兼容窗口 | Core 链接的 Protobuf 运行库 |
| gRPC runtime | 1.60.0+，与 grpc_cpp_plugin 使用同一 gRPC release 系列 | Core 链接的 gRPC 运行库 |
| Node.js | 20 LTS+ | TypeScript 控制面板 |
| npm | 10+ | 前端依赖管理 |
| Python | 3.11+（仅独立适配器） | NoneBot2 适配器和 Python SDK |

最低版本由根目录 `CMakeLists.txt`、`CMakePresets.json` 和 `proto/README.md` 共同约束。依赖必须锁定在实现项目的 lockfile 或包管理配置中。

## 当前检查到的环境

| 工具 | 检查结果 |
|---|---|
| AppleClang | 21.0.0，可编译 C++20 smoke test |
| GNU Make | 3.81 |
| GCC/G++ | 命令存在，未作为本次构建编译器验证 |
| CMake | 4.4.5，已完成 dev/proto configure、build 和 CTest |
| protoc | 36.2，已生成 descriptor、C++/gRPC 源码并通过 round-trip |
| grpc_cpp_plugin | gRPC 1.85.0 提供；该版本不支持 `--version` 参数，已通过实际代码生成验证 |
| Node.js | v26.12.0，满足 Node.js 基线 |
| npm | 11.20.0，满足 npm 基线 |
| Redocly CLI | 2.72.1，本次 OpenAPI lint 使用的版本 |
| AsyncAPI parser | 3.6.6，本次 AsyncAPI 2.6.0 语义校验使用的版本 |
| Python / grpc_tools | 本仓库当前无 Python 实现；独立适配器项目单独检查 |

缺失工具必须在执行 `proto` 预设前安装；CMake 配置不会静默跳过协议生成。

## 推荐验证命令

基础 Core：

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

协议生成和 descriptor：

```bash
cmake --preset proto
cmake --build --preset proto --target kiarabot_proto
ctest --preset proto
```

直接检查工具版本：

```bash
cmake --version
${CXX:-c++} --version
protoc --version
command -v grpc_cpp_plugin  # 插件本身不提供 --version；版本随 gRPC 包核对
node --version
npm --version
```

仓库也提供了统一检查脚本：

```bash
./scripts/check-toolchain.sh
```

## 版本更新规则

- C++ 标准固定为 C++20，不随单个编译器升级改变；
- protoc 与 protobuf runtime 必须遵守官方跨版本兼容窗口，生成和运行时优先使用同一 Protobuf release；grpc_cpp_plugin 与 gRPC runtime 使用同一 gRPC release 系列，不要求它们与 Protobuf 使用相同的数字主版本；
- 升级 CMake 或编译器必须运行 Core smoke test 和协议 round-trip 测试；
- 升级协议工具必须重新生成 descriptor，并检查字段编号和服务签名差异；
- 当前环境的实际版本只记录在本文件，不把本机路径写进 CMake。

## 代码启动句

> 请先按照 `doc/11-toolchain.md` 安装并核对 CMake 3.24+、C++20 编译器、protoc 25+、匹配的 grpc_cpp_plugin、Node.js 20+ 和 npm 10+，再使用 `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` 验证 Core。
