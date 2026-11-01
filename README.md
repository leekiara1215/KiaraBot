# KiaraBot

KiaraBot 是一个面向 QQ 的可扩展机器人平台：C++20 Core 负责事件、权限、插件调度和业务编排，Protobuf/gRPC 契约隔离各进程，Python PluginSDK 支持进程外插件，TypeScript 控制面板通过 Control API 管理运行时。QQ 协议端 NapCatQQ 是独立组件；Python NoneBot2 适配器由本仓库 `adapters/nonebot/` 维护，二者都只通过公共协议接入。后续可再考虑把适配器拆成单独插件。

## 当前先运行这一版

2026-11-01晚间优先交付单机消息闭环，插件机制、完整账号管理、多机部署后移。
已本机验收：真实NoneBot → reference C++ Core/SQLite → SDK固定处理器 → 本地OneBot
平台替身，群/私聊回复“收到：原文”，live网页登录、停止和保留数据的重启。
这条临时运行轨不代表六服务新架构已经完成。随后已完成本机真实QQ/NapCat群聊纯文本
收发首轮验收，见[真实QQ接入](doc/26-real-qq-onboarding.md)；模型及完整故障恢复仍未联调。

构建与锁定环境准备完成后执行：

```bash
python3 scripts/local/start.py start --ui \
  --core build/full-locked/reference/core/kiarabot-core \
  --credentials build/full-locked/reference/control/api/kiarabot-control-credentials \
  --python adapters/nonebot/.venv/bin/python
```

网页地址`http://127.0.0.1:5173`，账号`admin`，本机密码在`data/local/admin-password`。
不需要网页可省略`--ui`。完整环境准备、收发CLI及停止命令见
[单机运行说明](doc/24-single-host-mvp.md)；未完事项、分支与跨电脑恢复见
[开发交接文档](doc/25-development-handoff.md)。主集成源码已同步远端main，未合入WIP仍须按交接文档保全。

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
C++20 Runtime                      事件、实例、投递、Action、namespace
   ├─ PluginSDK（当前 Python）       进程外业务插件
   ├─ Plugin Host                  独立受管进程与本地日志
   ├─ Scheduler                    独立任务与可靠触发
   └─ Model                        独立模型配置与 Provider 调用

TypeScript 控制面板 ── REST/WS ── C++20 Control
                                      │ 类型化所有者 RPC
                             Identity / Runtime / Scheduler / Model

Identity                           独立身份、会话与范围授权
Identity/Runtime/Scheduler/Model    各自私有 PostgreSQL 逻辑库
Host/Adapter                       各自本地 SQLite 可靠执行/发送日志
```

图示是本轮目标，完成度见下文。协议和 API 契约是跨模块唯一来源；未来可以替换 NoneBot2 适配器、插件语言或面板客户端，而不改变公共协议。本轮设计见 [方向基线](doc/19-architecture-direction-baseline.md) 和 [详细设计](doc/20-detailed-architecture-design.md)；首版 [目标架构基线（人类维护，AI 只读）](doc/18-architecture-baseline.md) 保留原文。

## 仓库组成

| 路径 | 内容 |
| --- | --- |
| `proto/kiarabot/v1/` | 跨语言唯一契约：事件、Action、适配器、插件和 gRPC 服务 |
| `api/` | OpenAPI 与 AsyncAPI 契约 |
| `core/` | C++20 服务内层/端口及 platform 运行支撑；`core/reference/` 保留旧一体实现 |
| `infrastructure/` | 旧 reference 适配与目标四个私有 PG 库、typed仓储和本地可靠交接 |
| `sdk/python/` | 进程外插件 SDK（`uv.lock` 锁定依赖） |
| `plugins/` | 单机固定处理器`local-reply`及示例`event-observer`、`ai-reply` |
| `adapters/` | 渠道适配器；Python NoneBot2 适配器位于 `adapters/nonebot/` |
| `panel/` | TypeScript/React 控制面板，默认 mock，可切换 live |
| `control/` | 独立 Control application/入口及保留的旧 HTTP/WS reference；目标路由尚未接线 |
| `deploy/` | Docker Compose 与 systemd 编排骨架 |
| `scripts/` | 工具链、契约、格式、静态分析、CI 与运维脚本 |
| `tests/acceptance/` | 跨模块闭环验收入口 |
| `tests/architecture/` | 新六进程的 P02 黑盒验收与临时证书夹具 |
| `doc/` | 架构、模块、规范与开发台账 |
| `.github/workflows/ci.yml` | GitHub Actions 构建、契约、格式、依赖与密钥检查 |

## 当前状态

**2026-11-01 架构重建**：当前开发按 [新方向基线](doc/19-architecture-direction-baseline.md)
与 [P00–P14 计划](doc/23-implementation-plan.md) 推进。六个独立服务入口、分层构建与进程验收已接线，
P00–P03本机固定环境统一门禁已通过，P04认证链开发中，目标业务仍未完成，不能以存活探针替代就绪。旧 Core 源码和原断言保留为
`core/reference/` 构建；下列旧一体化闭环记录与部署说明属于 reference 证据，
不代表六服务已经具备这些能力。旧 `kiarabot-core` 参考产物位于构建目录的
`reference/core/`，不作为新生产入口。实际提交、检查与未验收项见
[P02 实施记录](reports/execution/2026-11-01-architecture-p02.md)。

- **已实现并纳入自动验收**：Core 进程启停与 gRPC 身份边界；统一运行时存储（内存/SQLite）；适配器持久化接收、命令授权、幂等与审计；插件注册、订阅和逐插件投递；Python 进程外插件 SDK；Action 纯文本/表情回应/撤回/群管理/好友审批闭环（权限门禁 + 会话白名单 + 每日预算 + 审计的授权模型）；Host storage/model/tasks 与任务调度；控制 API、认证/RBAC、WebSocket 事件流与面板真实服务闭环；结构化日志、健康探针，以及配置 API 到运行中 SDK 的下发与应用确认。Python NoneBot2 适配器测试已由 CI Protocol job 以锁定环境和 `KIARABOT_ADAPTER_DIR` 接入 `tests/acceptance/run.sh`，但远端 CI 尚未复跑，不能据此宣称适配器已验证通过。
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

### 目标 full 构建（含 reference）

```bash
uv sync --project sdk/python --locked --python 3.11.18
npm ci --prefix scripts/ci --ignore-scripts
cmake --preset full-locked
cmake --build --preset full-locked
ctest --preset full-locked --no-tests=error
```

预先按工具链说明安装固定 vcpkg、PG16.4 工具与 OpenSSL3.5 CLI，并设置 `VCPKG_ROOT`
及测试工具路径。`full-locked` 使用固定 manifest，包含六服务与 reference；环境获取与
真实版本核验见 [工具链说明](doc/11-toolchain.md)。Python 生成使用锁定的 SDK 环境，
不能只安装 C++ 依赖就跳过协议往返。阶段门禁未关闭时仍按执行记录列出限制。

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

## 运行目标服务（P02）

```bash
build/full-locked/bin/kiarabot-control --help
build/full-locked/bin/kiarabot-identity --config /path/to/identity.json
```

其余入口为同目录的 `kiarabot-runtime`、`kiarabot-plugin-host`、`kiarabot-scheduler`、
`kiarabot-model`，均使用 `--config`。配置包含匹配的服务名、独立回环探针、TLS 文件
引用；五个业务 RPC 服务另有 mTLS 地址，详见 [运维说明](doc/08-operations.md)。
本阶段业务未实现，live=200、ready=503，未实现 RPC 明确拒绝；不以这些探针宣称机器人可用。

### 旧 Core reference

以下入口仅用于隔离回归，其参数不适用于六个新服务：

```bash
# 一次性自检
build/dev/reference/core/kiarabot-core --check

# gRPC 入口（需 proto 构建；未配置业务凭据时只提供传输监听）
build/proto/reference/core/kiarabot-core --grpc-listen 127.0.0.1:50051
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
