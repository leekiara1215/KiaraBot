# KiaraBot

KiaraBot 是一个面向 QQ 的可扩展机器人平台。当前仓库保存 C++ Core、跨语言协议、PluginSDK 和控制面板的设计；Python NoneBot2 适配器作为独立组件接入，不与 Core 业务代码混在一起。

## 目标架构

```text
QQ
│
└─ NapCatQQ                         独立 QQ 协议端
   │ OneBot V11 / WebSocket
   ▼
Python NoneBot2 适配器               可替换渠道适配器
   │ gRPC / Protobuf
   ▼
C++ Core                            事件、权限、插件、数据和业务编排
   │
   ├─ PluginSDK                     Python/C++ 插件边界
   ├─ 基础设施适配器                 模型、数据库、任务和日志
   └─ Control API                   REST / WebSocket
       ▼
TypeScript 控制面板                 第一个控制面板客户端
```

未来可以把 Python NoneBot2 适配器替换为 C++ 适配器，把 Python 插件替换为 C++ 插件，把 TypeScript 面板替换为 C++ 客户端，而不改变公共协议。

## 当前仓库状态

当前开发保持 C++ Core、独立 Python 适配器/SDK 和 TypeScript 面板；纯 C++ 替换为未来可选路线。任务依赖、验收和实现进度见 [开发台账](doc/16-development-tasks.md)。Core 无参数入口在 POSIX 平台保持存活，SIGINT/SIGTERM 正常退出；`--check` 执行一次性自检。持续存活不代表 RPC 或健康服务已就绪。

详细的实现对照、文档导航及纯 C++ 演进清单见 [实现现状与纯 C++ 演进评估](doc/14-implementation-assessment.md)。

当前仓库不包含 Python 业务实现。NoneBot2、OneBot 适配器和 NapCatQQ 都按独立组件处理；它们只通过设计文档中规定的协议和适配器边界接入。

仓库现在包含一个不依赖第三方 SDK 的 C++ Core 构建骨架，以及实际的 Protobuf、OpenAPI 和 AsyncAPI 契约：

```bash
cmake -S . -B build
cmake --build build
```

## 设计文档

所有架构和开发文档统一放在 [`doc/`](doc/README.md) 目录，不再拆分 `docs/` 和 `doc/`：

- [分层开发规范索引](doc/README.md)
- [AI 开发入口规则](AGENTS.md)
- [C++ 主编码规范](doc/12-cpp-coding-standard.md)
- [开发边界与协作规则](doc/00-boundaries.md)
- [统一协议层](doc/01-contracts.md)
- [实际 Protobuf 契约](proto/README.md)
- [实际 OpenAPI 契约](api/openapi.yaml)
- [实际 AsyncAPI 契约](api/asyncapi.yaml)
- [事件运行时语义](doc/01-runtime-semantics.md)
- [C++ Core](doc/02-core.md)
- [PluginSDK 与插件运行时](doc/03-plugin-sdk.md)
- [Python NoneBot2 适配器](doc/04-nonebot-adapter.md)
- [基础设施适配器](doc/05-infrastructure.md)
- [控制面板 API](doc/06-control-api.md)
- [TypeScript 控制面板](doc/07-control-panel.md)
- [部署与可观测性](doc/08-operations.md)
- [分阶段开发顺序](doc/09-roadmap.md)
- [开发工具链版本](doc/11-toolchain.md)
- [CI/CD 流程方案](doc/12-ci-cd.md)

每份模块文档都写明了责任边界、输入输出、允许和禁止的依赖、验收标准，以及可以直接作为独立编码任务描述的“代码启动句”。

## 独立组件和许可证

请将 NapCatQQ、NoneBot2 和 OneBot 适配器分别安装和运行，不要把 NapCatQQ 源码复制到本仓库，也不要把它重新打包成 KiaraBot 的一部分。第三方组件及其许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

KiaraBot 自有代码采用 MIT 许可证，许可证文本见 [LICENSE.md](LICENSE.md)。
