# 开发边界与协作规则

## 目标

这一层不是业务代码，而是所有模块必须遵守的架构规则。它保证模块互不干涉，并允许未来替换实现语言。

## 模块分类

### 协议模块

目录：`proto/`

`contracts` 是协议模块的概念名称，不另设与 `proto/` 重复的实现目录。

负责定义跨进程、跨语言可见的消息和接口。它不能包含 QQ、AI、数据库或 Web UI 的实现。

### 核心模块

目录：`core/`

负责领域规则、事件编排、权限和插件调度。它不能导入 NoneBot2、NapCat、Python 运行时、浏览器代码或第三方模型 SDK。

### 适配器模块

目录：`adapters/`

负责把外部系统转换成统一协议。适配器不能把平台数据表、原始平台事件或第三方 SDK 对象泄漏给 Core。

Python NoneBot2 适配器由本仓库 `adapters/` 维护，不再作为独立仓库；当前实现暂存于同级检出，待迁入。后续可再考虑拆成单独插件。适配器只通过 `proto/` 接口与 Core 通信。

### 插件模块

目录：`plugins/`、`sdk/`

负责可选业务能力。插件必须通过 PluginSDK 访问消息、模型、存储、权限和任务服务，不能直接访问数据库、OneBot 或控制面板 API。

SDK 的实现位于 `sdk/`，具体插件位于 `plugins/`；Core 内的插件宿主属于 `core/runtime/`。

### 基础设施模块

目录：`infrastructure/`

负责实现 Core 在 `core/ports/` 定义的数据库、模型、调度、审计和测试替身，不决定业务规则，也不接收插件的直接调用。

### 控制面板模块

目录：`control/`、`panel/`

`control/api/` 负责 REST/WebSocket 服务，`panel/` 负责 TypeScript 客户端。面板不能实现消息路由、权限规则或 AI 业务逻辑；这些规则属于 Core。

### 运维模块

目录：`deploy/`、`scripts/`

负责启动、停止、配置、迁移、备份、健康检查和日志收集。运维脚本不能绕过 API 修改业务数据。

## 依赖规则

允许的依赖关系：

```text
proto       ← core / adapters / plugins / sdk
api         ← control/api / panel / sdk/typescript
core        ← control/api
core/ports  ← infrastructure
control/api ← panel
```

禁止的依赖关系：

```text
core        → NoneBot2 / NapCat / OpenAI SDK / 浏览器
plugin      → OneBot API / 数据库连接 / Core 内部类
panel       → C++ 源文件 / 数据库 / NapCat
adapter     → 插件业务逻辑 / 面板数据库
infrastructure → 插件业务逻辑 / 控制面板页面
```

## 数据所有权

- Core 拥有机器人、账号、会话、权限、插件状态和任务状态；
- Core 的应用服务拥有用例和事务边界；
- Repository 和 Unit of Work 接口由 Core 定义，具体数据库连接、迁移和事务实现由基础设施层负责；
- 适配器拥有平台连接状态和平台原始 ID 映射；
- 插件拥有自己的命名空间数据；
- 控制面板不拥有业务数据，只通过 API 读取或修改；
- 日志和审计由 Core 或运维层统一写入。

## 变更规则

- 跨模块变更先修改协议和接口，再修改实现；
- 不通过复制类型定义解决依赖问题；
- 不把临时字段写入公共协议后长期保留；
- 删除字段前先废弃一个版本；
- 每个模块必须提供独立测试和最小启动命令；
- 一个模块的实现可以替换，但公共协议和责任边界不能隐式改变。

## 统一的编码任务要求

任何可以交给开发者或代码代理的任务都必须包含：

```text
目标模块和目录
允许修改的文件
禁止修改的目录
依赖版本
输入、输出和错误语义
构建、代码生成和测试命令
验收条件
```

## 代码启动句

> 请只实现 `doc/` 中指定模块的公共接口和独立测试，严格遵守依赖方向，不修改其他模块的内部代码。
