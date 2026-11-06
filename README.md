# KiaraBot

KiaraBot 是一个面向 QQ 的可扩展机器人平台：C++20 Core 负责事件、权限、插件调度与业务编排，
Protobuf/gRPC 契约隔离各进程，Python PluginSDK 支持进程外插件，TypeScript 控制面板通过
Control API 管理运行时。QQ 协议端 NapCatQQ 是独立组件；Python NoneBot2 适配器由本仓库
`adapters/nonebot/` 维护，二者都只通过公共协议接入。

- 架构设计：[`docs/architecture.md`](docs/architecture.md)
- 模块设计：[`docs/modules/`](docs/modules/)
- 工程规范：[`docs/spec/`](docs/spec/)
- 许可证：[`LICENSE.md`](LICENSE.md)；第三方声明：[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)

## 核心设计思想

1. **契约优先**：`proto/` 是跨语言契约唯一来源，`api/openapi.yaml`、`api/asyncapi.yaml` 是
   HTTP/WebSocket 契约唯一来源；实现状态登记在 `proto/tests/contract_status.json`，未实现项
   必须显式标记，不能由历史 reference 证据或“文档写了”推导为可用。
2. **分层与边界**：`core/` 只承载领域规则、事件编排、授权判定与插件调度；适配器只做平台转换；
   插件只能经 SDK 访问 Core 能力；控制面板只能通过 Control API 访问业务数据；基础设施实现
   Core 定义的端口，不决定业务规则。依赖方向与模块边界见 `docs/spec/boundaries.md`。
3. **六服务、单机优先**：目标拓扑为 Identity / Runtime / Model / Scheduler / Host / Control
   六个独立进程（+ 控制面板、适配器、SDK 插件），默认单主机 Docker Compose；两主机/断网演练
   属于延期项。每个服务独立 TLS 材料、凭据与数据库角色边界，数据库不跨库访问。
4. **身份与授权独立成服务**：所有 owner 服务经 Identity 完成登录、会话、凭据、委托与逐操作
   在线授权；进程间使用 mTLS + 平台 peer 白名单 + 显式绑定，不把传输身份当业务授权。
   委托（delegation）带 audience/scope/expiry，子委派寿命受冻结契约约束；Host 维护准入另有
   窄范围 gate 检查；bootstrap 凭据只一次性交付（Runtime 仅存摘要，REST 不回显明文）。
5. **数据所有权与事务**：每个 owner 拥有私有 PostgreSQL 库/UoW 与迁移；Host 使用本地 SQLite
   journal；跨服务只经 RPC 协作，不做跨库事务。operation/participant、journal、outbox 与业务
   写入在 owner 内同事务提交；恢复/重建 journal 时变更 epoch，旧 cursor 返回 typed CONFLICT。
6. **可靠语义显式化**：写操作要求幂等键与 expected_version；operation 有明确状态机
   （ACCEPTED/RUNNING/BLOCKED/SUCCEEDED/FAILED/CANCELLED/EXPIRED）；传输失败或进程崩溃不得
   伪成功，未知结果以 UNKNOWN/partial 表达（模型调用另有 may_have_billed），依赖故障
   fail-closed；投递使用持久租约、attempt/fence 与 receipt，慢消费者有界。
7. **插件与适配器模型**：外部适配器/插件经 AdapterBridge/PluginBridge 长流连接，注册时用
   bootstrap 凭据换取连接授权与 Fence；受管插件由 Host 拉起并以 execution generation 隔离，
   重连/重启只推进代次，不复活旧执行；事件投递、Action、storage/namespace、task、model chat
   与 streaming 都走公共契约。
8. **模型调用两次事务**：先提交 intent/审计（txn1），在事务外调用 Provider，再用新 UoW 写终态
   或 UNKNOWN（txn2）；同一 logical_call_id 重放不重新调用 Provider；streaming 使用
   started/delta/terminal 有序帧与 cancel receipt，partial/UNKNOWN/usage 事实分别表达。
9. **可观测与可运维**：所有进程提供 liveness/readiness（依赖未就绪返回 503/DEGRADED，不伪造），
   日志/事件不落明文秘密；部署为显式初始化、迁移与身份 bootstrap，不自动清库/导入旧数据；
   备份/恢复与秘密轮换有固定顺序（见 `docs/modules/infrastructure.md`）。

## 部署步骤

### A. 本地开发环境（编译与测试）

依赖版本、锁定工具链与换机步骤见 [`docs/spec/toolchain.md`](docs/spec/toolchain.md)。准备后：

```bash
cmake --preset full-locked -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset full-locked --parallel 4
ctest --preset full-locked --no-tests=error --output-on-failure
```

Python 侧生成协议并按锁安装：

```bash
uv sync --project sdk/python --locked --python 3.11.18
uv sync --project adapters/nonebot --locked --python 3.11.18
sdk/python/.venv/bin/python sdk/python/tools/generate_proto.py
adapters/nonebot/.venv/bin/python adapters/nonebot/tools/generate_proto.py proto
```

控制面板：

```bash
npm ci --prefix panel
npm --prefix panel run lint && npm --prefix panel test && npm --prefix panel run build
```

完整质量门禁与 CI 说明见 [`docs/spec/ci-gates.md`](docs/spec/ci-gates.md)。

### B. 本机单机演示（reference 轨道：真实 NoneBot + C++ Core + SDK 处理器 + 可选 Panel）

该轨道用于快速验证消息闭环，不需要真实 QQ、PostgreSQL 或六服务，不用于生产部署。

```bash
cmake --build build/full-locked --target kiarabot-reference-core kiarabot-control-credentials

python3 scripts/local/start.py start \
  --core build/full-locked/reference/core/kiarabot-core \
  --credentials build/full-locked/reference/control/api/kiarabot-control-credentials \
  --python adapters/nonebot/.venv/bin/python

# 可选网页（先安装 panel 依赖）
python3 scripts/local/start.py stop
python3 scripts/local/start.py start --ui

# 状态 / 停止
python3 scripts/local/start.py status
python3 scripts/local/start.py stop
```

网页默认 `http://127.0.0.1:5173`，账号 `admin`，密码在 `data/local/admin-password`。
状态目录与密码/密钥/SQLite/outbox 均在忽略的 `data/`（或 `--state-dir` 指定目录），
不得提交；`init/start` 不重置已有密码与数据库，`stop` 只终止本程序拥有的进程。

### C. 目标六服务单机 Docker Compose（P14a）

要求 Linux 主机或 Apple Silicon 上的 amd64 容器执行支持、Docker Compose ≥ 2.24。默认拓扑为
单主机：PostgreSQL + Control 面 + 执行面同机，各服务独立容器/TLS/凭据/DB 权限边界。

```bash
# 0) 结构/YAML/shell 静态检查（无 Docker 时输出 UNVERIFIED）
./scripts/deploy/target-smoke.sh

# 1) 部署参数模板（不包含秘密；权限 600）
cp deploy/target.env.example deploy/target.env && chmod 600 deploy/target.env

# 2) 由部署流程生成材料：config/tls/secrets/data/packages/backups
#    秘密/证书/密码/root key/enrollment/bootstrap 只读挂载，不入镜像、不提交。
#    文件布局与消费者见 docs/modules/infrastructure.md 的部署章节。

# 3) 启动 PostgreSQL（官方 postgres:16.4-bookworm；不自动建库）
docker compose --env-file deploy/target.env -f deploy/compose.target.yml up -d postgres

# 4) 逐库显式空库初始化（四个私有库非原子，逐库检查；init profile 只 run，不随 up -d 启动）
for svc in identity runtime scheduler model; do
  docker compose --env-file deploy/target.env -f deploy/compose.target.yml \
    --profile init run --rm "init-$svc"
done

# 5) 离线身份 bootstrap（admin/enrollment）
docker compose --env-file deploy/target.env -f deploy/compose.target.yml \
  --profile init run --rm identity-bootstrap

# 6) 启动单机默认全栈（PG + 数据/控制面 + Host + Adapter）
docker compose --env-file deploy/target.env -f deploy/compose.target.yml --profile exec up -d
```

约束：服务进程不执行 DDL、不清库、不导入旧快照；初始化/迁移/身份 bootstrap 必须按顺序人工执行
并逐库检查（跨库非原子）。默认镜像名为本地 `:dev` 标签，正式发布必须固定 digest 并通过
release 门禁；Panel 端口只发布到宿主回环地址。停止顺序、健康检查与故障恢复见
`docs/modules/infrastructure.md`；两主机拓扑为可选项且跨主机断网/重连演练延期、未验证。

### D. 数据、秘密与备份

- 运行数据/密码/token/私有配置只在忽略的 `data/` 或指定 state 目录，禁止提交；
- 生产配置/TLS/密码/root key/enrollment/bootstrap 由部署生成，经只读挂载提供，0600/正确属主；
- 备份/恢复、秘密轮换与数据库恢复顺序见 `docs/modules/infrastructure.md`，演练脚本位于
  `scripts/ops/`；恢复前必须先停机并备份，禁止半途重试覆盖。

## 仓库结构

```text
proto/          跨进程/跨语言契约（唯一来源）
api/            OpenAPI / AsyncAPI 契约（唯一来源）
core/           C++20 Core 领域规则、事件编排、授权判定、Runtime/Model/Scheduler/Host 实现
control/        Control API（HTTPS/REST/WS owner gateway）
panel/          TypeScript 控制面板
adapters/       NoneBot2 适配器（OneBot V11）
sdk/            Python PluginSDK
plugins/        本地示例插件
infrastructure/ PostgreSQL schema/UoW/迁移与 SQLite store 实现
tests/          跨进程/真实链路端到端测试
deploy/         Docker Compose/Dockerfile/部署模板
docs/           架构、模块与工程规范（唯一文档树）
scripts/        本地运行、质量门禁、部署与运维脚本
```

## 文档索引

- 架构设计：`docs/architecture.md`
- 模块设计：`docs/modules/{identity,core-runtime,model,scheduler,host,control-api,control-panel,plugin-sdk,nonebot-adapter,infrastructure}.md`
- 工程规范：`docs/spec/{coding-standard,boundaries,toolchain,ci-gates,task-workflow}.md`

## 许可证

见 [`LICENSE.md`](LICENSE.md) 与 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
