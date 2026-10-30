# 实现现状与纯 C++ 演进评估

- 评估日期：2026-10-30；基线：工作区 `618423e`。
- 范围：本仓库的文档、契约、构建入口、Core、基础设施、面板及部署配置。独立 NoneBot2 适配器和 NapCat 未检查，不能据此判断其实现或替换成本。
- 定位：这是现状快照与待办，不改变 `proto/`、`api/` 的契约，也不将建议视为已批准的设计。目标和验收以各模块文档为准；具体字段以协议文件为准。

实施更新：用户确认当前保留混合语言架构；后续代码状态以 [开发台账](16-development-tasks.md) 为准。下述为实施前快照。

## 总体判断

架构方向支持替换语言，但目前是**部分实现的原型，而非可运行的完整机器人**。Core 的事件、逐插件投递和 Action 状态机有 C++ 参考实现和单元测试；协议和 mock 面板可供并行开发。真实消息入口、插件进程、业务授权、持久化和控制 API 尚未组成端到端链路。测试通过仅证明所执行的组件测试，不能证明 QQ 消息收发、重启恢复、安全或生产部署可用。

## 文档导航与状态

- **规则与边界**：[AGENTS.md](../AGENTS.md)、[C++ 规范](12-cpp-coding-standard.md)、[模块边界](00-boundaries.md)。
- **唯一契约**：[Protobuf](../proto/README.md)、[OpenAPI](../api/openapi.yaml)、[AsyncAPI](../api/asyncapi.yaml)；解释与运行语义见 [契约](01-contracts.md)、[运行时语义](01-runtime-semantics.md)。
- **设计与验收**：[Core](02-core.md)、[SDK](03-plugin-sdk.md)、[适配器](04-nonebot-adapter.md)、[基础设施](05-infrastructure.md)、[控制 API](06-control-api.md)、[面板](07-control-panel.md)、[运维](08-operations.md)。文中目录树、示例及代码启动句描述目标，不代表已经存在。
- **规划与执行**：[路线图](09-roadmap.md)、[任务模板](10-task-template.md)、[工具链](11-toolchain.md)、[CI/CD](12-ci-cd.md)。路线图是阶段顺序，不是完成状态表。
- **待确认或后续事项**：[控制 API 交接](13-control-api-contract-handoff.md)、[基础设施接入](13-infrastructure-integration-follow-up.md)、[SDK 协议缺口](../sdk/PLUGIN_SDK_DEPENDENCIES.md)。这些是建议或特定时点的交接材料，先与现行契约核对，不能直接认定仍全部有效；例如 SDK 缺口文档中的 Host API 建议需与当前 `proto/kiarabot/v1/` 中的消息和服务声明核对。

## 对照实现

1. **协议：部分就绪。** `proto/kiarabot/v1/`、`api/` 存在契约；`services.proto` 声明 AdapterBridge 和 PluginBridge。生成和序列化测试存在，不代表这些服务已运行。控制 API 交接文档列有会话、幂等、分页及流恢复等待确认事项，需逐项对照现行契约重新核验。
2. **Core：状态机原型，不是完整服务。** [runtime.h](../core/include/kiarabot/core/runtime.h) 包含内存存储、事件去重、重试、死信、逐插件投递、Action 及插件状态；调用需串行。入口 [main.cpp](../core/src/main.cpp) 注入一个启动事件、打印后退出，没有持续监听或对外桥接。设计要求的真实路由、权限、插件宿主、Action 执行和健康服务尚未形成可运行链路。
3. **基础设施：局部实现。** [ports.hpp](../core/include/kiarabot/core/ports.hpp) 定义端口，[file_storage.cpp](../infrastructure/src/file_storage.cpp) 实现文件存储和 fake；其 POSIX 接口还需跨平台替代或封装。SQLite 持久化 RuntimeStore、迁移、模型、调度与审计等尚待实现和接线。根 CMake 未加入 infrastructure，当前需独立构建，不能推断它已用于 Core。
4. **适配器与 SDK：主要是契约和设计。** 仓库无适配器或插件实现目录，`sdk/` 为交接文档；独立 Python 组件是否存在、状态如何未核查。不能把协议声明当作 SDK、宿主或 C++ 适配器已完成。
5. **控制面：面板原型，服务缺失。** [panel/src/main.tsx](../panel/src/main.tsx) 默认使用 mock，支持配置 live；仓库没有 `control/api/` 实现。面板测试证明 mock 场景，不证明认证、权限或实时恢复的服务端行为。
6. **部署：配置骨架。** [Dockerfile](../Dockerfile) 构建一次性 Core 程序；Compose 的镜像摘要是占位值，健康探针依赖尚未提供的服务。详见 [运维限制](08-operations.md#当前部署配置与依赖)。不能按生产系统部署。

## 纯 C++ 的边界与替换清单

建议先定义为“KiaraBot 自有生产运行时全部采用 C++20”：Core、渠道适配器、插件宿主与插件、基础设施、控制 API 和客户端均为 C++；协议文件、构建工具、测试脚本及独立外部服务可保留其他语言。这是评估假设，不是架构决策。

- **渠道适配器（必需）**：用 C++ 实现 OneBot V11 WebSocket 连接、事件映射、Action 执行、重连和能力报告，接入现有 AdapterBridge。以同一事件/Action 语料比较 Python 方案与 C++ 方案，覆盖重复、断线、超时及权限；独立 Python 实现尚未核查，不能估计迁移量。
- **插件与 SDK（必需）**：先落实进程外 C++ RPC SDK、fake host、插件宿主、授权和真实插件。进程内 C ABI 是可选扩展，须另定 ABI、生命周期和隔离策略；不要假设 C++ 本身解决崩溃隔离。Python 插件若实际存在，需要逐个迁移并回归。
- **控制面（取决于范围）**：用 C++ 实现控制 API 和安全边界；若客户端也必须 C++，以原生 GUI 或 C++ 命令行客户端替代 `panel/`，按 OpenAPI/AsyncAPI 验证功能等价。浏览器执行 JavaScript 的 Web 面板无法仅靠 C++ 源码原样保留，需选择原生客户端或接受 Web 技术。当前 TypeScript 面板是实际存在的非 C++ 产品代码。
- **Core/基础设施（补齐而非语言替换）**：实现持久化事务与恢复、授权、gRPC 桥、调度、模型 Provider、审计、服务生命周期及健康检查。现有 C++ 文件存储须接线并评估 POSIX 移植性。
- **外部 QQ 协议端（范围决策）**：NapCat 是独立组件；C++ OneBot 适配器替换 NoneBot2 并不会自动替换 NapCat。若要求所有部署进程均为 C++，还需另选或实现 QQ 协议端，并单独验证功能、维护和合规条件。
- **严格全仓库 C++（额外范围）**：还须处理 TypeScript/Node 面板及生成工具、Python/Ruby 契约测试、shell 运维与质量脚本、CMake、YAML、Dockerfile、Protobuf 定义及第三方库和系统工具。某些是声明式配置或构建工具而非运行时代码；若字面要求所有文件及依赖均为 C++，需要先明确可接受的例外，否则目标不可操作。

跨层工作包及统计口径见 [跨层未完成事项清单](15-cross-layer-gaps.md)。

## 建议实施顺序与验收门槛

1. **冻结范围与契约**：决定 GUI、NapCat 和开发工具是否属于纯 C++ 范围；梳理交接建议与现行契约差异，先确认错误、认证、恢复和兼容语义。
2. **形成最小 C++ 纵向链路**：持久运行 Core，接入可恢复的持久化存储、fake 适配器和 fake 插件，贯通事件→授权→插件→Action→结果；验证崩溃恢复、幂等、超时和拒绝。
3. **落地 C++ 适配器与 SDK**：基于冻结协议完成真实进程通信，使用共同契约测试和故障注入；再接入真实 QQ 协议端。不得以 mock 测试代替端到端验收。
4. **控制面和部署**：实现 C++ 控制服务与选定客户端，覆盖认证、RBAC、审计、断线恢复；补迁移、备份恢复、就绪及受控关闭，替换占位镜像。
5. **切换验收**：相同输入下验证事件/Action、错误码、顺序、去重和恢复语义；完成生产环境试运行和回滚演练，才移除旧组件。跨语言和旧版兼容需要保留到切换完成。

## 本次验证与限制

- `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`：3/3 通过。
- `cmake --preset proto && cmake --build --preset proto && ctest --preset proto`：6/6 通过；其中 descriptor 测试仅检查生成文件存在，不等于运行了旧版本兼容性门禁。
- `cmake -S infrastructure -B build/infrastructure && cmake --build build/infrastructure && ctest --test-dir build/infrastructure --output-on-failure`：1/1 通过。
- `cd panel && npm test -- --run`：4/4 通过，使用 mock。
- 本次未运行真实 QQ、独立适配器、端到端、生产部署、descriptor 基线兼容门禁或全量格式/静态分析；本次仅修改文档，未修改公共契约或代码。评估基于源码和上述局部测试，不是完整安全审计或逐项协议一致性证明。
