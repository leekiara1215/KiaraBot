# KiaraBot

[![CI](https://github.com/leekiara1215/KiaraBot/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/leekiara1215/KiaraBot/actions/workflows/ci.yml) [![Release images](https://github.com/leekiara1215/KiaraBot/actions/workflows/release.yml/badge.svg)](https://github.com/leekiara1215/KiaraBot/actions/workflows/release.yml)

自动 CI 默认由仓库变量 `CI_AUTO_ENABLED=true` 门控；首次启用前先手动运行 CI 的 full-build 并跑绿 full-locked，发布流程与权限门禁见 [doc/12-ci-cd.md](doc/12-ci-cd.md)。本地可先执行 `scripts/ci/check-workflows.sh` 做 workflow 静态检查。

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

**2026-11-03 目标架构状态**：P00–P14 的实现已基本合入 main（P14t/P05b-13 最终基线 `04f7044`（P13 生产退出 PASS、P11 独立验收 PASS、P10k 真实 Scheduler 任务链 PASS、P05b B1 链完成；安全评审 0 Critical/High；final gate **161/161 CTest passed @7b8fb0f**（`build/logs/recheck-ctest-155131.log`，1051.70s，2026-11-03 15:51:31→16:09:06；本轮一次全绿、无失败；此前 `1213790` gate 148/148（687.02s）的 `p02_core_process`/`p08_managed_host_e2e` 负载时序 flake 未复现））原生五进程栈矩阵 **9 success + 1 stable-ok + 0 known-gap**），
但“实现合入”“已实测”“待验收/未验证”“正式发布”必须分开看。统一证据矩阵与发布门禁见
[阶段退出证据](doc/27-phase-exit-evidence.md)、[P14t 发布清单](doc/28-p14d-release-checklist.md)；
CI/发布流水线配置见 [CI/CD 文档](doc/12-ci-cd.md)。

| 交付类别 | 当前状态 |
|---|---|
| 单机部署（默认目标） | **未验收**：单主机 Compose/脚本已完成离线检查，本机无 Docker，未执行 compose 启动、官方 PG16 镜像初始化/迁移/恢复 |
| 跨主机演练 | **延期/未验证**（用户确认两主机为可选项，网络分区/TLS/撤销/重连未演练） |
| 远端 CI | **未运行**：GitHub Actions workflow/本地静态检查已就绪，自动触发默认由 `CI_AUTO_ENABLED` 门控；需先手动 workflow_dispatch 跑绿 full-locked |
| 正式发布 | **未就绪**：GHCR/SBOM/cosign 骨架已合入但未推送签名镜像，需完成 release 环境/凭据/审批与发布清单 |

已实现并有自动测试覆盖的主要链路（证据与测试名见矩阵）：Identity/Control 认证；Runtime 资源/授权链；
PluginBridge/AdapterBridge 真实 TLS 事件→插件→Action；Scheduler 定时交接；Namespace/Storage CAS；
Model 两次事务/Provider/UNKNOWN 边界；Host 期望状态与进程桥；Control owner REST/健康/operation 快照；
P13 生命周期清理与 Host/SDK drain；P14f/i/j/k/l/m 原生单机栈矩阵 9 success+1 stable-ok+0 known-gap；
P12j Panel client 严格 live 通过。P13 生产退出通过（本验收范围；多主机/满盘/生产规模仍环境项）；P11 独立验收通过；P10k 真实 Scheduler 任务链通过；P05b B1 链完成；安全评审 0 Critical/High，Medium 全部处置（F-1/F-2 已修，F-3/F-4/F-5/F-6 已加固；F-5 host/core 边界文档化）；P12 host operation/
bootstrap_credential 字段级待决策；P14 Docker/Compose、两主机、远端 CI、正式发布仍未验收；真实 QQ/NapCat、
真实模型 Provider 只在用户环境单独验证，不能用 fake 结果替代。最终索引见
[P14t 交付状态](reports/modules/architecture-delivery-status.md)。
旧 `core/reference/` 一体化闭环与部署说明保留为 reference 证据，不代表六服务生产可用。

- **已实现并纳入本机/定向门禁**：P02 六进程启停与身份边界；P03 PG 私有库/初始化/桌面 fixture；
  P04 Identity/Control 认证与委托；P05 Runtime 资源/授权链；P06 真实 TLS Bridge/投递/SDK/Adapter；
  P07 Action/配额/执行闭环；P08 Host/期望状态/配置 ACK；P09 Storage/namespace；P10 Scheduler/定时交接；
  P11 Model 网关/Provider/调用者校验/PluginBridge model.chat；P12 Control REST 与 Panel 骨架；
  P13 生命周期清理/drain；P14 部署文件/CI/备份恢复 runbook。
- **已实测（本机真实依赖/loopback 假服务）**：真实 PG16.4 的事务/投递/存储/调度/Action 测试；
  真实 TLS gRPC Bridge/Control 认证验收；P14c 本地备份/恢复故障矩阵（`scripts/ops/target-drill.sh`，
  RPO=0s、RTO≈4.6–12.1s，小数据量）；P14b-2 workflow 本地 YAML/门禁静态检查。远端 Actions、
  Docker/Compose、两主机、真实 QQ/模型未运行。
- **尚未完成或未验收（P14t 收敛）**：Docker Compose 六服务栈；两主机/断网演练（延期）；远端 CI/release；
  真实 QQ/NapCat；真实付费外部 Provider；生产 CA/密钥托管、备份离站；部署级错误 Runtime 叶证书场景；
  Model Update/RotateProviderSecret proto `scope_kinds` 契约缺口；P12 host operation/bootstrap_credential
  按用户确认保持 not_implemented。
- **远端 CI 与发布**：GitHub Actions 触发器已恢复但默认由仓库变量 `CI_AUTO_ENABLED` 门控；
  `release.yml` 仅允许受保护 `release-gated` 环境 + `RELEASE_PUSH_ENABLED`/`RELEASE_COSIGN_KEYLESS`
  时推送自建镜像到 `ghcr.io/leekiara1215`，SBOM+签名是发布门槛；release dry-run 与正式发布均未运行。
- 面板仍有 mock/`VITE_API_MODE=live` 两种模式；Python NoneBot2 适配器由本仓库维护。
- 实时任务状态以 [开发台账](doc/16-development-tasks.md) 为准；P14n 证据缺口与建议顺序见
  [`doc/27`](doc/27-phase-exit-evidence.md)；历史快照（doc/14/15、reports/modules/operations.md 旧段）
  不作为当前完成度结论。

## 构建与测试

### 依赖

- CMake 3.24+，C++20 编译器：AppleClang 15+、Clang 16+ 或 GCC 13+；
- Python 3.9+（dev 构建的 CTest 进程测试）；Python 3.11.18 + `uv`（SDK 与闭环验收，依赖见 `sdk/python/uv.lock`）；
- 协议构建：`protoc` 25+、Protobuf 运行库 25.0+、gRPC 1.60+、OpenSSL 3.0+ Crypto；
- 控制 API 构建：Boost 1.86+、libsodium，以及构建时按内容哈希获取的 nlohmann/json 3.11.3；
- 基础设施构建：SQLite3、libcurl 7.85+、Abseil（`absl::time`），以及系统 IANA zoneinfo（Linux 为 tzdata）；
- 面板：锁定 Node 22.14.0（`scripts/toolchain/install-node.sh` 官方归档+SHA-256；不覆盖系统 Node）、npm 10+；
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

### 统一本地 CI（P14b）

```bash
# 安装锁内 Node 22.14.0（官方归档 SHA-256 校验，写入 build/ 不覆盖系统 Node）
scripts/toolchain/install-node.sh
export PATH="$PWD/build/toolchain/node-v22.14.0/bin:$PATH"

# 统一入口；--json 输出机器可读证据（默认 build/ci-local/）
scripts/run-ci-local.sh toolchain
scripts/run-ci-local.sh contracts --json build/ci-local/contracts.json
scripts/run-ci-local.sh panel
scripts/run-ci-local.sh all --json build/ci-local/all.json
```

`run-ci-local` 子命令：`toolchain|build|test|contracts|python|panel|all`；缺少前置的步骤记录为 `skip` 且整体 `pass-with-skips`，不会把未执行项当作已通过。`--dry-run` 只打印命令，`--self-test` 校验参数/JSON 写入。远端 CI 使用同一批脚本；自动触发仍暂停，恢复前需手动 Run workflow 全绿。

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
