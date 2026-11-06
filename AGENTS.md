# KiaraBot AI 开发规则

本文件是 KiaraBot 仓库的 AI 开发入口和强制规则。开始任何分析、修改或新增代码前，必须先阅读
本文件、[`docs/spec/coding-standard.md`](docs/spec/coding-standard.md) 与
[`docs/spec/boundaries.md`](docs/spec/boundaries.md)；涉及整体架构时阅读
[`docs/architecture.md`](docs/architecture.md)，涉及具体模块时阅读 `docs/modules/<module>.md`。

## 规则优先级

1. 用户在当前任务中明确提出的要求；
2. 本文件和 [`docs/spec/coding-standard.md`](docs/spec/coding-standard.md)；
3. [`docs/spec/boundaries.md`](docs/spec/boundaries.md)、[`docs/architecture.md`](docs/architecture.md)
   及对应模块文档；
4. 现有代码的局部惯例。

当现有代码与主规范冲突时，不要顺手进行大范围格式化；只在当前任务涉及的范围内遵守主规范，
并在变更说明中指出冲突。

## 开发前必须确认

- 先确定任务所属模块和允许修改的目录；
- 先阅读相关协议、架构、模块责任和工具链规范（[`docs/spec/toolchain.md`](docs/spec/toolchain.md)）；
- 不跨模块修改内部实现，不绕过公共协议和端口接口；
- 不把生成文件、构建产物、密钥或本机配置提交到仓库；
- 修改 C++ 后运行与变更相关的构建、测试和格式检查。

## 主规范

C++ 的命名、格式、现代 C++ 用法、资源管理、错误处理、测试和提交前检查，以
[`docs/spec/coding-standard.md`](docs/spec/coding-standard.md) 为准。格式化配置位于
[`.clang-format`](.clang-format)。任务拆分、交付要求与文档同步规则见
[`docs/spec/task-workflow.md`](docs/spec/task-workflow.md)；CI 与质量门禁见
[`docs/spec/ci-gates.md`](docs/spec/ci-gates.md)。

## 架构边界

- `proto/` 只定义跨进程、跨语言契约；`api/` 只定义 HTTP/WebSocket 契约；
- `core/` 负责领域规则、事件编排、权限和插件调度；
- 适配器负责外部平台转换，不能把平台对象泄漏给 Core；
- 插件只能通过 SDK 访问 Core 能力；
- 控制面板只能通过 API 访问业务数据；
- 基础设施实现 Core 定义的端口，不能决定业务规则。

## 受保护文档（AI 只读）

- 当前受保护清单为空。原人类冻结基线已按维护者授权并入 [`docs/architecture.md`](docs/architecture.md)，
  其冻结事实保留在该文档中。
- 若人类维护者后续重新指定受保护文档，AI 只能只读、引用、报告实现差异和提出修订建议，
  不得修改、重排、格式化、删除或替换其内容，也不得以“同步文档”为由编辑它。
- 未经人类维护者在当前任务中明确授权，AI 不得把任何文档加入或移出受保护清单。
- 跨语言契约以 `proto/` 为准，HTTP/WebSocket 契约以 `api/` 为准；发现冲突时报告人类，
  不得自行修改受保护文档或契约来“消除差异”。

## 交付要求

每个实现任务都应说明目标模块、输入输出、错误语义、允许修改的文件、构建命令、测试命令和
验收条件。完成修改后，报告实际运行过的检查及未运行检查的原因。

## 文档同步规则（AI 强制）

文档影响判断、范围与唯一来源、更新顺序和豁免条件以
[`docs/spec/task-workflow.md`](docs/spec/task-workflow.md) 第 4–5 节为准。要点：

- 任务开始前先列出需要同步的文档、无需同步的文档及理由；
- 改变外部可见的接口、协议、状态、错误语义、配置、运行方式或模块责任时，必须在同一变更中
  更新对应文档并运行校验，不能只改实现或只改测试；
- 跨语言契约唯一来源是 `proto/`，HTTP/WebSocket 契约唯一来源是 `api/openapi.yaml` 与
  `api/asyncapi.yaml`；模块行为文档是 `docs/modules/*.md`；
- 每次交付必须包含文档影响判断、实际更新的文档路径、未更新文档及理由、文档校验命令和结果。
