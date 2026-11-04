# KiaraBot 分层开发规范

这组文档把 KiaraBot 拆成互相隔离的开发单元。每个单元都有明确的责任、输入、输出、依赖和禁止事项，开发时只实现当前单元，不直接修改其他单元的内部实现。

## 当前入口与文档口径（2026-11-04）

当前目标服务入口为 `core/app/{identity,runtime,host,scheduler,model}_main.cpp` 与
`control/api/app/main.cpp`，六类进程通过目标协议交互；内部领域、应用和端口位于
`core/{include/kiarabot/core,src}/{domain,application,ports}/target/`，传输适配位于
`core/{include/kiarabot/core,src}/runtime/platform/`。部署和启动参数查
[运维文档](08-operations.md)；实现范围、本机实测、未验收与延期项统一查
[阶段证据矩阵](27-phase-exit-evidence.md)，原始报告保留其提交与日期作用域。

历史一体 Core 已隔离到 `core/reference/`，只用于 reference 构建和行为依据。
[单机 MVP](24-single-host-mvp.md)、[旧开发交接](25-development-handoff.md) 以及
下文首版示意不是六服务当前启动入口。设计文档 19–22 保留 2026-11-01 的方向与设计快照，
页内“未实施”属于当时结论；现行线协议以 `proto/`、HTTP/WS 以 `api/` 为准。

2026-11-04 新增批准的 Control WS、模型 streaming、Host 错误语义/独立 operation 与
bootstrap 交付见 [五项扩展计划](29-approved-capability-expansion-plan.md)。
主代理统筹，GPT-6.1 Sol/high subagent 执行；批准纳入计划不等于当前实现已可用。

## 首版开发目标（历史示意）

```text
外部平台
  ↓
渠道适配器
  ↓
统一协议
  ↓
C++ Core
  ↓
PluginSDK / 插件
  ↓
基础设施适配器
  ↓
控制面板 API
  ↓
TypeScript 控制面板
```

首版阶段使用以下进程（历史）：

```text
NapCatQQ                    外部 QQ 协议端
kiarabot-adapter-nonebot   Python NoneBot2 / OneBot 适配器
kiarabot-core              C++ 核心服务
kiarabot-panel             TypeScript 控制面板
```

未来可以把 Python 适配器替换为 C++ 适配器，把 RPC 插件替换为 C++ 插件，把 TypeScript 面板替换为 C++ 客户端，而不修改统一协议和核心业务规则。

## 目录组织

```text
proto/             跨语言协议
api/               OpenAPI / AsyncAPI 契约
core/              C++ Core、端口和运行时
infrastructure/    Core 端口的具体实现
sdk/               Python SDK（C++ 和 TypeScript SDK 为规划）
plugins/           可选业务插件
adapters/          同仓库渠道适配器
control/api/       控制面板 API 服务
panel/             TypeScript 控制面板
tools/             插件与契约工具（规划）
deploy/            Compose、Docker 和 systemd
scripts/           检查与运维脚本
tests/             跨模块测试
doc/               架构和开发规范
```

Python NoneBot2 适配器由本仓库 `adapters/nonebot/` 维护，不把实现放进 `core/`、`plugins/` 或 `panel/`；后续可再考虑拆成单独插件。

## 文档索引

当前验收状态以 [阶段证据矩阵](27-phase-exit-evidence.md) 为入口；
[发布清单](28-p14d-release-checklist.md) 保留发布要求与历史门禁，不能代替本轮工作区验证。

[开发台账](16-development-tasks.md) 用于追溯任务；当前实施与验收状态见证据矩阵和源码。[实现现状与纯 C++ 演进评估](14-implementation-assessment.md)、[跨层未完成事项清单](15-cross-layer-gaps.md) 以及两份 13-* 交接文档均为历史快照，不是现行清单。下列模块文档同时包含目标设计和阶段记录。

- [C++ 主编码规范](12-cpp-coding-standard.md)
- [开发边界与协作规则](00-boundaries.md)
- [统一协议层](01-contracts.md)
- [事件运行时语义](01-runtime-semantics.md)
- [C++ Core](02-core.md)
- [PluginSDK 与插件运行时](03-plugin-sdk.md)
- [Python NoneBot2 适配器](04-nonebot-adapter.md)
- [基础设施适配器](05-infrastructure.md)
- [控制面板 API](06-control-api.md)
- [TypeScript 控制面板](07-control-panel.md)
- [部署与可观测性](08-operations.md)
- [分阶段开发顺序](09-roadmap.md)
- [编码任务模板](10-task-template.md)
- [开发工具链版本](11-toolchain.md)
- [CI/CD 流程方案](12-ci-cd.md)
- [控制 API 契约交接（历史快照，待确认）](13-control-api-contract-handoff.md)
- [基础设施接入交接（历史快照）](13-infrastructure-integration-follow-up.md)
- [SDK 跨模块依赖与协议缺口（待核验）](../sdk/PLUGIN_SDK_DEPENDENCIES.md)
- [实现现状与纯 C++ 演进评估（历史快照）](14-implementation-assessment.md)
- [跨层未完成事项清单（历史快照）](15-cross-layer-gaps.md)
- [账号封禁风险评估与分层控制建议](17-account-ban-risk.md)
- [目标架构基线（人类维护，AI 只读）](18-architecture-baseline.md)
- [本轮架构方向基线（2026-11-01 方向快照）](19-architecture-direction-baseline.md)
- [详细架构设计总览（2026-11-01 设计快照）](20-detailed-architecture-design.md)
- [服务通信与方法设计（2026-11-01 设计快照）](21-service-contract-design.md)
- [事务、数据模型与初始化设计（2026-11-01 设计快照）](22-transactions-and-migration-design.md)
- [新架构分步实施计划（P00–P14，实现状态见 P14d 矩阵）](23-implementation-plan.md)
- [P04–P14 阶段退出证据矩阵（P14d）](27-phase-exit-evidence.md)
- [P14d 发布清单与发布门禁](28-p14d-release-checklist.md)
- [已批准的五项能力扩展计划（2026-11-04）](29-approved-capability-expansion-plan.md)
- [五项扩展契约冻结记录与实施放行（2026-11-04）](30-capability-expansion-contract-freeze.md)

## 依赖方向

```text
TypeScript 面板 ── HTTPS REST ──► Control
                                   │ 目标 RPC / mTLS / 受限委托
                                   ├─► Identity
                                   ├─► Runtime
                                   ├─► Scheduler
                                   └─► Model

Python 适配器 ◄── AdapterBridge ──► Runtime ◄── PluginBridge ──► SDK / 插件
                                     │
                                     ├─► Plugin Host（期望状态 / 执行观察）
                                     ├─► Scheduler（任务）
                                     └─► Model（模型）

Core 内层 ──► 自有端口 ◄── 基础设施实现（各 owner 私有库 / 密码学 / Provider）
```

所有模块都可以依赖 `proto/`，但不能依赖其他模块的内部目录。基础设施是 Core 端口的实现分支，不是插件的下一层，也不接收插件直接调用。

## 每个模块的固定交付物

开始一个模块前，应先写清楚：

1. 它实现哪一个接口或协议；
2. 它拥有哪一类数据；
3. 它接收什么输入、产生什么输出；
4. 它不允许做什么；
5. 它的健康检查如何判断成功；
6. 它有哪些单元测试、协议测试和故障测试；
7. 它如何被单独启动或替换。

每个文档末尾都有一句“代码启动句”。以后可以直接把这句话作为一次独立编码任务的描述。

- [分层开发任务与实施台账](16-development-tasks.md)
