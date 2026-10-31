# KiaraBot

KiaraBot 是一个面向 QQ 的可扩展机器人平台：C++20 Core 负责事件、权限、插件调度和业务编排，Protobuf/gRPC 契约隔离各进程，Python PluginSDK 支持进程外插件，TypeScript 控制面板通过 Control API 管理运行时。QQ 协议端 NapCatQQ 是独立组件；Python NoneBot2 适配器由本仓库 `adapters/nonebot/` 维护，二者都只通过公共协议接入。后续可再考虑把适配器拆成单独插件。

## 目标架构

```text
QQ
│
└─ NapCatQQ                          独立 QQ 协议端
   │ OneBot V11 / WebSocket
   ▼
kiarabot-adapter-nonebot            Python NoneBot2 适配器（本仓库 adapters/）
   │ gRPC / Protobuf
   ▼
C++20 Core                          事件、权限、插件调度、Action、持久化编排
   ├─ PluginSDK（Python/C++）        进程外插件边界
   ├─ 基础设施适配器                 SQLite、模型、时区、任务
   └─ Control API                   REST / WebSocket
       ▼
TypeScript 控制面板                 第一个控制面板客户端
```

协议和 API 契约是跨模块唯一来源；未来可以替换 NoneBot2 适配器、插件语言或面板客户端，而不改变公共协议。完整设计基线见 [目标架构基线（人类维护，AI 只读）](doc/18-architecture-baseline.md)。

## 仓库组成

| 路径 | 内容 |
| --- | --- |
| `proto/kiarabot/v1/` | 跨语言唯一契约：事件、Action、适配器、插件和 gRPC 服务 |
| `api/` | OpenAPI 与 AsyncAPI 契约 |
| `core/` | C++20 Core：Runtime、AdapterBridge/PluginBridge、插件调度、Action/Host、任务调度和控制应用端口 |
| `infrastructure/` | Core 端口实现：SQLite RuntimeStore、HTTP 模型 Provider、时区解析、文件存储 |
| `sdk/python/` | 进程外插件 SDK（`uv.lock` 锁定依赖） |
| `plugins/` | 示例 Python 插件：`event-observer`、`ai-reply` |
| `adapters/` | 渠道适配器；Python NoneBot2 适配器位于 `adapters/nonebot/` |
| `panel/` | TypeScript/React 控制面板，默认 mock，可切换 live |
| `control/` | Control API 服务：把 Core 控制应用端口映射为 REST/WebSocket |
| `deploy/` | Docker Compose 与 systemd 编排骨架 |
| `scripts/` | 工具链、契约、格式、静态分析、CI 与运维脚本 |
| `tests/acceptance/` | 跨模块闭环验收入口 |
| `doc/` | 架构、模块、规范与开发台账 |
| `.github/workflows/ci.yml` | GitHub Actions 构建、契约、格式、依赖与密钥检查 |

## 当前状态

- **已实现并纳入自动验收**：Core 进程启停与 gRPC 身份边界；统一运行时存储（内存/SQLite）；适配器持久化接收、命令授权、幂等与审计；插件注册、订阅和逐插件投递；Python 进程外插件 SDK；Action 纯文本闭环；Host storage/model/tasks 与任务调度；控制 API、认证/RBAC、WebSocket 事件流与面板真实服务闭环；结构化日志、健康探针，以及配置 API 到运行中 SDK 的下发与应用确认。Python NoneBot2 适配器测试已由 CI Protocol job 以锁定环境和 `KIARABOT_ADAPTER_DIR` 接入 `tests/acceptance/run.sh`，但远端 CI 尚未复跑，不能据此宣称适配器已验证通过。
- **跨语言闭环**：`tests/acceptance/run.sh` 使用真实 gRPC/SQLite/SDK 和协议 fake 平台验证事件→Action、权限拒绝、重复事件、断线重放和重启恢复；这是协议级验收，不代表真实 QQ/NapCat 联调。
- **尚未完成或未验收**：生产部署、备份恢复和发布镜像（T11b，依赖真实环境）；真实 QQ、真实模型联调（T07b，等待真实环境）；纯 C++ 替换是延期选项（T12）。
- 面板默认使用 mock 数据，`VITE_API_MODE=live` 时才连接实际控制 API。
- Python NoneBot2 适配器由本仓库 `adapters/nonebot/` 维护；后续可再考虑拆成单独插件。
- **远端 CI 与自动触发**：GitHub Actions 历史上 5 次运行均未通过（4 次失败、1 次取消；HEAD `e70de83` 仍有 4 个 job 失败）；自 2026-10-30（`16191cb`）起 `.github/workflows/ci.yml` 已暂停自动触发，只支持手动 `workflow_dispatch`。恢复自动触发前需先让手动运行变绿。
- **2026-10-31 十模块集成（W1–W4）**：contracts、toolchain、ci-cd、core、infrastructure、adapter、plugin-sdk、control-api、control-panel、operations 十个模块分支的 P0 + 阻断主线 P1 已完成并合并；本机验收：`full` 构建 CTest **32/32**、contracts 门禁 17/17、adapter 46、SDK 82、plugin 14、control-api 27、panel 17（live 2 项需 C++/venv 前置）、deploy 11/11；新增 `full` CMake 预设、契约状态登记/枚举基线、CI full-build 与依赖审计、Core 维护循环与可配置容量、SQLite 容量/崩溃/迁移测试、适配器健康与会话 ID 统一、插件示例、控制面生产传输、面板管理动作、可运行化的 Docker/compose 骨架。**仍未验证**：远端 CI 手动运行、Docker/T11b、真实 QQ/NapCat/模型 T07b；环境差异（Python 3.11.18、clang-format 16、vcpkg 版本）需 CI 复跑确认。待确认事项见 [`reports/pending-confirmation.md`](reports/pending-confirmation.md)，执行记录见 [`reports/execution/`](reports/execution/)。
- 实时任务状态和交付记录以 [开发台账](doc/16-development-tasks.md) 为准；[跨层未完成事项清单](doc/15-cross-layer-gaps.md) 是历史快照、不是现行清单，[实现现状评估](doc/14-implementation-assessment.md) 是早期快照。

## 构建与测试

### 依赖

- CMake 3.24+，C++20 编译器：AppleClang 15+、Clang 16+ 或 GCC 13+；
- Python 3.9+（dev 构建的 CTest 进程测试）；Python 3.11.18 + `uv`（SDK 与闭环验收，依赖见 `sdk/python/uv.lock`）；
- 协议构建：`protoc` 25+、Protobuf 运行库 25.0+、gRPC 1.60+、OpenSSL 3.0+ Crypto；
- 控制 API 构建：Boost 1.86+、libsodium，以及构建时按内容哈希获取的 nlohmann/json 3.11.3；
- 基础设施构建：SQLite3、libcurl 7.85+、Abseil（`absl::time`），以及系统 IANA zoneinfo（Linux 为 tzdata）；
- 面板：Node 20.19+（低于 21）或 22.12+、npm 10+；
- 契约与质量检查：Ruby、clang-format 16+、clang-tidy 16+。

完整版本矩阵与检查方式见 [开发工具链](doc/11-toolchain.md)。

### dev 构建（不需要 protobuf/gRPC）

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --no-tests=error
```

### 协议 + 基础设施构建

```bash
cmake --preset proto -DKIARABOT_ENABLE_INFRASTRUCTURE=ON
cmake --build --preset proto
ctest --preset proto --no-tests=error
```

### 跨语言闭环验收

需要上面的 proto + infrastructure 构建、Ruby，以及锁定 SDK 和控制 API 测试环境：

```bash
uv sync --project sdk/python --python 3.11.18 --locked
uv sync --project control/api/tests --python 3.11.18 --locked
bash tests/acceptance/run.sh
```

可选设置 `KIARABOT_ADAPTER_DIR=适配器目录绝对路径`（仓库内 `adapters/nonebot`），把适配器的全部测试一并接入；缺少构建或 Python 环境会直接失败，不会跳过。详见 [验收说明](tests/acceptance/README.md)。

### 控制面板

```bash
cd panel
npm ci
npm run check:contracts
npm run lint
npm test
npm run build
```

`npm run dev` 默认使用 mock 数据；连接真实服务时设置 `VITE_API_MODE=live`，必要时再设置 `VITE_API_BASE` 和 `VITE_WS_URL`。

### 统一检查

```bash
./scripts/check-toolchain.sh
```

脚本依次检查工具版本、C++20、协议生成、dev/proto 预设的构建与 CTest、独立基础设施构建与 CTest、契约、C++ 格式、静态分析和 API lint。也可以单独运行 `./scripts/quality/check-format.sh`、`./scripts/quality/check-tidy.sh`、`./scripts/quality/check-openapi.sh`、`./scripts/quality/check-asyncapi.sh` 和 `./proto/tests/check_contracts.sh`。CI 不直接调用 `scripts/check-toolchain.sh`：各 job 分别执行构建/测试、契约兼容、格式、静态分析、API 契约、工作流、依赖审计和密钥扫描检查（在 GCC 13/Clang 16 上构建 dev，用 pinned vcpkg 构建 proto + infrastructure 并运行闭环验收），与 [开发工具链](doc/11-toolchain.md) 的说明一致。

## 运行 Core

```bash
# 一次性自检
build/dev/core/kiarabot-core --check

# gRPC 入口（需 proto 构建；未配置业务凭据时只提供传输监听）
build/proto/core/kiarabot-core --grpc-listen 127.0.0.1:50051
```

持久化适配器接收、插件进程、模型 Provider、任务调度和完整环境变量见 [部署与可观测性](doc/08-operations.md)。传输监听不等于业务就绪：`rpc_ready` 保持 false，业务配置必须完整提供，部分配置会拒绝启动。数据库 schema 不保证向旧二进制回滚，升级前先备份。

## 文档

- **规范与规则**：[AI 开发入口规则](AGENTS.md)、[目标架构基线（人类维护，AI 只读）](doc/18-architecture-baseline.md)、[C++ 主编码规范](doc/12-cpp-coding-standard.md)、[开发边界与协作规则](doc/00-boundaries.md)、[分层开发规范索引](doc/README.md)
- **唯一契约**：[Protobuf](proto/README.md)、[OpenAPI](api/openapi.yaml)、[AsyncAPI](api/asyncapi.yaml)、[统一协议层](doc/01-contracts.md)、[事件与 Action 运行时语义](doc/01-runtime-semantics.md)
- **模块设计**：[C++ Core](doc/02-core.md)、[PluginSDK 与插件运行时](doc/03-plugin-sdk.md)、[Python NoneBot2 适配器](doc/04-nonebot-adapter.md)、[基础设施适配器](doc/05-infrastructure.md)、[控制面板 API](doc/06-control-api.md)、[TypeScript 控制面板](doc/07-control-panel.md)、[部署与可观测性](doc/08-operations.md)
- **过程与质量**：[分层开发任务与实施台账](doc/16-development-tasks.md)、[分阶段开发顺序](doc/09-roadmap.md)、[可执行编码任务模板](doc/10-task-template.md)、[开发工具链](doc/11-toolchain.md)、[CI/CD 流程方案](doc/12-ci-cd.md)、[本机闭环验收](tests/acceptance/README.md)、[Python SDK 使用说明](sdk/python/README.md)
- **现状与缺口**：[实现现状与纯 C++ 演进评估](doc/14-implementation-assessment.md)、[跨层未完成事项清单](doc/15-cross-layer-gaps.md)、[账号封禁风险评估](doc/17-account-ban-risk.md)、[控制 API 契约补充任务说明](doc/13-control-api-contract-handoff.md)、[基础设施接入后续事项](doc/13-infrastructure-integration-follow-up.md)

修改代码前先阅读 [AI 开发入口规则](AGENTS.md) 和 [C++ 主编码规范](doc/12-cpp-coding-standard.md)；只要改变了外部可见的接口、协议、状态、错误语义、配置或运行方式，就必须在同一变更中同步对应文档。

## 独立组件与许可证

- NapCatQQ 作为独立 QQ 协议端安装和运行，不复制源码到本仓库，也不重新打包成 KiaraBot 的一部分。
- Python NoneBot2 适配器由本仓库 `adapters/nonebot/` 维护，通过 `proto/` 契约接入；后续可再考虑拆成单独插件。
- 第三方组件及其许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
- KiaraBot 自有代码采用 MIT 许可证，许可证文本见 [LICENSE.md](LICENSE.md)。
