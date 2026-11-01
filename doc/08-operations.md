# 部署与可观测性设计

T07b 真实联调环境尚未提供（用户已确认）。准备说明和不含密钥的模板见 [真实联调环境准备](../tests/acceptance/real-environment.md)。先以 event-observer 验证接收，模板不自动发送平台消息或模型请求；真实收发、模型和恢复结果仍待实际环境验收，不能用 T07a 替代。

T06c 持久化插件入口装配 TaskScheduler 并协商 host.tasks.v1；插件权限显式添加 task.schedule，定时事件订阅为 event.timer.triggered。数据库升级至 v5，包含任务/领取 token；启动在开启监听前恢复 RUNNING，恢复失败拒绝启动。一个数据库只运行一个 Core；替换进程必须先停止旧进程。任务每 100ms 检查，系统必须安装 IANA zoneinfo（Linux tzdata）；备份任务/inbox/幂等记录须保持同一快照。COMPLETED 只表示入队，排障同时检查插件投递状态。

T06b 可选配置 KIARABOT_MODEL_ENDPOINT（完整 /chat/completions URL）、KIARABOT_MODEL_API_KEY 和 KIARABOT_MODEL_NAME，三者必须同时设置且持久化插件入口已启用；部分/非法配置启动失败 INVALID_ARGUMENT。未配置时不协商 host.model.v1。插件须获授 model.chat。只允许 HTTPS 或本机 127.0.0.1 HTTP；禁用环境代理/重定向，证书校验开启，最大请求三十秒，停机可能等到该调用期限。密钥不要放命令行或入库；受管 Python 环境白名单不包含任何模型配置。模型只读探活与实际外部供应商联调仍未验证。

T06a 持久化插件入口装配 HostDispatcher；存储、可选模型和任务能力按实际配置协商，storage.namespace 必须显式列入 KIARABOT_PLUGIN_PERMISSIONS。T06a 引入 schema v4，当前版本升级为 v6，回滚使用迁移前备份。Host 存储和幂等响应随 Core 数据库一起备份，不能单独删除幂等响应后重放写入。

T04b 启用持久化插件入口时装配 ActionRouter，插件 message.send 权限仍显式配置；只声明已实现的 action.result.v1。T04b 引入 schema v3，当前已继续升级至 v6，旧二进制不能读取新 schema，回滚必须使用迁移前备份。Python 适配器在 OUTBOX_PATH 旁维护 .actions 执行日志，备份/恢复须与事件 outbox 一起处理，禁止删除未终态记录后重放平台请求。

## T03b 受管 Python 插件

在 T03a 插件入口基础上，配置 KIARABOT_PLUGIN_EXECUTABLE（Python 可执行文件绝对路径）和 KIARABOT_PLUGIN_MANIFEST（清单绝对路径）；可选 KIARABOT_PLUGIN_PYTHONPATH 指定 SDK、生成协议和插件模块目录。Core 执行 `python -m kiarabot_sdk --manifest ...`，不经过 shell。默认启动等待 5000 ms、停止宽限 2000 ms，可分别用 KIARABOT_PLUGIN_START_TIMEOUT_MS、KIARABOT_PLUGIN_STOP_TIMEOUT_MS 配置为 10–30000 ms。配置不完整或缺少插件入口时启动失败。

子进程只接收最小 PATH、插件 token、endpoint、manifest 参数和可选 PYTHONPATH，不继承其它 Core 秘密。stdout/stderr 当前重定向到空设备；Core 仅报告启动、退出、强制回收及稳定错误。插件崩溃不退出 Core；停止对插件进程组先 TERM、后 KILL，并回收直接子进程。当前面向本机 POSIX 开发运行，生产资源沙箱、自动重启和镜像仍待后续任务。

T03a：在持久化入口配置基础上，设置 KIARABOT_PLUGIN_ID/TOKEN 及 KIARABOT_PLUGIN_PERMISSIONS（逗号分隔权限；事件插件至少 message.read），即可启用单个已授权插件连接。库级 PluginHub 支持多个策略/连接，CLI 当前只配置一个插件。没有权限配置仍只提供插件传输认证并拒绝业务。插件启动器、Action/Host 和控制服务已接线；启动日志的 rpc_ready 仍为 false，plugin_delivery_ready 只表示本机插件投递入口已启用。

## 责任

运维层负责让各个模块可启动、可停止、可检查、可升级和可恢复。

## 目录

```text
deploy/
  compose.staging.yml        # staging 单机栈（可从仓库构建镜像）
  compose.prod.yml           # production 单机栈（只接受 digest 镜像）
  docker/
    adapter.Dockerfile       # NoneBot 适配器镜像
    panel.Dockerfile         # 面板静态构建 + nginx 反代镜像
    panel-nginx.conf.template
  control/control.example.json  # 控制配置模板（复制后改权限/属主）
  env.example                # 部署环境变量模板（复制为 deploy/.env）
  systemd/kiarabot.service
scripts/
  check-toolchain.sh
  ci/…                       # CI 脚本（不属本层）
  deploy/
    deploy.sh  migrate.sh  backup.sh  healthcheck.sh  smoke.sh  log.sh
    tests/deployment_test.py  tests/healthcheck_test.py
  quality/…
```

数据库迁移由 `infrastructure/src/sqlite_runtime_store.cpp` 实现，独立命令入口为 `infrastructure/src/migrate.cpp`（镜像内 `/usr/local/bin/kiarabot-migrate`）；运维目录只负责构建、编排、启动、健康检查、备份、验证和回滚。

## 进程与部署模型

真实链路是 4 个进程，其中只有 3 个由本仓库构建镜像：

```text
napcat（第三方，宿主机进程，不入镜像、不入 Compose）
kiarabot-adapter-nonebot（本仓库镜像）
kiarabot-core（本仓库镜像，控制 API 同进程）
kiarabot-panel（本仓库镜像：nginx 静态站点 + 反代）
```

Core、控制 API、适配器健康探针和面板反代都**只监听 127.0.0.1**，这是协议与安全基线（Core 的 gRPC 只接受 `127.0.0.1:` 前缀，控制 HTTP 只接受回环 listen）。因此 Compose 采用 **Linux host 网络** 单机模型：

- 所有容器共享宿主机网络命名空间，`127.0.0.1` 在 Core、适配器、面板之间是同一个回环地址；
- 不需要跨容器 DNS，也不发布任何端口；对宿主机以外的访问只能通过 SSH 隧道或另行部署的反向代理；
- 该模型要求 Linux 主机，Docker Desktop 默认不支持；NapCat 必须在同一台宿主机上以 `127.0.0.1` 监听 OneBot 正向 WebSocket（默认端口 3001）。

不允许把回环监听改成 `0.0.0.0` 来“让跨容器可用”。需要多主机/多容器隔离时，应先评审 Core 的绑定地址策略和 TLS 方案，而不是改 Compose。

## 端口与暴露面

| 端口 | 所属进程 | 绑定 | 用途 |
| --- | --- | --- | --- |
| `${KIARABOT_CONTROL_PORT}`（默认 8080） | Core 控制 API | `127.0.0.1` | REST/WS、`/health/live`、`/health/ready`、`/api/v1/health` |
| `${KIARABOT_GRPC_PORT}`（默认 50051） | Core BridgeServer | `127.0.0.1` | 适配器/插件 gRPC（明文回环，非生产 TLS 服务） |
| `${KIARABOT_ADAPTER_HEALTH_PORT}`（默认 8081） | 适配器健康服务 | `127.0.0.1` | `/health/live`、`/health/ready` |
| `${KIARABOT_PANEL_PORT}`（默认 8082） | 面板 nginx | `127.0.0.1` | 静态站点 + `/api/`、`/ws/` 反代 + `/health/live`、`/health/ready`（转发 Core ready） |
| 3001（示例） | NapCat | `127.0.0.1` | OneBot 正向 WS，属于外部组件配置 |

TLS 由本层之外的反向代理/SSH 隧道提供；仓库当前**未交付 TLS 终止配置**，Compose 中不发布公网端口，不能把本配置当作“已启用 HTTPS”的生产入口。

## 镜像构建

`Dockerfile` 使用与 CI 相同的 pinned vcpkg toolchain（`ARG VCPKG_COMMIT`，2025.04.09）构建，显式启用：

```text
-DKIARABOT_GENERATE_PROTO=ON
-DKIARABOT_ENABLE_INFRASTRUCTURE=ON
--target kiarabot-core kiarabot-migrate
```

因此镜像内的 `kiarabot-core` 包含 gRPC、控制 API、SQLite 持久化和 `/health/live`、`/health/ready` 探针；`kiarabot-migrate` 随镜像提供。运行阶段安装 `ca-certificates curl sqlite3 tzdata`（任务调度需要 IANA zoneinfo），以 UID/GID 10001 非 root 运行，只复制运行时二进制与 `scripts/deploy/*.sh`。

适配器与面板镜像由 `deploy/docker/` 提供，均从仓库根上下文构建（`.dockerignore` 白名单包含 `proto/`、`adapters/nonebot/`、`panel/`、`api/`，仍排除 `.env`、密钥、`node_modules`、`.venv`、生成物）：

- `adapter.Dockerfile`：`python:3.11-slim` + uv（锁定 `uv.lock`）+ 用仓库 `proto/` 生成 Python stub，运行 `python -m kiarabot_adapter_nonebot`，安装 curl，UID 10001，outbox 持久化到 `/var/lib/kiarabot-adapter`；
- `panel.Dockerfile`：`node:22` 构建 `VITE_API_MODE=live` 的静态产物，`nginxinc/nginx-unprivileged` 提供 8082 回环站点，并把 `/api/`、`/ws/`、`/health/ready` 反代到 Core。

## 启动顺序

1. `migrate` 服务获取迁移锁，用 `kiarabot-migrate` 完成数据库迁移；**迁移前**自动做 SQLite 一致性快照，迁移后校验 `PRAGMA user_version` 等于 `EXPECTED_SCHEMA_VERSION`（默认 6），失败即以非零退出并阻断后续服务；
2. Core 启动：打开数据库（再次校验 schema）、恢复 RUNNING、启动 BridgeServer 与控制 API，`/health/ready` 通过后 Compose 认为 core healthy；
3. 适配器在 core healthy 后启动，注册能力并开启 8081 健康服务；Core ready 不要求 QQ 在线，适配器 ready 依赖 Core 注册；
4. 面板在 core healthy 后启动（**不依赖适配器**，符合“Panel ready 依赖 API 可达，不要求 QQ 在线”），其 `/health/ready` 直接转发 Core ready；
5. NapCatQQ 由操作员在宿主机运行并连接适配器；Compose 不管理它。

## 环境变量与配置注入

单一权威来源是 `deploy/.env`（或 `KIARABOT_ENV_FILE` 指向的文件，`chmod 600`）：Compose 用它做插值，也通过 `env_file` 注入 core/migrate/adapter 容器。模板见 `deploy/env.example`，至少需要：

- Core：`KIARABOT_RUNTIME_DB`、`KIARABOT_ADAPTER_ACCOUNTS`、`KIARABOT_ADAPTER_ID`、`KIARABOT_ADAPTER_TOKEN`；可选 `KIARABOT_PLUGIN_ID/TOKEN/PERMISSIONS`、模型三件套。
- Core 运行时容量/维护参数（W2 新增，`core/app/main.cpp` 读取并校验范围，非法值启动退出 1）：
  `KIARABOT_EVENT_QUEUE_LIMIT`（默认 1024）、`KIARABOT_AUDIT_LIMIT`（100000）、`KIARABOT_ACTION_LIMIT`（10000）、`KIARABOT_ACK_TIMEOUT_MS`（30000）、`KIARABOT_MAINTENANCE_INTERVAL_MS`（60000）、`KIARABOT_RETENTION_HOURS`（168）。
- Core 出站 Action 配额（`action_quota.hpp`，范围 1–100000，非法值启动退出 1；窗口固定为 60 秒/60 秒/10 秒，仅限制值可配）：
  `KIARABOT_ACTION_ACCOUNT_LIMIT_PER_MINUTE`（默认 10，单账号所有 Action）、`KIARABOT_ACTION_GROUP_LIMIT_PER_MINUTE`（默认 3，单群）、`KIARABOT_ACTION_PRIVATE_LIMIT_PER_10_SECONDS`（默认 1，单私聊会话）。配额为进程内滑动窗口，重启重置；超限返回可重试 `RATE_LIMITED` 并写审计。默认值来自 `doc/17` 的起始建议，必须在真实平台实测后调整。
- Core 危险 Action 每日预算：`KIARABOT_ACTION_DAILY_LIMITS`（逗号分隔 `<action kind>=<limit>`，limit 1–100000，默认空=不限制；非法格式启动退出 1）。预算按 `(action kind, 账号)` 与 UTC 日持久化计数，跨重启保留，维护循环清理过期日；超限返回可重试 `RATE_LIMITED` 并写审计。当前只为危险动作配置（如 `action.mute_member.v1=20,action.kick_member.v1=5`）。
- Core 危险 Action 会话白名单（fail closed）：`KIARABOT_ACTION_ALLOWED_CONVERSATIONS`（逗号分隔 `<action kind>=<会话>|<会话>...`，默认空；非法格式启动退出 1）。kind 一旦出现，只允许目标会话在白名单内的提交，其他返回 `PERMISSION_DENIED`；未出现的 kind 不受此配置限制。**部署禁言/踢人时必须为 `action.mute_member.v1`、`action.kick_member.v1` 配置白名单**，并同时配置每日预算与速率上限。
- 控制面：`KIARABOT_CONTROL_CONFIG=/etc/kiarabot/control.json`（容器内路径）与 `KIARABOT_CONTROL_PORT`（默认 8080）。宿主机文件路径由 `KIARABOT_CONTROL_CONFIG_FILE` 指定并只读挂载；文件必须属于 UID 10001、权限 0600、普通文件且非符号链接，否则 Core 拒绝启动。`deploy/control/control.example.json` 给出字段模板，`encryption_key` 用 `kiarabot-control-credentials --key` 生成，密码哈希用同工具从 stdin 生成。
- 适配器：`CORE_GRPC_ENDPOINT`、`CORE_AUTH_TOKEN`、`ADAPTER_ID`、`ACCOUNT_ID`、`ONEBOT_WS_URL`、`ONEBOT_ACCESS_TOKEN`、`OUTBOX_PATH`、`HEALTH_PORT`。`CORE_AUTH_TOKEN` 必须等于 Core 的 `KIARABOT_ADAPTER_TOKEN`。
- 面板：`VITE_API_MODE=live`、`VITE_API_BASE=/api/v1` 是**构建期**参数（由 `panel.Dockerfile` 的 build args 传入）；运行期只读取 nginx 模板变量 `KIARABOT_CONTROL_HOST/PORT`、`KIARABOT_PANEL_PORT`。Compose 中不再使用 `CORE_ENDPOINT`、`CONTROL_API_URL` 这类不存在的运行期变量。

systemd 的 `EnvironmentFile=-/etc/kiarabot/production.env` 只进入 `deploy.sh` 进程，用于 Compose 插值；**不会**自动进入容器。容器环境来自 `deploy/.env`（`KIARABOT_ENV_FILE`）。如需单一文件，请把 `KIARABOT_ENV_FILE` 指向同一路径。

## 健康检查

每个进程必须提供（`api/openapi.yaml` 是根路径探针的唯一契约来源）：

```text
/health/live     进程是否存活（不访问存储）
/health/ready    是否可以接收请求（Core 做无业务修改的写事务探针）
/api/v1/health   认证后的组件详情（需 health.read 权限）
```

- Core：根路径 live/ready 由控制 API 提供；未 ready 时 API 返回 503；适配器/插件状态在 `/api/v1/health` 聚合。
- 适配器：`HEALTH_PORT`（默认 8081）上的 `127.0.0.1:/health/live`、`/health/ready`；ready 快照包含 `core`（是否已注册/连接）与 `platform`（是否有 OneBot bot 连接）组件，端口被占用只记录错误，不杀死进程。
- 面板：nginx 的 `/health/live` 返回静态 200（进程存活），`/health/ready` 转发 Core `/health/ready`（等价“API 可达即 ready”）；不要求 QQ 在线。
- 控制面传输层（W3）：支持 `OPTIONS`/`405 + Allow`、入站 `traceparent`/`X-Trace-ID` 传播并回写 `X-Request-ID`/`X-Trace-ID`、访问日志（JSON→stderr，含 method/path/status/duration/request_id/trace_id）；CORS 只在 `control.json` 的 `cors_origins` 精确白名单内放行（默认空=不做跨源），其余请求返回 429 + `Retry-After` 的限流按 `limits` 配置（认证 1000 rps/burst 2000，公开 30 rps/burst 60，默认值）。这些是传输层行为，不改变业务状态码语义。

## 日志

日志必须为结构化格式，并包含：

```text
timestamp
level
component
operation
outcome
request_id
trace_id
event_id / action_id
resource_id
message
error_code
```

每个操作的成功、拒绝、重复、超时、重试、死信、依赖故障和状态转换都必须产生至少一条结构化记录，并通过 `request_id`、`trace_id`、`event_id` 或 `action_id` 关联。日志中不得记录 Token、API Key、消息正文和未经脱敏的私聊内容。

部署侧当前提供：`scripts/deploy/*.sh` 的 JSON 日志（`timestamp/level/component/operation/outcome/request_id/trace_id/resource_id/message/error_code`，不含正文）、Core/适配器结构化日志、控制面访问日志；Compose 为每个服务配置 `json-file` 驱动并限制 `max-size=10m`、`max-file=5`。**尚未交付**：集中收集（如 vector/promtail）、保留期策略、日志告警。运维层不把日志通道当作命令通道。

## 备份和升级

- `.env`、控制配置和密钥不进入 Git/镜像；控制配置与 `encryption_key` 必须与数据库备份一起离线保管，丢失密钥会导致既有加密状态不可读；
- 数据库备份使用 SQLite `VACUUM INTO` 一致性快照，写入 `PRAGMA user_version`、SHA-256 和创建时间；快照与恢复前都执行 `PRAGMA integrity_check`；
- 备份与数据库**分卷**：`kiarabot-<env>-data` 挂载 `/var/lib/kiarabot`，`kiarabot-<env>-backups` 挂载 `/var/lib/kiarabot/backups`；默认保留最近 7 份，可用 `--retain`/`BACKUP_RETENTION` 调整。仍要求操作员定期复制该卷到独立受保护位置，本配置不提供异地复制；
- 恢复必须显式传 `--confirm`（或 `RESTORE_CONFIRM=1`），且目标数据库不得存在 `-wal/-shm`、迁移锁；恢复会校验校验和、`integrity_check` 和 `schema_version` 与 `EXPECTED_SCHEMA_VERSION`，不匹配拒绝覆盖；
- 插件配置、控制配置和适配器 outbox/.actions 不包含在 SQLite 备份中，需单独备份（见下文依赖）。

## 升级与回滚

- 迁移唯一权威命令是镜像内 `/usr/local/bin/kiarabot-migrate`；`MIGRATION_COMMAND` 默认值即该命令（`migrate.sh` 会把数据库路径作为唯一参数追加，也支持 `{database}` 占位符覆盖）。迁移失败或 schema 校验失败会阻止 Core 启动；
- 升级：`KIARABOT_CORE_IMAGE_DIGEST=sha256:<64 位十六进制> scripts/deploy/deploy.sh upgrade`。脚本停止面板/适配器/Core，用新镜像在数据卷内运行 `migrate`（先备份），成功后再按 core → adapter → panel 顺序等待 ready；失败不自动启动旧镜像，保留停机状态等待操作员核对 schema 与迁移前备份；
- schema v6 的会话键类型化迁移（session key 加入 conversation_type）在 Core 数据库迁移时自动完成：有保留事件的会话水位被重键为含类型的四元组，同一旧键同时覆盖群/私聊同号时两类密钥继承同一水位；没有保留事件的旧水位无法归因、被丢弃，升级后该会话首个事件可能报告一次 `DEGRADED`（正常现象，无需手工处理）。适配器本地 outbox 数据库在自己的 `user_version` 1→2 迁移中同步升级 sequence 键，同样自动完成；两端均需保留迁移前备份。
- 回滚：`scripts/deploy/deploy.sh rollback sha256:<digest>`。不传 digest 时使用 `<数据卷>/deploy-record.jsonl` 中最后一次成功记录；回滚前用 `sqlite3` 检查当前 `PRAGMA user_version` 等于 `EXPECTED_SCHEMA_VERSION`，不匹配则拒绝并要求先恢复迁移前备份（确认后可用 `ROLLBACK_FORCE=1` 覆盖）；镜像回滚不会自动恢复数据库；
- 每次 start/upgrade/rollback 都向 `deploy-record.jsonl` 追加一条 JSON 记录（time、environment、outcome、image、schema_version、commit=`KIARABOT_COMMIT`），用于审计追踪与默认回滚点；
- 部署后验证：`scripts/deploy/deploy.sh verify`（即 `smoke.sh`）检查 Core live/ready、可选认证 `/api/v1/health`、适配器 ready 与面板 ready；systemd 单元在 `ExecStartPost` 中调用它，smoke 失败使 unit 失败并可用 `ExecStop` 清理（`KIARABOT_SMOKE_SKIP=adapter,panel` 可裁剪检查项）；
- 升级窗口：当前 `upgrade` 是“先停后起”的计划内宕机窗口，不提供蓝绿/滚动；systemd `TimeoutStartSec=600` 超时会发送 SIGTERM，操作员需按部署记录判断栈状态。

## 不属于本层的内容

- 不修改业务数据绕过 API（备份恢复是文档化的停机例外）；
- 不在启动脚本中实现权限判断；
- 不把第三方源码复制进 Core；
- 不通过日志系统传递业务命令。

## 验收标准

- 任一进程退出后可被单独重启（Compose `restart: unless-stopped`；迁移服务一次性运行）；
- Core 和适配器断线可以恢复（连接退避在适配器侧）；
- 健康检查能区分存活和就绪（core/adapter/panel 三套探针语义见上）；
- 日志可以按 trace_id 关联一条消息链路（业务日志 + 控制面访问日志；集中收集待交付）；
- 数据库、插件配置和适配器 outbox 有备份方案（SQLite 自动快照已验证；插件配置/outbox 需操作员额外复制）。

## 当前实现状态与未验证事项（2026-10-31 W4）

- 本分支把部署骨架从“不可运行”推进为“可解释地接近可运行”：镜像构建启用 proto+infrastructure 并包含 `kiarabot-migrate`；Compose 补齐 `--grpc-listen`、持久化环境变量、控制配置挂载、host 网络与回环约束、adapter/panel 镜像、健康检查、日志驱动、独立备份卷与迁移权威命令。
- 宿主机依赖：Docker Engine + Docker Compose v2（`env_file.path/required`、`--wait` 需要较新版本）、`curl`（探针/smoke）、`sqlite3`（回滚前 schema 检查、备份校验）、bash；systemd 单元还需 `docker compose` 在 PATH 中。
- **本机没有 Docker**：`docker build`、`docker compose config/up` 均未运行；镜像构建、host 网络行为、健康检查与 Compose 依赖链的真实结果必须在有 Docker 的 Linux 环境或 CI 中验证，本分支不声称部署已验收（T11b 继续未完成）。
- 仍需外部环境验证：真实 NapCat/QQ 联调（T07b）、TLS 反代、集中日志、指标/告警、备份/恢复与回滚演练、真实 digest 发布、CI 镜像构建与推送。
- 生产 Compose 的全零 digest 是“不可拉取”的占位符；发布系统必须注入真实 digest。staging Compose 的镜像引用是 `:staging` 标签，且可通过 `build:` 从仓库构建。

## 其他模块需要完成的依赖

下列事项由对应模块负责人实现并验证；运维层不代替它们定义业务行为或修改公共契约：

1. **Core**：已提供持久化、控制 API、进程探针和 6 个运行时参数；运维镜像已按 proto+infrastructure 构建。剩余：`/health/ready` 在控制配置缺失时的错误信息（当前只返回 1，排障困难）、插件进程的容器化路径（运行层不含 Python，受管插件仍按 T03b 本机模型运行）。
2. **协议和控制 API**：根路径探针与认证 `/api/v1/health` 已在 OpenAPI 冻结；控制配置的 `cors_origins`/`limits` 字段由 W3 实现，运维模板已同步。
3. **基础设施/数据库**：独立迁移程序已随镜像发布；Core 打开数据库时仍会自动迁移 v1–v5→v6，因此运维必须保证先运行 migrate 服务（Compose 依赖已强制）；备份/恢复演练仍待 T11b。
4. **适配器与面板**：适配器健康接口与 `HEALTH_PORT` 已实现并有测试；outbox 容量/失败记录清理仍需适配器负责人给出运维导出/清理工具；面板是静态站点，运行形态由 `panel.Dockerfile` 固定为 nginx 回环反代，若要求公网 TLS 需另加反向代理。
5. **CI/CD**：镜像构建、GHCR 发布、staging 部署与部署记录尚未接入流水线（CI 自动触发暂停中）；本分支只提供 Dockerfile/Compose/脚本和本地单测，不声称 CI 已绿。
6. **发布验收**：依赖完成后必须用真实 digest 镜像与隔离数据重新验证迁移失败阻断、live/ready/details、启动顺序、备份恢复、指定 digest 回滚和旧版本 schema 兼容性。

## 代码启动句

> 请只实现 KiaraBot 各进程的 Docker Compose/systemd 启动配置、健康检查、结构化日志、重启策略和本地开发环境，不修改 Core、插件或适配器业务代码。

## 部署操作摘要

```bash
# 1) 环境与配置（一次性）
cp deploy/env.example deploy/.env && chmod 600 deploy/.env   # 填入真实值
cp deploy/control/control.example.json deploy/control/control.json
chmod 600 deploy/control/control.json && chown 10001:10001 deploy/control/control.json
# encryption_key / password_hash 用镜像内的 kiarabot-control-credentials 生成

# 2) 启动 / 状态 / 验证
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh start   # 需 KIARABOT_BUILD_IMAGES=1 首次构建
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh status
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh verify

# 3) 探针（不输出响应正文；详情需要 health.read token 文件）
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /health/live
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /health/ready
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /api/v1/health --token-file /path/to/token

# 4) 备份 / 恢复（恢复必须停机并显式确认）
docker compose -f deploy/compose.prod.yml run --rm --no-deps migrate /usr/local/bin/backup.sh backup
docker compose -f deploy/compose.prod.yml stop panel adapter core
docker compose -f deploy/compose.prod.yml run --rm --no-deps migrate \
  /usr/local/bin/backup.sh restore --input /var/lib/kiarabot/backups/<set>/database.db --confirm
KIARABOT_ENVIRONMENT=production scripts/deploy/deploy.sh start

# 5) 升级 / 回滚
KIARABOT_CORE_IMAGE_DIGEST=sha256:<64 位十六进制> scripts/deploy/deploy.sh upgrade
scripts/deploy/deploy.sh rollback sha256:<64 位十六进制>   # 省略参数时用 deploy-record.jsonl 最后成功记录
```

## T01b 本机传输验证

使用 proto 构建和 Core 文档中的 `--grpc-listen` 参数可启动本机桥接监听；凭据通过成对 ID/TOKEN 环境变量提供。禁止将回环明文监听当作生产 TLS 服务或 ready 端点。已有 Docker 默认 dev 构建仍不包含 gRPC，镜像和跨主机 TLS 在 T11 中集成。停止信号取消流、等待处理器退出后返回 0；绑定/凭据配置错误返回 1。

## SQLite 运行时开发验证

当前数据库初始化为 PRAGMA user_version=6，支持 v1–v5 事务升级并拒绝未知版本与损坏数据。T02a 已可选接入进程启动，健康探针及独立迁移命令也已实现。运行时快照包含事件、投递、会话、Action、插件、命令审计、Host 命名空间、任务、Action 审计目标与每日 Action 预算；完整生产备份与恢复仍须演练。

## T02a 持久化接收启动

使用 proto 且 KIARABOT_ENABLE_INFRASTRUCTURE=ON 构建后，可设置 KIARABOT_RUNTIME_DB（非空文件路径，禁止 :memory: 和 file: URI）及 KIARABOT_ADAPTER_ACCOUNTS（逗号分隔的非空账号允许列表），并保留 KIARABOT_ADAPTER_ID/TOKEN 和 --grpc-listen。启动先打开 SQLite/迁移，失败退出 1；配置仅部分提供时拒绝启动。未配置数据库时仍为传输认证模式，所有业务显式拒绝。接收可用只表示 adapter_ingress_ready=true，完整 rpc_ready 仍为 false；插件入口可按 T03a 配置启用，Action 纯文本/表情回应闭环已由 T04b 实现，模型和任务已接线，具体能力取决于配置。账号授权来自该配置，不能由适配器自报。

## 控制 API（T08b）

proto + infrastructure 构建可通过 `KIARABOT_CONTROL_CONFIG` 指定私有 JSON 文件，在同一 Core 进程启动 REST 服务。文件须为当前用户所有的普通文件且权限 0600，不得提交。字段包括 `listen`（127.0.0.1:端口）、`encryption_key`（32 字节的 hex）、`users`（id、username、Argon2id password_hash、role，及可选 `permissions` 显式权限数组）及可选 `plugins` 公共清单。角色为 admin/operator/viewer；用户名 ASCII 且不区分大小写。密钥丢失或变化会导致已有加密状态无法读取，必须与数据库备份一起安全保管。

构建提供 `kiarabot-control-credentials`：默认从 stdin 读一行密码并输出 Argon2id 哈希；`--key` 输出随机控制密钥。只在受保护终端/管道中运行，不在命令行传密码。不配置控制文件时原 gRPC 启动方式保持不变。控制服务要求现有持久化配置，不接受临时内存库作为部署状态。监听占用、无效配置或无法解密状态使启动失败。

请求体上限 64 KiB、HTTP 头上限 16 KiB、连接有界；登录每用户名/分钟限额，超限返回 429/Retry-After。服务绑定回环地址，浏览器通过同源代理连接；不启用宽泛 CORS，不信任代理头提供的身份。

## W5 messages 频道授权与 users[].permissions（2026-10-31）

控制配置的 `users[]` 支持可选 `permissions` 字符串数组，给单个用户显式追加权限；有效权限 = 角色权限 + 显式授予，`GET /auth/me` 与 WebSocket 流投影返回的都是有效权限。当前唯一允许的显式权限是 `message.read`：它不由任何默认角色持有（admin/operator/viewer 都没有），只能逐用户授予，授予后该用户可订阅 `/ws/messages`（只推送消息元数据，不含正文）。未知权限值、重复值或类型错误在 Core 构造配置时直接启动失败（fail-closed），不会静默忽略。

最小示例（仅展示 `users[]` 相关字段；`password_hash`/`encryption_key` 用 `kiarabot-control-credentials` 生成，文件须 0600 且不提交）：

```json
{
  "listen": "127.0.0.1:8080",
  "encryption_key": "REPLACE_WITH_64_HEX_CHARS_FROM_kiarabot-control-credentials_--key",
  "users": [
    {
      "id": "admin",
      "username": "admin",
      "password_hash": "$argon2id$v=19$m=65536,t=3,p=4$REPLACE_WITH_SALT$REPLACE_WITH_HASH",
      "role": "admin"
    },
    {
      "id": "auditor",
      "username": "auditor",
      "password_hash": "$argon2id$v=19$m=65536,t=3,p=4$REPLACE_WITH_SALT$REPLACE_WITH_HASH",
      "role": "viewer",
      "permissions": ["message.read"]
    }
  ]
}
```

安全提示：`message.read` 让该用户看到全站消息元数据（`message_id`、`conversation_id`、`direction`）。v1 不推送消息正文，但元数据仍可关联会话与收发方向，应按最小权限授予、为授予账号单独审计；该授予不改变角色、不扩大任何 REST 权限，也不能用于读取消息正文或历史。频道是内存投影：重启后不补历史，10 分钟重放且有界，不能当作审计。

## 结构化运行日志（T10b）

持久化运行模式通过日志存储装饰器写入有界结构化日志缓冲，sink 也支持可选 JSON 输出（daemon 默认不写阻塞 stdout），字段含 id、occurred_at、level、component、operation、outcome、resource、request_id、trace_id。禁止输出消息正文、模型请求、配置值、密码或令牌；标识限制为安全字符。sink 写失败不影响已提交业务，并累计失败计数。`GET /logs` 只对 operator/admin 返回十分钟内有界内存记录，日志不代替 `/audit` 的持久审计，也不是无限历史查询。


## T11a 独立迁移入口

`kiarabot-migrate DATABASE_PATH` 调用同一 SQLite schema v6 迁移实现；缺参数/非法路径退出 2，未知 schema、损坏快照、依赖故障或锁等待超时退出 1，成功 0。迁移依赖 SQLite 的 BEGIN IMMEDIATE 互斥，不创建可能遗留的文本锁文件；冲突最多等待三十秒并重试整个未提交迁移，其他错误不重试。重复运行安全，成功只表示数据库就绪，不宣称其他服务 ready。daemon 启动沿用相同迁移逻辑，坏 schema/无法解密控制状态禁止监听控制端口。

运行 `build/proto/infrastructure/kiarabot-migrate /path/to/runtime.db`；生产迁移前仍须备份，旧二进制回滚不得直接读取新 schema。该命令不绕过端口修改业务记录。

## 配置应用确认

启用私有 KIARABOT_CONTROL_CONFIG 且配置插件桥接身份/权限后，支持 plugin.config.v1 的 SDK 自动接收配置。PUT 成功后轮询 GET /plugins/{plugin_id}/config 的 delivery；APPLIED 才表示当前运行连接完成回调。REJECTED 需修复配置提交新版本或重连，UNAVAILABLE 需检查插件连接/停用/能力协商。Core 重启后重新确认，不能用旧进程的 applied_version 作健康证明。不新增配置项、SQLite schema 或真实环境验收要求。

## P02 六进程入口约定（实现中）

公共 runner 支持 --help（退出0、不监听）与 --config PATH；CLI/配置形状错误退出2，
证书/绑定/运行故障退出1，正常 SIGINT/SIGTERM 关闭退出0。P02 业务未实现，
ready 必须保持 false；live 仅表明进程能响应探针。具体配置和传输实现随本阶段补齐，
此接口提交不表示进程已经可以运行。

### P02 严格配置与 TLS 文件

JSON 根只允许 schema_version=1、service、probe_listen、tls；非 Control 另必需
 grpc_listen。service 使用 control/identity/runtime/plugin-host/scheduler/model；
 tls 只含 certificate_file/private_key_file/ca_file 三个字符串路径。未知/重复字段、
 类型/版本/服务不匹配、缺字段拒绝（退出2）。地址使用 IPv4 字面量及0–65535端口；
 probe_listen 必须为127.0.0.1，业务 grpc_listen 可绑定指定跨主机接口。端口0允许
 OS分配。路径按进程工作目录解析，建议部署配置用绝对路径。

配置/材料必须为非空常规文件，每文件最多1MiB，拒绝符号链接；私钥必须归运行用户
 所有且无组/其他用户权限（0600或更严）。配置不可读退出2；TLS文件不可读、PEM错误、
 证书未生效/过期、链验证失败、非CA根或私钥不匹配退出1。验证支持叶证书后附中间链，
 CA bundle均须在有效期；密钥不允许交互密码提示。诊断仅稳定原因，不回显路径、配置
 正文或PEM。此校验在任何监听前完成，不能关闭；业务身份/SAN登记与授权仍待P04，
 不能将链有效视为业务授权成功。

文件以非阻塞方式打开后检查 regular，FIFO 等特殊文件必须有界拒绝，不能在 open
等待写端。五服务入口的工厂装配异常也返回1及脱敏 INTERNAL，不输出异常 what()。

### P02 已实现监听与退出

五个 Core 入口为 identity_main.cpp、runtime_main.cpp、host_main.cpp、scheduler_main.cpp、
model_main.cpp；Plugin Host 的 CLI service 值是 plugin-host。每个入口接受相同配置规范。
启动成功输出一行 service、实际 probe_port、grpc_port、ready=false；不会输出凭据或
配置正文。端口冲突退出1；业务gRPC禁止共享监听端口。Control 使用同一runner但不监听gRPC。

探针明文HTTP仅回环：GET /health/live 为200 `{"state":"HEALTHY"}`，GET /health/ready
在P02为503 `{"state":"UNHEALTHY"}`，严格沿用ProcessProbe仅state结构；其他路径404，
探针非GET按既有BadRequest返回400/INVALID_ARGUMENT，包含脱敏message与request_id。Control 的 /api/v1/ 与 /ws/ 前缀返回503及
CAPABILITY_NOT_SUPPORTED，不挂接旧业务或升级WebSocket。该探针不是远程业务服务器。

所有非Control的业务gRPC使用配置CA验证客户端证书，未提供客户端证书不能到达RPC；
正确证书连接下已注册owner方法与Runtime三个桥接流均返回UNIMPLEMENTED。CA链检查
只证明传输证书有效，未实现P04的SAN/工作负载登记/受限委托/在线授权，不能将握手成功
视为可执行任何业务。探针就绪状态继续取application门禁，TLS成功不修改业务依赖状态。

SIGINT/SIGTERM在主线程同步消费。探针每连接500ms总预算，空闲轮询100ms，gRPC关闭
期限2秒；正常信号退出0，运行异常脱敏退出1。线程/socket/server均在runner返回前回收。
本阶段证书只在启动加载验证；热轮换、到期后业务连接收敛与注册授权仍待P04，ready始终false。

证书时间边界为 notBefore <= 当前验证时刻 < notAfter；起始恰好相等合法，ASN1时间
解析失败拒绝。配置校验时刻支持内部测试注入，CLI/配置不能覆盖服务器当前时间。

## P03 PostgreSQL 显式初始化（实施中）

四个私有逻辑库分别部署 Identity/Runtime/Scheduler/Model。集群管理员先创建独立的
无超级权限迁移/运行登录、收紧 pg_hba 并撤销数据库 PUBLIC CONNECT/CREATE/TEMP，
只授予本库两种身份 CONNECT；迁移身份拥有本库，运行身份不得加入迁移角色。
再以迁移身份执行显式空库初始化。数据库/角色部署不是 schema 初始化的一部分。
禁止对已有开发数据清库、导入旧快照或建立双写。

初始化在单库事务中创建受限 schema 和版本标记，失败回滚；跨四库不是原子事务，
部分库成功时必须分别检查，不能宣称全套成功。重复、非空、半初始化、版本不匹配拒绝。
运行服务只验证版本/对象/权限并获取库内活动写入锁，不执行 DDL；数据库锁或连接失效时
停止受理/派发/续租。所有连接强制 verify-full/channel_binding=require，超时有界；
错误不打印数据库配置和秘密。新命令/组合根接线的实际状态以 P03b 模块报告为准。

初始化 CLI：`kiarabot-pg-initialize --initialize-empty SERVICE HOST PORT DATABASE MIGRATION_ROLE RUNTIME_ROLE CA_FILE PASSWORD_FILE`。
SERVICE 只允许 identity/runtime/scheduler/model；只以迁移登录连接。
密码文件必须是当前用户所有、无组/其他权限、1–4096字节的普通文件，拒绝符号链接/FIFO；
末尾允许一个换行，不允许多行、NUL。密码不进入命令行。`--help`退出0；
参数/密码文件错误退出2，存储/权限/版本/非空拒绝退出1，初始化成功退出0。
提交回执丢失输出 `INITIALIZATION_UNKNOWN` 并退出1，须先检查原库状态，不自动重跑；
失败且完整回滚的空库可再次显式初始化，已成功或半初始化库拒绝。
连接配置拒绝环境中的 PGSERVICE/PGSERVICEFILE/PGHOSTADDR/PGOPTIONS/PGSSLCERT/PGSSLKEY/PGPASSFILE，
避免环境连接配置覆盖受控策略。CLI 不检查业务就绪，不启动 daemon。

### P03 数据库配置材料（装配前置）

仅Identity/Runtime/Scheduler/Model的schema_version=1配置可额外包含database对象，
键严格为host、port、name、migration_role、runtime_role、password_file、ca_file。
port为1–65535整数；单主机DNS/IP，不接受多host、Unix socket或DSN；库名/角色为
1–63个小写字母/数字/下划线，两角色不同。运行身份固定选runtime_role，配置不能
覆盖TLS、channel binding或超时。Control/Host出现该对象拒绝（退出2）。

密码只引用文件，不写入JSON/argv；文件归当前用户、无组/其他权限、常规非symlink，
原始文件最多4096字节，可有一个末尾LF，内容非空且无其他CR/LF/NUL。CA同样要求
非空常规非symlink文件，具体链/主机验证由PG连接完成。类型/值错误退出2，材料
不可读/权限不安全/FIFO等退出1，日志不显示文件路径、密码或连接异常正文。

本项只交付配置解析前置，尚未将factory装配到daemon，不表示数据库已连接或ready；
后续组合根接线才做监听前schema/writer验证，daemon始终不自动DDL。缺database时
目前未实现业务继续不ready，不允许以后以无数据库方式处理持久业务。

### P03 配置后组合与依赖维护钩子

外层run_service支持在CLI及材料校验通过后调用受审组合器；--help/无效输入不调用它，
先屏蔽SIGINT/SIGTERM再允许组合器创建工作线程。组合为空、owner不匹配、显式失败或
异常均在监听前退出1并脱敏；接口不能从配置中指定任意回调或脚本。

依赖钩子在监听前与运行后每秒由进程线程调用，必须有界；持久故障退出1并关闭RPC/
socket，不继续受理。暂时忙等可恢复情况由具体组合根区分，不把忙当writer失锁。
销毁顺序是RPC/监听→application→回调持有的依赖→恢复信号mask。内层readiness仍只读
快照，不在探针中查询数据库；仅有这些通用钩子不表示PG或业务已经装配。

### P03 common transaction limits

P03 schema当前为v3，要求UTF8数据库；旧v1/v2或其他结构不自动升级，必须按本轮空库部署
流程初始化新的专属库，不能清空原库或自动补表。schema结构描述/对象owner/运行权限均参与
启动验证。当前仅存储公共能力，业务就绪仍由各服务已实现依赖决定。

公共事务在同一持锁session上串行；并发begin最多等待250ms后以Conflict表示忙，不能据此
认定失锁。事务总预算2秒，SQL锁等待250ms；RAII deadline使用复制的同session socket，
到期只关闭该socket且等待线程收尾，避免fd重用误关闭别的连接。超时/失连接后旧实例停止
业务，不能在后台重连。已发送COMMIT但回执丢失为Unknown，关闭旧session释放活动锁，
通过新实例按原收据/交接身份查询，不能自动重放副作用。显式rollback不能撤销Unknown。

每库迁移/运行身份必须使用独立秘密，各库也不得复用相同密码；迁移密码只给显式初始化，
daemon只持运行密码。PG CA路径必须是可读、非符号链接、1字节至1MiB的普通PEM文件，
拒绝FIFO等特殊文件，避免libpq打开信任材料时越过连接超时。仍由TLS完成链与主机名验证。
数据库连接强制target_session_attrs=read-write，拒绝只读会话/只读副本，不能仅凭版本及
SELECT可用就当作具有业务写入能力；本轮不提供只读副本或自动故障切换路径。
