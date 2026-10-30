# 分阶段开发顺序

## 阶段 0：冻结边界

交付：

- `doc/00-boundaries.md`；
- `proto/kiarabot/v1/` 目录；
- `api/openapi.yaml` 初稿；
- 根目录 CMake 骨架和独立 Core smoke test；
- 模块依赖检查规则。

完成标准：任何新代码都能判断自己属于哪个模块。

## 阶段 1：协议层

先实现事件、Action、适配器注册、插件注册、错误码和版本协商。不要实现 QQ、AI 和控制面板。

## 阶段 2：C++ Core

使用模拟适配器、模拟插件和内存存储实现事件路由、权限和 Action 分发。Core 必须可以脱离 Python 和 NapCat 启动。

## 阶段 3：Python NoneBot2 适配器（本仓库 adapters/）

在 `adapters/` 下实现 Python 适配器，只做 OneBot 连接、事件转换和 Action 执行；不把 NoneBot2 细节或 Python 运行时依赖引入 Core。适配器当前暂存于同级检出，待迁入本仓库；后续可再考虑拆成单独插件。

推荐的目标结构：

```text
adapters/nonebot/
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

## 阶段 4：PluginSDK

先完成 Python 进程外 SDK、fake host、插件清单、权限和生命周期。AI 插件作为第一个真实插件。

## 阶段 5：基础设施

接入 SQLite、模型 Provider、会话存储、任务调度和审计日志。插件不直接访问这些依赖。

## 阶段 6：控制面板 API

先完成 OpenAPI 和 mock Core，再连接真实 Core。控制 API 优先实现认证、RBAC、插件和适配器管理。

## 阶段 7：TypeScript 面板

使用 mock API 并行开发，最后接入真实 API 和 WebSocket。

## 阶段 8：C++ 替代实现

实现 C++ 原生渠道适配器，使用同一套协议接入 Core。通过回归测试证明 Python 适配器可以被替换。

## 阶段 9：C++ 插件和控制客户端

按性能和部署需求增加进程外 C++ 插件、进程内 C ABI 插件或 C++ 控制面板客户端。

## 每个阶段的完成条件

- 单元测试通过；
- 协议兼容测试通过；
- 模块可以单独启动；
- 模块可以使用 fake 依赖测试；
- 模块边界没有新增反向依赖；
- 文档中的代码启动句可以实际执行。

## 第一批推荐任务

按以下顺序开始最稳妥：

1. 协议定义；
2. C++ Core 空壳；
3. Python 适配器桥（本仓库 `adapters/`）；
4. Python PluginSDK（独立 SDK 包）；
5. AI 插件；
6. SQLite 和模型适配器；
7. 控制 API；
8. TypeScript 面板。
