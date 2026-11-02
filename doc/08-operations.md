# 部署与可观测性设计

T07b 真实联调环境尚未提供（用户已确认）。准备说明和不含密钥的模板见 [真实联调环境准备](../tests/acceptance/real-environment.md)。先以 event-observer 验证接收，模板不自动发送平台消息或模型请求；真实收发、模型和恢复结果仍待实际环境验收，不能用 T07a 替代。

T06c 持久化插件入口装配 TaskScheduler 并协商 host.tasks.v1；插件权限显式添加 task.schedule，定时事件订阅为 event.timer.triggered。数据库升级至 v5，包含任务/领取 token；启动在开启监听前恢复 RUNNING，恢复失败拒绝启动。一个数据库只运行一个 Core；替换进程必须先停止旧进程。任务每 100ms 检查，系统必须安装 IANA zoneinfo（Linux tzdata）；备份任务/inbox/幂等记录须保持同一快照。COMPLETED 只表示入队，排障同时检查插件投递状态。

T06b 可选配置 KIARABOT_MODEL_ENDPOINT（完整 /chat/completions URL）、KIARABOT_MODEL_API_KEY 和 KIARABOT_MODEL_NAME，三者必须同时设置且持久化插件入口已启用；部分/非法配置启动失败 INVALID_ARGUMENT。未配置时不协商 host.model.v1。插件须获授 model.chat。只允许 HTTPS 或本机 127.0.0.1 HTTP；禁用环境代理/重定向，证书校验开启，最大请求三十秒，停机可能等到该调用期限。密钥不要放命令行或入库；受管 Python 环境白名单不包含任何模型配置。模型只读探活与实际外部供应商联调仍未验证。

T06a 持久化插件入口装配 HostDispatcher；存储、可选模型和任务能力按实际配置协商，storage.namespace 必须显式列入 KIARABOT_PLUGIN_PERMISSIONS。T06a 引入 schema v4，当前版本升级为 v6，回滚使用迁移前备份。Host 存储和幂等响应随 Core 数据库一起备份，不能单独删除幂等响应后重放写入。

T04b 启用持久化插件入口时装配 ActionRouter，插件 message.send 权限仍显式配置；只声明已实现的 action.result.v1。T04b 引入 schema v3，当前已继续升级至 v6，旧二进制不能读取新 schema，回滚必须使用迁移前备份。Python 适配器在 OUTBOX_PATH 旁维护 .actions 执行日志，备份/恢复须与事件 outbox 一起处理，禁止删除未终态记录后重放平台请求。

### P11a Model 根密钥配置（可选，仅 model 服务）

P11 起 model 配置允许 `model_security: {"root_key_file": "...", "key_version": 1}`：`root_key_file` 必须为 32 字节 hex、0600、非符号链接的部署文件，组合根只读取一次；`key_version` 为单调正 uint64。缺失/非法/其他服务携带即退出 2；运行期解密失败 fail closed（不启动 Provider、不写调用意图）。根密钥不进入数据库、日志、审计、outbox、argv 或面板；P11b 起模型进程默认使用 libcurl HTTP Provider（无额外进程级 endpoint/key 环境变量；模型 endpoint/model/超时/并发送由 Model 私有配置写入，secret 只经 AEAD 解封），P11c 起配置顶层 `identity_endpoint` 后使用真实 Identity 委托/CheckAuthorization 校验调用者，Identity 通道可用即 ready=true（组合根仍 AND DB）；未配置时保持 fail-closed/not-ready。Runtime 另可用 `model_endpoint` 出站 Model Chat；当前 P05 实例委托获取未接线，Runtime 默认 fail-closed，直到委托来源接入。Provider 请求/响应不写日志，错误只保留稳定码；上游 429 的 Retry-After 以有界提示透传，5xx/超时/畸形保持 UNKNOWN。

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
- 适配器（P06 target bridge）：`BRIDGE_ENDPOINT`、`BRIDGE_CA_FILE`、`BRIDGE_BOOTSTRAP_CREDENTIAL_FILE`（0600 一次性 bootstrap）、`ADAPTER_INSTANCE_ID`、`ACCOUNT_ID`（Runtime 账号绑定身份）、`OUTBOX_PATH`、`HEALTH_PORT`，平台侧 `ONEBOT_WS_URL`/`ONEBOT_ACCESS_TOKEN`；可选 `ADAPTER_ID`/`BRIDGE_SERVER_NAME`/超时与重试变量。reference 时代 `CORE_GRPC_ENDPOINT`/`CORE_AUTH_TOKEN` 仅存在于冻结 reference 分支/tag，目标进程不读取。
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
- P12 四依赖聚合：`/api/v1/health` 需 `health.read`，逐组件返回 `identity`/`runtime`/`scheduler`/`model`。Identity 不可用、凭据链过期或撤销链无法证明 → 整体 `UNHEALTHY`（503），匿名 `/health/ready` 同时 503；Runtime/Scheduler/Model 任一单点故障 → 整体 `DEGRADED`（200），只让对应功能返回 `UPSTREAM_UNAVAILABLE`，不整体摘流；Control 自身存储/进程故障 → 未就绪。`/health/live` 与匿名 `/health/ready` 不暴露组件细节；组件观测时间由 Control 记录，告警不得用本地时钟推断。
- 适配器：`HEALTH_PORT`（默认 8081）上的 `127.0.0.1:/health/live`、`/health/ready`；ready 快照包含 `core`（是否已注册/连接）与 `platform`（是否有 OneBot bot 连接）组件，端口被占用只记录错误，不杀死进程。
- 面板：nginx 的 `/health/live` 返回静态 200（进程存活），`/health/ready` 转发 Core `/health/ready`（等价“API 可达即 ready”）；不要求 QQ 在线。
- Control Model 管理面需要可选配置 `control_api.model_endpoint`（`host:port`，与 Control 其它 owner endpoint 使用同一 mTLS 材料）。未配置时 `/api/v1/models/*` 管理路由返回 503 UNAVAILABLE；配置后每个请求都发一次性 Identity 委托并重新在线授权，Control 不缓存授权、不读取/记录 Provider 密钥。运维排障可查稳定码（CONFLICT/NOT_FOUND/UPSTREAM_UNAVAILABLE/TIMEOUT），但不得把查询超时当作 Provider 未执行或未计费。
- P12 告警口径：Identity/凭据链/撤销链未就绪 → 立即告警并整体摘流（ready=false）；Runtime/Scheduler/Model 单点故障 → 功能级告警（对应 API 返回 `UPSTREAM_UNAVAILABLE`），不得因单点故障重启 Control 或清空面板缓存伪造健康。组件详情以受权 `/api/v1/health` 的 Control 观测时间为准，告警规则不得用本地时钟重算 ready。
- P12 命令与事实分离：安装/实例/配置的 202 只表示已受理；进度必须按 operation 查询。期望状态/配置版本（Runtime）、受管进程启动/退出（Host）、配置已应用（插件）是三类独立事实，运维排障不得用其中一类替代另一类。日志与审计是分开的敏感读数据，`log.read`/`audit.read` 分别授权，缺失权威来源时返回 UNAVAILABLE 而不是空页。
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

## P06 Bridge 全链路验收

`ctest -R 'p06_client_e2e'`（tests/architecture）自建一次性 PG16.4 集群、临时证书并启动真实
Runtime，使用真实 SDK 插件与 Adapter bridge client 验证注册/授权/refresh/事件投递/租约/
ACK/重连/ShutdownNotice；不连接真实 QQ，不读取生产凭据。它与 `p06_bridge_acceptance` 的
wire 级断言互补：后者锁协议帧，前者锁已发布客户端。

## 验收标准

- 任一进程退出后可被单独重启（Compose `restart: unless-stopped`；迁移服务一次性运行）；
- Core 和适配器断线可以恢复（连接退避在适配器侧）；
- 健康检查能区分存活和就绪（core/adapter/panel 三套探针语义见上）；
- 日志可以按 trace_id 关联一条消息链路（业务日志 + 控制面访问日志；集中收集待交付）；
- 数据库、插件配置和适配器 outbox 有备份方案（SQLite 自动快照已验证；插件配置/outbox 需操作员额外复制）。

## 当前实现状态与未验证事项（2026-10-31 W4）

- 本分支把部署骨架从“不可运行”推进为“可解释地接近可运行”：Dockerfile 静态配置启用 proto+infrastructure 并复制 `kiarabot-core`/`kiarabot-migrate`（构建路径 2026-11-02 才与本仓库 CMake 布局对齐，见本节最后一目）；Compose 补齐 `--grpc-listen`、持久化环境变量、控制配置挂载、host 网络与回环约束、adapter/panel 镜像、健康检查、日志驱动、独立备份卷与迁移权威命令。
- 宿主机依赖：Docker Engine + Docker Compose v2（`env_file.path/required`、`--wait` 需要较新版本）、`curl`（探针/smoke）、`sqlite3`（回滚前 schema 检查、备份校验）、bash；systemd 单元还需 `docker compose` 在 PATH 中。
- **本机没有 Docker**：`docker build`、`docker compose config/up` 均未运行；镜像构建、host 网络行为、健康检查与 Compose 依赖链的真实结果必须在有 Docker 的 Linux 环境或 CI 中验证，本分支不声称部署已验收（T11b 继续未完成）。
- 仍需外部环境验证：真实 NapCat/QQ 联调（T07b）、TLS 反代、集中日志、指标/告警、备份/恢复与回滚演练、真实 digest 发布、CI 镜像构建与推送。
- 生产 Compose 的全零 digest 是“不可拉取”的占位符；发布系统必须注入真实 digest。staging Compose 的镜像引用是 `:staging` 标签，且可通过 `build:` 从仓库构建。
- **2026-11-02 静态核对与修正（未运行 Docker）**：原 Dockerfile 与本仓库 CMake 布局不一致，镜像无法构建——`--target kiarabot-core` 不存在（实际 target 是 `kiarabot-reference-core`，`kiarabot-core` 只是 `OUTPUT_NAME`，产物在 `build/reference/core/`），`kiarabot-migrate` 在 `build/reference/infrastructure/`；builder stage 的经典 vcpkg 包列表也缺少 `dependencies/architecture` manifest 要求的 libpqxx/jwt-cpp/nlohmann_json/OpenSSL 3.5.0 EXACT 等版本。现已改为固定 registry checkout + `vcpkg install --x-manifest-root=dependencies/architecture`，并补齐 ruby、openssl CLI、bison/flex/perl 构建前置，`.dockerignore` 允许 manifest 目录进入上下文。本轮只做静态路径/参数核对，**未运行 `docker build` 或 `docker compose`**；镜像内二进制、健康检查与 Compose 依赖链仍未验证，T11b 仍未完成。

## 其他模块需要完成的依赖

下列事项由对应模块负责人实现并验证；运维层不代替它们定义业务行为或修改公共契约：

1. **Core**：已提供持久化、控制 API、进程探针和 6 个运行时参数；运维镜像已按 proto+infrastructure 构建。剩余：`/health/ready` 在控制配置缺失时的错误信息（当前只返回 1，排障困难）、插件进程的容器化路径（运行层不含 Python，受管插件仍按 T03b 本机模型运行）。
2. **协议和控制 API**：根路径探针与认证 `/api/v1/health` 已在 OpenAPI 冻结；控制配置的 `cors_origins`/`limits` 字段由 W3 实现，运维模板已同步。
3. **基础设施/数据库**：独立迁移程序已随镜像发布；Core 打开数据库时仍会自动迁移 v1–v5→v6，因此运维必须保证先运行 migrate 服务（Compose 依赖已强制）；备份/恢复演练仍待 T11b。
4. **适配器与面板**：适配器健康接口与 `HEALTH_PORT` 已实现并有测试；outbox 容量/失败记录清理仍需适配器负责人给出运维导出/清理工具；面板是静态站点，运行形态由 `panel.Dockerfile` 固定为 nginx 回环反代，若要求公网 TLS 需另加反向代理。
5. **CI/CD**：镜像构建、GHCR 发布、staging 部署与部署记录尚未接入流水线（CI 自动触发暂停中）；2026-11-02 只把 contracts/full-build 的 C++ 依赖安装改为锁定 `dependencies/architecture` manifest，full CTest 仍缺 P00 fixture 环境（PG16.4/OpenSSL3.5/Python venv），且未在远端 CI 验证；本分支不声称 CI 已绿。
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

## P14a 目标部署面（Docker/Compose，未验证）

- 状态：**已实现 P14a 交付文件；本机无 Docker，镜像构建/Compose 启动/healthcheck/初始化/两主机行为均未执行、未验证**。不得把下文当作已验收部署。两主机拓扑（Host-A 数据/控制面 + Host-B 执行面）按 P14 提案文档化，**待用户确认**；P13 的 drain/生命周期设计未合入，启停顺序只是临时口径。
- 交付物：`deploy/docker/suite.Dockerfile`（Ubuntu24.04 + 固定 vcpkg registry/manifest + Python3.11.18/uv0.13.6 生成 proto，构建 `kiarabot-control/-identity/-runtime/-plugin-host/-scheduler/-model` + `kiarabot-pg-initialize` + `kiarabot-identity-bootstrap`，共享 runtime base 与六个薄镜像 + tools）；`deploy/compose.infra.yml`（PG16.4/四私有库、TLS/数据/备份卷、显式 init profile）；`deploy/compose.core.yml`（六服务、只读 rootfs/非 root/资源限制/healthcheck/只读 config-TLS-secrets 挂载）；`deploy/compose.exec.yml`（Plugin Host + Adapter，Adapter 使用 P06c `BRIDGE_*`/`ONEBOT_*_FILE` 与 bootstrap 秘密）；`deploy/target.env.example`、`deploy/target/README.md`、`scripts/deploy/target-initialize.sh`、`scripts/deploy/target-smoke.sh`、`deploy/README.md`。
- 拓扑/端口（提案默认，待确认）：Host-A 的 PG 5432（不公网）+ Identity 7441/probe 9081、Runtime 7442/ bridge 7443/probe 9082、Scheduler 7444/9083、Model 7445/9084、Control 8080/9080、Panel 8082；Host-B 的 Plugin Host 7446/9085、Adapter health 8081；所有 owner gRPC 用 mTLS，Runtime bridge 用 server-verified TLS + Bearer bootstrap，Panel 仅经反代/隧道对外。端口可在 `deploy/target.env` 覆盖并按防火墙收敛。
- TLS/秘密：部署自建 CA/leaf（SAN 精确覆盖连接名）、PG server CA、四库迁移/运行双密码、Identity enrollment/admin密码、model root key（32B hex/0600）、Control encryption key、Adapter/插件 bootstrap 与 OneBot token 均以只读文件挂载；不进入镜像、argv、日志或 git。当前只有启动校验，无热轮换；P14 采用“新文件 + 按顺序重启”的手动轮换。
- 初始化/迁移：`compose.infra.yml` 的 `init-{identity,runtime,scheduler,model}` 与 `identity-bootstrap` 是显式 profile/一次性命令；daemon 不执行 DDL、不清库、不自动迁移。角色/数据库创建属 DBA/部署前置；四库非原子，必须逐库检查，失败/半初始化拒绝重跑（P03 规则）。
- 健康/依赖：Compose healthcheck 使用各服务 `/health/live`/`/health/ready`（P02 语义）；readiness 覆盖 schema/版本/锁/TLS/依赖；Control 聚合与降级口径待 P12c/P13（Q5），不把 live 当业务 ready。启动顺序为 PG→显式初始化→Identity→Runtime→Scheduler/Model→Host→Control→Panel，停止反向；P13 合入后替换为正式 drain。
- 单机 dev 与生产差异：dev 用 loopback 发布端口 + 单机三套 compose；生产两主机 + 私有网络/防火墙 + 镜像 digest + 只读挂载 + 资源限制，且不发布 PG/owner/bridge 到公网。reference `compose.{prod,staging}.yml`/根 Dockerfile 仍是单 Core+SQLite，不可用于目标部署。
- 检查/未运行：`./scripts/deploy/target-smoke.sh` 已在本机离线通过（Compose YAML 解析、服务集合、非 root/只读/no-new-privileges、Dockerfile 目标/二进制、shell 语法、JSON 模板存在时校验）；Docker 缺失时脚本输出 UNVERIFIED 并以 0 退出（`--require-docker` 可强制失败）。`docker build`、`docker compose config/up`、PG 初始化、healthcheck、两主机故障矩阵、真实 QQ/Model 均**未运行**；hadolint 未安装。与 P14 提案差异：P13 drain 未定、两主机/镜像仓库/秘密方式仍待用户确认、Ubuntu base 默认 tag 仅本地构建（release 需 digest，smoke --release 强制校验）。

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

P06 起 runtime 配置允许可选的 `bridge_listen`（IPv4 字面量:端口，仅 runtime 服务；
其他服务携带即退出2）：用于外部 Adapter/Plugin 的 Bridge 长连接。它必须与
grpc_listen 不同（相同则退出2），复用同一 tls 证书/私钥/CA 材料，但**不要求也不验证
客户端证书**（server-verified TLS；外部客户端用部署 CA 校验服务端证书和 SAN）。
外部 `bridge_listen` 只注册 AdapterBridge/PluginBridge，使用 server-verified TLS、不请求客户端证书；
bootstrap 以 gRPC metadata `authorization: Bearer <credential>` 交付一次，只存 SHA-256，
proof/refresh 仅存进程内存。SIGTERM/SIGINT 时先向活动 bridge 流发有界 ShutdownNotice
（drain_timeout_ms，1s 预算）再关闭监听；客户端未收到该帧时按“结果未知”重连，不能假定投递成功。
grpc_listen 的平台 owner mTLS 监听保持强制客户端证书，两个监听在同一进程、同一
application 上并存；bridge_listen 缺省时不启动外部监听，且 Runtime 已注册 Bridge
服务时必须配置该项（否则组合失败退出1）。bridge 端口不得作为匿名公网入口发布，
部署须由防火墙/反向代理/受控网络限制，并确保服务端证书 SAN 覆盖客户端使用的主机名。
对外监听状态写入启动日志的端口信息（不输出凭据/配置正文），探针就绪仍由 application
门禁决定，不因外部监听启动而变 ready。

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


P03 公共 schema 为 v3；P04 Identity 空库含安全关系、完整会话结果、当前 bootstrap 定位、源 Control 会话绑定、平台当前定位索引、Assignment墓碑、完整普通结果、Pending 凭据受众关系及一个 binding 只对应一个 Principal 的唯一索引后为 v11，另三库为 v5。要求UTF8数据库；
旧版本或其他结构不自动升级，必须按本轮空库部署
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

### P03 daemon 数据库接线

四个数据库服务入口现在实际调用各自typed factory，在监听前完成TLS/schema/writer
校验。daemon始终使用runtime_role，迁移密码不进入其配置；连接/锁/版本拒绝退出1，
不输出监听成功行，不执行DDL。Control和Plugin Host不创建这些数据库连接。
运行后每秒以同工厂短事务检查数据库当前UTC；忙等待最多250ms不当作失锁，事务故障
按2秒总预算终止旧session。永久依赖失效记录reason=dependency_lost并退出1，关闭
RPC/探针；由运维启动新进程重新验证/取得锁，不能由旧实例静默恢复继续业务。
本项只证明持久依赖已接入，当前业务仍未完成，/health/ready继续503；普通SIGTERM退出0。
未配置database时当前未实现业务可启动不ready，拒绝型UoW不允许持久业务回退到内存。

### P04 RPC 装配生命周期

受审进程组合根可登记本owner的RPC适配器，配置不能指定脚本、动态类或任意服务表。
登记失败和异常在所有监听前退出1，日志不打印异常正文。无业务适配器仍返回现有
UNIMPLEMENTED；平台监听继续强制mTLS，不能因换入handler改为无证书访问。
退出先停止并等待RPC、回收handler，再释放application/依赖，避免在途请求访问已
销毁对象。当前新增装配钩子不表示任何认证RPC已实现，业务ready继续由应用门禁决定。
数据库名及迁移/运行角色名限定1–63个小写ASCII字母、数字或下划线；单主机地址、端口
1–65535，不接收DSN、多host或Unix socket覆盖受控策略。

### 凭据登记、轮换与时间投影（实现中，待完整验收）

主体登记返回 Disabled Principal 与 PENDING 初始凭据引用（allowed_services 持久化为 Pending audiences），保存其 ID/版本后经 RotateCredential 首次发放；首轮成功轮换在同一事务激活 principal，WorkloadBootstrap 轮换交付一次 opaque token（24h，可信策略），human Password 轮换交付一次服务端生成的临时密码，两者都受可信策略期限约束；CertificateBinding 轮换由证书登记流程负责，该 RPC 明确返回 CAPABILITY_NOT_SUPPORTED。GetCredentialStatus 只查询状态，不能恢复秘密。PENDING 无到期时间，不可用于连接；ACTIVE/EXPIRED 有权威到期，撤销保留历史状态而不再发秘密；撤销后该凭据的 Login/Validate 必须失败。加密交付收据用 keyed request commitment（含 expected_version），同键异版本 CONFLICT；未授权与不存在统一 NOT_FOUND；v1 一个精确 binding 只对应一个 Principal。禁止记录 token、refresh、密钥/摘要。会话 created_at 首次持久化后不变，Control 按 Identity checked_at 判定到期和剩余 TTL，不可补本机时间。应用/仓储/RPC 已接线，readiness 以 bootstrap Consumed、DB writer、凭据装载与可信时钟为门禁。

### P04 peer 证据与登记

gRPC TLS证书证据由真实AuthContext读取，受8张/64KiB上限约束，指纹来自实际leaf。
缺失/重复/不安全transport或不可解析证书拒绝；不记录证书、秘密或自由诊断正文。
证据不等于业务授权：即使CA验证成功，也必须查询当前Identity登记与凭据/SAN绑定。
连接复用缺少中间链时不能自造证书；后续受信链验证无法完成须拒绝/重新建立完整链。

### 外部连接授权目标补全（未实现）

platform.proto三类ConnectionAuthorization/RefreshConnectionAuthorization/Result与两Bridge的登记、proof、refresh帧定义短期外部连接授权。名义最多56秒、总预算60秒、每秒检查失效，单飞原子轮换；丢响应须关闭旧流、新bootstrap与连接代次。完整规则见doc/01-contracts及运行语义；内部JWT委托和P02 mTLS owner监听不变，P05/P06真实接线前仍not_implemented。授权材料禁止日志/公开投影/持久秘密收据，高风险业务仍在线核验，刷新不续投递租约、不改历史业务事实。浏览器REST/WS不使用这些proof，OpenAPI/AsyncAPI因此无schema变更。

P06 bootstrap 的运维约定（未实现）：登记凭据由 Runtime 逐实例生成，只经一次响应
（`RuntimeBindAdapterResponse.bootstrap_credential` / 仅 externally_managed 实例的
`RuntimeCreateInstanceResponse.bootstrap_credential`）交付给 Control/部署流程；Runtime
只保存 SHA-256，明文必须放入 adapter/plugin 的部署秘密（环境/文件 0600），不得进入
日志、面板回显、备份明文或 outbox。外部客户端以 `authorization: Bearer <credential>`
连 `bridge_listen`；撤销随实例停用/解绑生效，proof/refresh 到期或依赖故障时停止新副作用
并让客户端重新 bootstrap。禁止把 bootstrap 当作长期业务授权或绕过逐操作在线授权。

### P04 密码学适配器运行约束（部分实施）

Identity 密码适配器依赖固定 libsodium 1.0.20，需预算每次密码操作64MiB及相应CPU，
应用在事务外限频并控制并发。秘密内存由 sodium_malloc/free 管理，不能记录密码或令牌。
后续对称密钥由可信组合根传入，32字节、独立用途、不可变受限keyring，与数据库分离；
不得从请求、JSON令牌头或远端key URL获取。收据当前到期由应用使用数据库时间检查，
适配器不得以本机wallclock替代数据库时间。尚未接入生产组合根或宣称服务ready。

PeerCertificatePort信任根由部署配置传入公共PEM，最多16根，每根最多64KiB；SAN类型
由可信部署配置固定为URI（默认）或DNS，登记字符串精确匹配，不使用通配hostname授权。
证书必须包含clientAuth EKU并符合适配器固定算法白名单；本适配器不签发CA或提供在线
证书撤销查询。调用者仍须读取当前登记/凭据状态，并以真实TLS认证证据构造叶在前DER链。

### P04 Control HTTPS configuration and endpoint lifetime

Control alone accepts optional `control_api` with exactly `listen` and `identity_endpoint` strings, plus optional `runtime_endpoint`, `scheduler_endpoint`, `model_endpoint`
and `host_endpoint` owner mTLS endpoints for P12c owner-backed REST/health aggregation (absent endpoints
keep the corresponding routes stable-failing rather than serving fabricated data). Listen is IPv4:port (port zero is allowed for isolated
tests), independent of the loopback probe. Identity is a single ASCII DNS/IPv4
host:port, port 1–65535; URLs, DSNs, scripts and TLS verification overrides are
rejected. Unknown/duplicate keys and wrong types exit 2. Validated `tls` material
is reused for browser HTTPS and outbound Identity mTLS. Without this object no
business listener is started and readiness remains false; no local fallback exists.

ServiceComposition may exclusively own an outer ServiceEndpoint. Its start runs
after dependency validation and RPC registration, before startup announcement;
failure exits 1. Its local health is checked once per second. Destruction cancels
and joins within a bound, before application destruction and after RPC shutdown.
Identity request failures do not require terminating Control.

Runtime alone additionally accepts an optional top-level `identity_endpoint` string
(ASCII DNS/IPv4 host:port, port 1–65535; URLs/DSNs/scripts and TLS verification
overrides are rejected). It is the Identity owner gRPC endpoint used by Runtime's
online `CheckAuthorization` client and delegation-metadata verification; the
validated Runtime `tls` material is reused as the outbound mTLS client identity.
Without it Runtime keeps a fail-closed authorization placeholder and reports
readiness false; no local authorization fallback exists.

The target HTTPS endpoint uses asynchronous Boost.Asio/Beast TLS/read/write with
five-second deadlines (TLS close one second), 64 concurrent connections, an 8KiB
header limit, 64 parsed fields and a 1MiB request body limit. Oversized requests
return INVALID_ARGUMENT/400 when a TLS response is possible. Four fixed upstream
workers share each operation's two-second deadline including queue time; shutdown
cancels contexts and closes sockets before joining workers. No detached threads,
local auth fallback, remote TLS bypass or browser client-certificate requirement.
The business port is reported as control_https_port when bound. Its material must
support server TLS plus the outbound Control client identity; Identity still requires
that peer to be registered independently of CA trust. All test credentials/configs
are temporary; no bootstrap secret is put in argv or committed.

委托签名配置使用独立Ed25519密钥，私钥通过OwnedSecret交付并复制到安全内存，不能
出现在数据库、日志或公开配置投影。签名clock必须由部署证据支持可信及已知skew<=2秒，
验签sample同样受限；本适配器不实现NTP信任探测。iat/nbf/exp为整秒，调用应用须明确
构造最多56秒的证明期限，不能将任意数据库纳秒时间静默扩展有效期。真实peer验证时间
必须足够新鲜且登记证书仍有效；高风险操作仍在线向Identity检查当前来源与授权。

### P04 Identity 安全初始化（实施中）

Identity 最新新空库版本为 5，其余三类私有库为 4（含下文完整会话结果表），启动逐库严格核对版本及完整对象描述。
此变更仅面向显式创建的空库；不存在自动升级、重置或清空旧数据的路径。
初始化在同一 DDL 事务写入唯一 Required/v1 管理员引导标记；缺失该标记必须报告损坏，
不能视为允许再次引导。后续可信离线引导通过同一 Identity UoW 条件消费标记，连同
主体、凭据、精确授权、水位和审计一起提交；目前 schema 与端口不代表命令已接线。
安全成功收据与消费 refresh 索引按最多 24 小时恢复窗口清理；不能删除尚有效的收据腾
容量。备份包含验证材料与加密收据，须按现有私有库保护；原始秘密不应写入库或日志。

Malformed HTTP framing (including conflicting Content-Length headers) returns the
same bounded INVALID_ARGUMENT/400 envelope when TLS permits a response; ordinary
network EOF and expired handshake/read deadlines close the connection.

The retained raw HTTP read buffer is also capped at 1MiB + 8KiB. This bounds an
unfinished chunk-extension/framing line even before decoded body bytes are counted;
exceeding the cap returns the same INVALID_ARGUMENT/400 response. The bound business
port is read before the I/O thread starts so acceptor state has one thread owner.

### P04 完整会话收据的空库版本（实施中）

完整无秘密 SessionMetadata 普通结果表进入四库共享schema后，最新新空库版本为
Identity5、Runtime/Scheduler/Model4；之前Identity4和另三库3不兼容，不自动升级、补表、
清空或重置。必须按已有显式初始化流程为本轮部署创建新专属空库，保留原库与备份。
新表随原事务/版本标记和授权一起创建，daemon仍不执行DDL，启动严格校验完整描述。

Identity安全依赖组合使用open_identity_security，探针通过该工厂自身begin/current_time/
rollback采样，不能创建第二个持锁连接。返回工厂只表示存储依赖已验证；业务仍需当前
认证、crypto与应用用例接线。UoW结束后才释放本地许可；Unknown或连接丢失停用旧实例。

组合根可用 `identity_crypto/key_files.hpp` 显式装载密钥；对称文件为64小写hex加可选LF，
秘密文件须regular、当前effective UID所有、禁止group/other权限（通常0600）。路径及父
目录由可信部署配置提供并受控；O_NOFOLLOW约束最后路径项，不宣称防止不可信父目录替换。
公钥文件也必须regular且有界、非symlink。装载器只读，不创建、覆盖、轮换或修正权限，
失败时调用者拒绝启动且仅记录稳定码，不记录路径、PEM、hex内容或系统异常正文。

公钥装载文件需当前effective UID或root所有，禁止group/other写位，可用0644；0664和0666
视为不可信配置拒绝。秘密权限规则仍更严格。不要把公开可读误解为可被任意用户修改。

离线bootstrap组合根可用load_bootstrap_password_file装载受控regular秘密文件（通常0600、
当前effective UID、无group/other权限、可信父目录）。允许一个末尾LF；实际密码1..1024
字节且不含CR/LF/NUL。此入口只读并返回安全内存，不自行创建管理员、生成/覆盖密码，
不把密码放进argv、日志或公开配置；业务消费仅由下述受控离线组合根执行。

### P04 Identity RPC 适配器尚未启用生产监听

make_identity_rpc借用纯IdentitySecurityApplication和组合根注入的受审PeerAuthenticator。必须同时有真实mTLS证据及当前登记/凭据/SAN核验，不能用CA握手成功或fixture替代。未接纯应用/PG/认证回调时保持not ready，不激活旧reference Identity。适配器不读本机密钥或改配置/主入口；测试只在临时目录生成证书/私钥，密码/token和异常正文不入日志。认证回调须有界并遵守取消，服务寿命覆盖application和回调捕获端口直至RPC Wait结束。实际生产PG/当前凭据/撤销故障闭环由后续组合根验收；本轮只证明RPC边界。

### P04 当前登记 PeerAuthenticator（Core持有权威编排）

make_identity_peer_authenticator(IdentityAdminApplication&, SecurityClockPort&)只借用Core Admin应用和受信时钟。wrapper从真实AuthContext提取指纹/DER证据，取2秒本地与真实gRPC deadline较早者，调用Admin.authenticate_peer({leaf_fingerprint, deadline}, der_chain)。当前DB UTC、登记/凭据/主体、释放UoW后成熟crypto验证及验证后fresh recheck全部由Core负责；外层不直接读取repos、不复制业务规则。

wrapper仅核对返回指纹、正verified_at、受信clock/已知偏差及与verified_at的2秒窗口，再检查取消/期限。不构造数据库/信任根/bootstrap或ready。借用应用与clock须活到服务停止/Wait，Core应用与安全应用由组合根绑定同一完整Identity工厂。wire/API不变。

### P04 离线管理员与 Control 登记命令

`kiarabot-identity-bootstrap` 只在可信部署主机离线运行，不提供 HTTP/gRPC 初始化入口。
先显式初始化 Identity 空库（当前 schema11），停止同库 Identity daemon，再执行：

```bash
kiarabot-identity-bootstrap --config identity.json --enrollment enrollment.json --password-file administrator-password
kiarabot-identity-bootstrap --config identity.json --status
```

Identity 配置沿用服务 TLS 和私有数据库配置，CLI 使用 runtime_role，通过公开安全 UoW
提交数据，不执行 DDL。活动 daemon 持有排他 writer 锁时命令失败，不自动停止进程、重连
绕过锁或清空旧库。`--help` 不读配置、不连接数据库；参数错误退出2，运行/引导拒绝退出1，
确认提交或状态查询退出0。输出只有稳定操作/错误/受理状态，不打印密码、证书、路径或连接异常。

密码从受控常规文件安全加载（当前用户、0600、无 symlink/FIFO），内容1..1024字节，可有
一个末尾LF，不含其他CR/LF/NUL。使用成熟 Argon2id 派生后释放原密码安全内存；引导产生
一年有限期的密码凭据。Control 公共 PEM 链经受信 CA、clientAuth、实际 SAN、指纹、
时间和服务绑定检查后才写入；CLI不生成CA/证书、不接收CA私钥或打印可复用凭据。

登记 JSON 是部署输入，必须为当前用户或root拥有、无group/other写权限的常规文件，
不接受重复/额外键。Control链文件使用相同公共文件检查。示例中 UUID 必须由维护者为
本次部署明确分配，以下仅示例；授权不含隐式全局权限：

```json
{
  "schema_version": 1,
  "administrator_id": "10000000-0000-4000-8000-000000000001",
  "password_credential_id": "10000000-0000-4000-8000-000000000002",
  "username": "administrator",
  "control_id": "10000000-0000-4000-8000-000000000003",
  "control_credential_id": "10000000-0000-4000-8000-000000000004",
  "control_certificate_chain_file": "/run/kiarabot/control-chain.pem",
  "control_san": "spiffe://kiarabot/control",
  "administrator_assignments": [{
    "assignment_id": "10000000-0000-4000-8000-000000000005",
    "role": "admin",
    "scope": [{"kind": "RESOURCE_PRINCIPAL", "id": "10000000-0000-4000-8000-000000000001"}]
  }],
  "control_assignments": [{
    "assignment_id": "10000000-0000-4000-8000-000000000006",
    "permissions": ["identity.delegation.issue"],
    "scope": [{"kind": "RESOURCE_PRINCIPAL", "id": "10000000-0000-4000-8000-000000000001"}]
  }]
}
```

username 使用现有小写 ASCII 规范。每条assignment只能有role或permissions其中一种，scope
是显式资源数组；permissions与scope展开完整product，再交Core精确规则验证。Control
不继承人类角色，管理员至少有一条明确scoped admin角色。该示例仅授权自身Principal，
其他目录/账号/实例的授权需明确登记，不能把示例当作所有资源通配符。

Required/v1 → Consumed/v2 和所有主体、凭据、授权、水位、审计在同一本地事务提交。
重复引导返回CONFLICT且不会生成替代管理员。提交结果Unknown时先用同配置`--status`
查询持久marker（required/consumed与version），不要自动生成新身份重试；缺marker为损坏。
命令未实现秘密恢复/密码找回，后续凭据轮换仍走其既定所有者契约。

### P05 平台服务 peer 离线注册（Runtime/Scheduler/Model，实施）

Runtime/Scheduler/Model 作为 Identity 的 mTLS 消费者，必须先有平台 Principal、clientAuth
证书凭据与显式 assignment，才能通过真实授权链。该供给只走可信离线 bootstrap，不新增匿名
注册或 RPC：在登记 JSON 顶层追加可选 `platform_peers` 数组（最多16项），与管理员/Control
在**同一 once-only 事务**、同一 UoW 提交，含主体、证书凭据、授权水位与审计：

```json
{
  "platform_peers": [{
    "service": "runtime",
    "principal_id": "20000000-0000-4000-8000-000000000001",
    "credential_id": "20000000-0000-4000-8000-000000000002",
    "certificate_chain_file": "/run/kiarabot/runtime-chain.pem",
    "san": "spiffe://kiarabot/runtime",
    "assignments": [{
      "assignment_id": "20000000-0000-4000-8000-000000000003",
      "permissions": ["identity.authorization.check"],
      "scope": [{"kind": "RESOURCE_PRINCIPAL", "id": "20000000-0000-4000-8000-000000000001"}]
    }]
  }]
}
```

`service` 仅接受 `runtime|scheduler|model`；`principal_id`/`credential_id`/`assignment_id`
必须与既有主体、Control 及其凭据/assignment 互不重复。证书链须经受信 CA、clientAuth、实际
SAN/指纹/时间及平台服务绑定校验（复用 Control 同一 `peer_certificate_crypto`）；证书主体
必须等于 principal，平台服务必须等于 enrollment service，且仅在有效期内。每条 assignment
必须是**显式 permissions**（不得 role，角色仅 human），subject 为该 peer，且在其 scope 上
非空、去重、合法 exact 资源；`identity.authorization.check` 是 Runtime 消费
CheckAuthorization 的必要能力（scope=自身 Principal）。缺失/非法/重复/证书不匹配返回稳定
INVALID_ARGUMENT（exit1），重复引导仍 CONFLICT；不打印证书、密钥、密码或路径正文。
该 JSON 值仍受“当前用户或root拥有、无 group/other 写权限的常规文件、重复/额外键拒绝”约束。

Runtime 配置继续用服务 TLS 材料作为出站 mTLS 身份：`identity_endpoint` 必填时可启动业务
RPC，并在 Identity 授权链可用后 ready=true；缺省 `identity_endpoint` 时使用 fail-closed
authorizer、ready=false 且拒绝业务命令（不是降级放行）。Identity 不可用/证书或时钟异常时
Runtime readiness 必须变 false、拒绝新命令且不伪造成功；只有恢复后重新观测到当前授权才可
再次 ready。Runtime 方法通过 `scope_kinds`（见 doc/01）声明 exact 资源族；未标注方法保持
fail closed，CreateAccount/List*/GetResourceState/GetOperation 仍待补应用 scope 后再标注。


### Identity security configuration declaration (P04)

Identity alone accepts optional `identity_security`, with exactly these keys:

```json
{
  "token_key": {"id": "token-v1", "file": "/run/kiarabot/identity/token.hex"},
  "request_key": {"id": "request-v1", "file": "/run/kiarabot/identity/request.hex"},
  "receipt_key": {"id": "receipt-v1", "file": "/run/kiarabot/identity/receipt.hex"},
  "delegation_key": {
    "id": "delegation-v1",
    "public_key_file": "/run/kiarabot/identity/delegation-public.pem",
    "private_key_file": "/run/kiarabot/identity/delegation-private.pem"
  },
  "issuer": "kiarabot-identity",
  "clock": {"trusted": true, "maximum_known_skew_ms": 2000},
  "verification": {
    "catalog_id": "11111111-1111-4111-8111-111111111111",
    "version": 1,
    "key_version": 1,
    "valid_until_epoch_seconds": 4102444800
  }
}
```

This object requires the existing database and TLS configuration. Every object uses
exact keys; duplicate/unknown keys, wrong types, empty/NUL strings and missing fields
exit2. Key IDs and issuer are1–1024 bytes, file references1–4096 bytes. Clock trusted
must explicitly be the JSON boolean true; skew is an integer0–2000 inclusive, never a
boolean/float or inferred default. This is a trusted deployment clock declaration,
not an NTP measurement. Production composition must monitor wall/steady drift and
fail closed when it exceeds the declared bound; it may not turn an untrusted sample
into a successful authorization.

The parser holds file references only. The production composition must load all keys
before listeners through the mature secure key-file loaders, including rejection of
symlinks/special files, insecure permissions, invalid hex/PEM and mismatched Ed25519
keys. It must not read key bytes through ordinary JSON/string configuration storage.
The configured Identity entrypoint securely loads these files, constructs mature
crypto, validates its clock and derives a dummy password PHC from 32 CSPRNG bytes
before opening the complete PostgreSQL security factory or listeners. Failures stop
startup; no configured failure falls back to the placeholder. All session, Admin and
issuer applications share that factory. Bootstrap remains an explicit offline action.
Absent security retains the existing not-ready placeholder path.

The native security clock samples UTC between steady reads and compares drift with
its original baseline, accounting for measurement interval and clock resolution.
A measurement wider than 1ms is temporarily unavailable; drift beyond the declared
bound, steady regression or invalid range latches unavailable until process restart.
No rolling recalibration or NTP check is performed. Faults reject authentication and
keep readiness false. Fixed finite application limits are 4096 sessions per subject,
10000 secret receipts, 1024 snapshots and 4096 total snapshot records; password lifetime
is 365 days. These do not remove lower global/domain limits. Readiness is the real gate described in P04-A (business implementation + DB writer + loaded key material + consumed bootstrap + trusted clock); it is no longer a fixed false placeholder.

### P04 IssueDelegation RPC（第七方法，可选依赖）

make_identity_rpc(IdentitySecurityApplication&, PeerAuthenticator, IdentityDelegationApplication* issuer=nullptr, InvocationDeadline={})仍注册一个IdentityService，借用issuer到停止/Wait；两参旧调用可编译，缺issuer时Issue返回RpcFailure/CAPABILITY_NOT_SUPPORTED。Exchange与其他未接方法仍明确未实现，不降级到reference。Peer factory当前签名为make_identity_peer_authenticator(IdentityAdminApplication&, SecurityClockPort&)。

Issue只接受当前Control peer，subject_proof按原始AccessSession token借用，不剥Bearer或接收现成委托替代。audience显式映射封闭六服务；scope/permissions非空、去重、已知枚举与合法exact资源，client_certificate_sha256必须恰好32字节并等于真实transport peer。wire声明的permission×scope product只转换一次为exact tuples，由Core issuer逐tuple复核当前来源与权限，外层不授予或拼接其他assignment。

按父定稿1b962d3，Issue/Exchange requires_idempotency=false：签发不消费配额或一次性材料、无业务意图/普通七天秘密收据，重复请求重新授权并可产生新proof ID。Login/Refresh/Revoke/Rotate仍要求原幂等键。返回只复制应用实际签出的proof及原audience/expiry/一跳信息，核对claim与请求tuple、真实证书及来源上下文一致，不补签名/期限/来源；crypto和权威查询均由Core持有。真实JWT生产签名/PG/HTTPS组合根闭环由父继续验收，本RPC fixture不据此标target implemented。

### Identity own TLS expiration lifecycle

Configuration records the actual OpenSSL-verified selected chain's latest not-before
and earliest not-after, including intermediates and trust anchor. All timestamps must
fit the supported finite UTC range; validity is inclusive at not-before and exclusive
at not-after. It stores no guessed TTL or new clock-derived expiration. Configured
Identity's trusted clock checks these bounds for each security sample and before/after
real peer authentication, so an existing channel cannot admit new operations after
expiration. The failure is UPSTREAM_UNAVAILABLE, not a permission denial. The
Credentials readiness snapshot becomes unavailable; the next bounded maintenance
check stops the process with exit1 and closes RPC connections. Restart requires valid
material and the usual startup chain validation. There is no automatic reload or
extension. The short-certificate regression uses real UTC and existing Control gRPC
transport, without changing the system clock.

### Identity 自身 TLS 调用期限

当前factory为make_identity_rpc(SecurityApp&, PeerAuthenticator, Issuer*=nullptr, InvocationDeadline={})，其中InvocationDeadline是function<Outcome<TimePoint>()>，闭包由服务持有。生产composition必须注入实际选定自身TLS链有效期与当前可信clock/lifecycle gate；空回调仅为fixture/源兼容，不得作为生产默认安全策略，也不得把自身到期时间写入远端Control的CertificateBinding。

run保留body/gRPC/default2秒/max5秒及取消检查，可信回调失败沿原Failure进入完整RpcFailure，异常在调用应用前转Internal/NotAccepted。返回非正时间失败关闭为UpstreamUnavailable；过期期限为Timeout/NotAccepted。有效期限取已有预算与自身limit最早者，回调之后与认证之后再次检查取消/到期；传入每个Core用例的context.deadline包含此上限，由Core final_check阻止自身TLS到期后的提交。外层不授权、不查仓储、不实现TLS链/clock判断或伪造有效期。较晚limit不能延长请求期限。

### Identity verification deployment metadata

identity_security.verification is required whenever identity_security is present.
Its exact four keys are catalog_id (offline canonical lowercase UUIDv4 Catalog
anchor), version and key_version (positive uint64 integers), and
valid_until_epoch_seconds (positive integer1–9223372036 inclusive, representable
finite UTC seconds). Booleans, floating numbers, zero versions, implicit defaults,
unknown/duplicate/missing fields and noncanonical identifiers exit2. A syntactically
valid past expiry fails composition before PG/listeners. Absent identity_security
still selects the old not-ready path; the presence of that object never selects
legacy unlimited keys. This is trusted deployment metadata, never token-selected
URLs, public material fetched from callers, or an automatic rotation policy.

The secure loader preserves the explicit key version/expiry. The same complete
DelegationConfig constructs signing/verification and the public verification port;
composition retains both with their clock and validates a current snapshot before
PG. No second keyset/configuration is built. This enables material ownership only;
GetVerificationKeys authorization and its RPC require their own reviewed capability.

### Production Identity invocation limit injection

The configured production factory now always supplies InvocationDeadline. Its owned
clock first validates the current trusted sample and own-chain lifecycle, then returns
exactly the retained selected-chain not_after. Missing own metadata, untrusted clock
or expired credentials fail closed. RPC takes the minimum with request/transport
budgets and passes that deadline into Core final_check, including work that starts
before own TLS expiry and finishes later. The remote Control certificate metadata
is never replaced. Empty callback compatibility remains confined to fixtures.

### IssueDelegation platform capability and represented permissions

An authenticated Control service must currently hold an explicit
identity.delegation.issue binding at its own stable Principal resource
(ResourceKind::Principal, id=the actual registered platform subject). Certificate
validity/registration and default roles do not grant this capability; a grant at a
represented human Principal, Catalog or other service Principal cannot substitute.
This service capability is checked independently of every represented subject's
exact requested business permission/resource. It therefore supports dynamic Session
resources without turning the service capability into blanket business authority.
Offline enrollment must explicitly provide the own-Principal grant; no CLI or daemon
automatically creates it.

Issue reads the current Control registration, credential, Principal, authorization
watermark and exact grants before signing and again after signing. Source access
and business grants are also revalidated. Each short authority transaction uses the
existing current-time/final-check gate with at most one second authorization window
and the request/certificate/credential deadlines. Crypto/random/session-application
calls hold no outer UoW. Missing/wrong capability returns PermissionDenied; revoked,
expired or mismatched current peer returns Unauthenticated; changed authorization
watermark returns Conflict/ReadCurrent; dependency and deadline failures propagate.
A signed intermediate proof is discarded on any final check failure and never returned.


### P04 CURRENT bootstrap verifier 定位

IdentityCredentialRepository::by_bootstrap 只接受 WorkloadBootstrap purpose、可信 key 引用和
固定 32 字节 tag；不接受原始 proof 或请求主体。只查询 credential_heads 当前定位，不用
LIMIT 1 掩盖歧义。无匹配返回成功空 optional，非法输入为 InvalidArgument，结构损坏为
Internal，数据库连接失败为 UpstreamUnavailable；失败沿用同一 UoW 的回滚语义。
返回记录仍可为 Revoked 或已过期，Core 必须检查主体、状态、期限、binding 和 audience。

Identity 空库 schema6 的 head 显式保存 material；CHECK 要求 bootstrap 必有 key/tag、
certificate 必有 leaf_fingerprint，其他材料必须清空对应定位。CURRENT key/tag 唯一，
复合延迟外键绑定当前版本、材料种类及实际 verifier；save 在同一事务中维护全部定位。
历史版本可保留相同 tag（例如撤销），轮换或更换材料后旧 tag 不再定位 CURRENT。
仅显式初始化空库；旧 schema 拒绝打开，不提供自动迁移、删除或重置。


### P04 会话的原始 Control 持久绑定

Identity 空库 schema7 在 session_security 保存非空 control_subject/control_credential。
复合外键指向 credential_heads(id,subject)，避免把另一主体凭据冒作原始 Control；不保存
旧证书版本或叶指纹作为后续认证依据。只有同事务已登记的 PlatformService/Control 证书
凭据可用于新建。缺值、空值、NUL 或错误身份形状为 InvalidArgument，整个 UoW 回滚。
已有会话更新必须与原 binding 完全一致，否则 Conflict；refresh/revoke 均保留稳定 ID。
读取缺失/损坏字段为 Internal，更新不能为旧 missing binding 隐式补值。认证、当前状态、
证书期限与主体 gate 由 Core 检查；存储成功不代表授权。证书在同稳定 ID 下轮换不改变绑定。
旧库仍拒绝打开，不自动升级/填默认 Control，不启用尚未接线的 Revoke/Exchange 路径。

### GetVerificationKeys RPC（借用Core公共验证材料用例）

factory保留原issuer与InvocationDeadline位置，末尾新增IdentityRpcOptions={.verification_keys=&app}第五参；app借用至server停止/Wait，缺省typed Unsupported。当前mTLS认证及自身TLS InvocationDeadline在查询前照常执行。GetKeys不读/验证context.delegation或JWT，不对unknown kid先做JWT预检，避免获取验证材料的自举循环；Core151f31e负责当前平台身份、显式IdentityKeysRead@可信Catalog与授权后future-version拒绝。

known_key_version显式0无效；正值原样交Core，none/same/old均返回完整current set，不做空delta或成功cache。只映射Core的public-only DTO：key_id、raw32 Ed25519公钥、正uint64键/集合版本与精确纳秒有限valid_until；空集合/零版本/不合法Catalog/零checked_at/过期或无限期限等畸形成功结果Internal。实际依赖/材料过期失败保持完整RpcFailure。外层不访问private/config路径，不加载材料、不重建keyring、不使用token选择远程key源。当前Core契约仅一个active key，生产PG及成熟材料端口闭环由组合根验收。

### Production public-key bootstrap enrollment

The production GetVerificationKeys path uses the same identity_security.verification
Catalog UUID and single crypto bundle as signing. An operator must explicitly include
identity.keys.read at that exact RESOURCE_CATALOG UUID in the registered platform
service's offline assignments if it should fetch verification material. A trusted CA,
registered certificate, default role, delegation-issue capability or grant at another
resource does not imply this permission. The CLI does not automatically create it.
No JWT is required to bootstrap the verification keys; no unauthenticated HTTP route
or token-selected key URL is introduced.

The real owned-PG daemon regression accepts --keys-client <compiled-client> to enroll
this explicit test grant and verify raw32/key-ID/set-version/key-version/absolute-
expiry over mTLS. --keys-no-grant omits it and requires registered-caller denial,
including the future-version request; a CA-valid but unregistered leaf is also denied.
Default bootstrap and daemon tests do not automatically gain Catalog privileges.


### P04 CURRENT 平台 actor 定位

by_platform(PlatformPeer) 在同一 UoW 用 subject/current-head 索引与当前 certificate 关系
按 platform service 精确定位。仅当前 head 参与，历史证书不参与；零条为空，多个当前匹配
为 Internal，不任选，不将全部主体载入内存。未知 service、空/NUL subject 为
InvalidArgument；缺失当前证书材料或 head/指纹错配为 Internal，依赖故障沿原 Failure
映射传播并使 UoW 回滚。返回可为 Revoked/过期记录，Core 仍验证 owner、主体状态、当前
证书/SAN/service 和有限期限；此查询不证明 TLS possession，也不代替来源 actor 授权。
Identity 空库 schema8 增加 credential_heads(subject,id,version) 非唯一索引；允许识别
歧义而不以索引任意选择。其他三库未变，旧库拒绝，不自动迁移或重置。

### Current original Control of browser sessions

Every browser session retains its original Control subject and credential ID. Access,
refresh and secret replay require the actual current Control to match both stable IDs;
certificate rotation under that same ID is allowed after current TLS/registration
verification. Another Control cannot take over or rebind old tokens. Missing binding
fails closed. Current head, active PlatformService/Control, own Principal binding,
registered SAN, credential/certificate validity and dependency failures are checked
again at session authority reads; no VerifiedPeer is fabricated from stored records.

For delegated source_session, the persisted original Control subject must match
claims.actor_chain.front(). That original Control is checked from typed current
registration independently of the actual direct Runtime's own authenticated TLS.
Relevant final deadlines include the original Control current credential/certificate
expiry and the existing one-second authorization checkpoint. Crypto runs outside UoW;
fresh reads after verification/replay reject intervening invalidation or dependency
failure. Source binding does not pin a certificate version or overwrite direct peer data.


### P04 ExchangeDelegation RPC 适配

`IdentityRpcOptions{.exchange=&app}` 可选借用 Core `IdentityExchangeApplication`，生命周期至
server 停止/Wait；原 issuer、InvocationDeadline 及 verification_keys 参数位置不变。
未注入时返回 typed CapabilityNotSupported。只接受当前认证的 Runtime 平台 mTLS，
子受众仅 model/scheduler；body 的32字节 client_certificate_sha256 必须等于实际 Runtime
叶指纹。parent_proof 原样借用，不剥离 Bearer，不在外层解析 JWT，不把 Runtime 指纹
当成父 proof 的上一跳 Control 指纹。父签验、两平台当前显式能力、来源会话/凭据/业务
权限及签后重查均由 Core 负责。

封闭 permission/resource 去重并在解码、笛卡尔积展开前限制最多128个 exact tuples；
父 proof 非空、可见 ASCII 且最多32768字节。无普通写入幂等键要求、无收据或库写入。
继承当前 mTLS、请求/transport/自身 TLS InvocationDeadline 的最小期限与完整 RpcFailure
映射。成功只投影 Core 返回的原始 proof、audience、expiry、hop2，校验当前 Runtime
指纹、actor 链和请求精确集合；异常成功结果失败关闭，不发送部分 proof。

真实 mTLS fixture 覆盖该适配与实际 Core 编排；测试的仓储、随机和签验仍为明确替身，
不代表生产 PostgreSQL、JWT 材料端口或组合根已接线可用。


### P04 Assignment 当前墓碑

Identity 空库 schema9 的 exact_assignments 保存 Active/Revoked 与有限正 revoked_at。
current(id) 包括永久墓碑的 owner、最终版本和旧精确 tuple；不存在为空，损坏为 Internal，
SQL依赖故障为 Failure。for_subject 与 inherited grants 有效读取均过滤 Revoked，不能用
墓碑授予权限。原 save 等同 Active save_state；仅 Active v1 创建、同 owner CAS v+1 更新
或撤销。撤销不改旧 scope，时间不得晚于实际 DB UTC；已撤销 ID 再写、复用或换 owner
均 Conflict，不能以物理删除/空 tuple 代替撤销。save_state 是 base metadata 同事务唯一
同步者，重复 base 写入不作为假成功；基础 store_live 也拒绝 revoked ID。失败整 UoW 回滚，
Unknown commit 必须重读原状态/收据，不能假定未提交。无TTL删除墓碑，旧库拒绝、不自动迁移。


### P04 普通收据的完整原始公共结果

共享 ReplayResult 保留既有0..12，追加 RegisteredPrincipalSnapshot13 和 ExactAssignment14。
13在独立 typed 关系保存原 Principal 和 initial_credential 的全部公共字段，包含可空 binding、
expiry 及原 created_at/checked_at 纳秒；14保存原 owner/version、role XOR explicit permission
精确 tuple，按 position 无损保留顺序，并用声明计数检测缺失子行。没有 live authority FK，
不通过当前 principal/credential/grant 查询或 base metadata 逆拼旧成功，不保存 token、proof、
verifier 或 SecretReplayPayload。重复同指纹仍返回原完整快照及原 retention；Unknown commit
重连用原键恢复原结果，即使当前凭据已轮换或授予已撤销。新输入坏形状/NUL为 InvalidArgument，
缺快照/子行、次序或 typed shape 损坏为 Internal，依赖故障沿同 UoW Failure/回滚语义传播。

共享 typed 表及 kind 范围变化后，Identity 新空库 schema11（含 Pending 凭据受众关系与
一 binding 一 Principal 唯一索引），Runtime/Scheduler/Model 均为 schema5；四库 descriptor
全部更新。旧库拒绝打开，不自动迁移或清空。Register/Rotate/Revoke/GetStatus 五个 RPC 的
应用/PG/传输已接线（实现中），仍待完整门禁与真实 PG/TLS 验收。

## P07 Action 配额、保留与运维口径（契约冻结，未实现）

- **配额/预算**：出站滑动窗口占用持久化（单账号 10/分钟、单群 3/分钟、私聊 1/10 秒为默认起点，部署可配），跨进程重启不重置；危险 Action 的 UTC 日预算按 `(kind, account, UTC day)` 持久化。超限返回可重试 `RATE_LIMITED`，取消/明确失败不自动退额（保守取舍）。
- **ActionPolicy**：Mute/Kick 必须显式开关 + 群白名单 + 正预算，缺省 fail-closed（缺失 policy 即拒绝）；预算 0 表示禁用；`UpdateActionPolicy` 为 upsert（缺 `expected_version`=创建）。`allowed_group_ids` 仅群管理 kind 生效。
- **deadline 与保留**：deadline 未来 UTC 且 ≤300 秒；终态从最后状态更新起保留 168 小时；执行证据同样保留 168 小时；清理随维护循环，失败告警，不清除未决记录。
- **查询/权限**：Control 查询必须显式 `RESOURCE_ACCOUNT` scope + `action.inspect`，admin 不隐式越权；插件仍走 `PluginBridge` owner-scoped 查询；`NOT_FOUND` 不代表平台未执行。
- **日志/审计**：审计记录 action/submit|dispatch|complete|cancel|expire|evidence 与目标账号/会话；不记录消息正文、平台秘密或幂等键明文；本阶段未实现，不得以契约冻结宣称可达。

## P08 Host 部署与受管凭据（契约冻结，未实现）

- **监听与身份**：Host 连接 Runtime 的内部 mTLS `grpc_listen`（不是外部 `bridge_listen`）；Host 使用受控部署签发的客户端证书，Runtime 部署配置 `hosts[]` 预注册 `host_id`、证书指纹/SAN 精确匹配、允许的包摘要/profile 与凭据文件路径；未预注册或 `host.host_id` 与证书绑定不一致返回 `PERMISSION_DENIED`，不泄漏 allowlist 细节。
- **Host 侧配置**：Host 自身登记 `host_id`、证书、包目录/摘要、launch profiles（interpreter/fixed_args/allowed_env_names/credential_env）与凭据文件；启动参数/环境变量走冻结 `name=value` 白名单，Host 校验长度/字符集；不得读取未登记 profile 或任意环境。
- **受管凭据**：部署为每个受管实例生成 opaque bootstrap，`0600` 文件挂载 Host；Runtime 部署配置只登记 `(instance_id, version, hash, expires_at?)`（载入 P06 `bridge_credentials`），Host spawn 时按 `credential_env` 注入环境变量，SDK 走 P06 握手；明文不进入 Runtime 存储/outbox/日志/观察/审计，Host 不落日志。撤销/轮换属 P14 运维入口。
- **心跳/离线告警**：Host 每 5s 心跳；15s 无心跳只告警在线状态并保持 `process_outcome=UNKNOWN`，不得显示“已停止”或触发自动重启；运维需按跨主机 gate 人工处置（旧执行未确认时拒绝迁移）。
- **迁移**：切换 Host/账号前必须确认旧执行已终止或留下操作员显式处置记录；不得同时运行 reference 内嵌 launcher 与 P08 Host 管理同一实例（双 spawn 风险）。

## P09/P10 Storage/Task 运维口径（契约冻结，未实现）

- **Storage 限额**：每私有/共享 namespace 1024 键/1MiB、单值 64KiB，安装聚合 8192 键/8MiB；写收据每实例 10000/安装 80000、保留 7 天，容量满拒绝（RATE_LIMITED）且不预删有效收据。TTL 绝对值必须未来且 ≤30 天，过期读取视同不存在；运维清理只清过期值，不清未决收据。
- **共享 namespace**：每安装至多一个，必须先由 Control `CreateSharedNamespace`（namespace.manage、Idempotency-Key）创建再 `SetSharedMembers`；插件首次写前未创建返回 NOT_FOUND，不静默创建。成员/禁用撤销与并发写按同 gate 线性化，敏感共享读每次在线检查 Identity。
- **任务容量/时间**：每实例 1024/安装 8192 未终态任务；run_at 必须未来且 ≤ 默认 366 天 horizon（`KIARABOT_TASK_MAX_HORIZON_DAYS` 可配）；local_time 依赖 tzdata，缺失时 readiness 不就绪并返回 UPSTREAM_UNAVAILABLE，不得静默按 UTC。
- **交接过期/人工恢复**：trigger outbox valid_until 默认 7 天、attempts 上限 100；超窗 occurrence EXPIRED、task FAILED(TIMEOUT) 且不自动重建；运维通过 GetTask/GetOccurrence 观察后用新 task 显式恢复，禁止自动重放。COMPLETED 仅表示事件已持久入队，不是插件执行成功。
- **审计/幂等**：storage 审计以 namespace/key 为 resource，任务审计以 task 为 resource，复用共享 audit_entries 扩列；幂等 scope=认证主体+方法+有效 namespace/key（或 owner+task_id）；跨实例不串响应。

## P11 Model 密钥/Provider 运维口径（契约冻结，未实现）

- **根密钥**：`KIARABOT_MODEL_ROOT_KEY_FILE`（32 字节 hex、0600、`O_NOFOLLOW` 可信文件）与 `KIARABOT_MODEL_ROOT_KEY_VERSION`（正 uint64）由部署注入；进程启动加载，缺失/长度错/权限错→不 ready。根密钥不入库、不进 argv/日志/环境 dump、不与密文同表；轮换走部署流程，旧版本在无在途引用前保留。
- **Provider secret**：AEAD 解密只在 Model 调用 Provider 前、内存用完擦除；`GetModelConfiguration` 只回 secret_configured/secret_version；解密失败/根密钥版本不匹配→该模型 readiness Credentials Unavailable、Chat 不写记录返回 UPSTREAM_UNAVAILABLE（not accepted），不启动 Provider。
- **依赖/超时**：Model 需 Identity `GetVerificationKeys` 缓存与自身 `identity.authorization.check` 部署授权；Provider 超时 1..30s、per-model max_concurrency 1..64；5xx/TLS/超时/畸形响应按 UNKNOWN 处理，不得当作未计费/未调用。
- **UNKNOWN 处置**：未决 UNKNOWN 30 天内不得清理；运维用 `ResolveModelCall`（expected_version CAS）选择 NOT_EXECUTED/EXECUTED_NOT_BILLED/EXECUTED_BILLED/ABANDONED；处置后终态保留 ≥7 天防重窗口，归档后同 logical_call_id 可重新调用（边界文档明示）。Provider 账单需外部对账；不引入价格/预算/扣费模型。

## 今晚单机 MVP：reference 本地收发入口

`scripts/local/start.py` 使用现成 reference Core、Control HTTP、SQLite、固定
`plugins/local-reply` SDK 子进程和真实 NoneBot。默认自动启动仅 loopback 的 OneBot
协议替身；不启动 PostgreSQL 或目标六服务，不接 QQ 或模型服务。此闭环不代表新架构完成。

已有 reference 构建和锁定 Python 环境是前提：

```bash
uv sync --locked --python 3.11.18 --project adapters/nonebot
adapters/nonebot/.venv/bin/python adapters/nonebot/tools/generate_proto.py proto
adapters/nonebot/.venv/bin/python sdk/python/tools/generate_proto.py
python3 scripts/local/start.py start
python3 scripts/local/start.py status
```

默认从 `build/full-locked/reference/core/kiarabot-core`、`build/full/reference/core/kiarabot-core`、
旧 `build/full/core/kiarabot-core` 等现有位置选择 reference 二进制；credentials 工具位于相应
`control/api/kiarabot-control-credentials`。自定义构建可显式传 `--core PATH --credentials PATH`；
解释器可传 `--python PATH`。不自动构建、不升级依赖。默认账号123、允许回复的会话456；首次
启动可用 `--account 123 --conversation 456` 明确设置。保留认证、限流、幂等与 action allowlist。

`start` 输出一行无秘密 JSON，包括 `control_url`、gRPC端口、demo socket和自有子进程。
默认私有状态目录 `data/local` 已被忽略，目录0700，密码/token/config/log文件0600。
管理员用户名 `admin`，密码见该目录 `admin-password`，不在终端输出。`init` 可先只生成配置。
第二次 `start` 不重置密码、加密密钥、SQLite或outbox；运行中更改参数必须先 `stop`。

发一条仅本机的群消息并等待实际固定 Sender 回复：

```bash
adapters/nonebot/.venv/bin/python scripts/local/onebot_demo.py message \
  --token-file data/local/onebot-token --control-socket "$PWD/data/local/demo.sock" \
  --type group --user-id 789 --group-id 456 --text hello --timeout 10
python3 scripts/local/start.py stop
```

预期回复 `收到：hello`。macOS Unix socket路径须不超过100字节；长工作目录可用短的
私有 `--state-dir /private/tmp/kb-local`（须为空或由本入口拥有，0700）。状态目录拒绝符号链接，
不跨工作树共享。`stop` 保留数据，只向 UID、启动时间、完整命令、目录和运行标识均匹配的
自有 supervisor 发信号；supervisor 持有进程句柄，逐一有界回收孩子。可疑旧PID/孤儿状态拒绝
自动接管，须人工核对私有 `run.json`；不删除数据库或杀占用端口的其他程序。失败非零退出。

可选网页使用 `start --ui`：要求已有锁定 `panel/node_modules` 和 Node，Vite绑定
`127.0.0.1:5173 --strictPort`，以 live API／WS URL 连接实际 Control，仅允许该 Origin。
登录后必须收到 `/ws/control` 的 `SESSION_READY` 才加载页面；不回退 mock。没有 `--ui` 时
只报告 API URL，消息 CLI仍可用。缺依赖或端口占用会失败并回收本次子进程。

真实平台仅在用户显式传入 `--onebot-url ws://HOST:PORT --onebot-token-file PRIVATE_FILE`
时启用；token文件必须属于当前用户、0600，不把 token 写入参数或日志。`--demo-url` 仅供
显式 loopback 测试替身。`--core-only` 是标明无收发的诊断模式，不作为 MVP 收发验收。
本次使用当前重建的 reference 二进制，实际验收覆盖连续群聊456／私聊456回复、登录、WS
首帧、REST、持久重启及停止。先前旧 `build/full/core/kiarabot-core` 遗留产物的私聊超时
未在最新 reference 复核中重现；须使用当前源码构建的产物，不混用旧 checkout 二进制。

## 单机真实QQ：独立协议端驱动

2026-11-01用户已授权本机安装配置NapCat，扫码由用户完成，其他业务功能暂停。
使用`scripts/local/napcat.py`作为独立运维驱动，不把安装、容器或QQ登录放入Core。
这不是已经实现的Panel内置安装向导；可替换驱动与程序内向导的已确认方向见
[真实QQ接入设计](26-real-qq-onboarding.md)。

macOS入口使用专用Colima profile：只将`data/napcat-qq/container`挂入VM，NapCat
容器仅挂config/qq/plugins三个专用目录、loopback发布6099/3001；宿主机仍运行
reference Core/Adapter/Panel。与上文整套Linux host网络Compose是两条不同运行方式，
不更改Core回环绑定。Linux入口要求已安装可用Docker；安装系统Docker不由此脚本接管。

```bash
# macOS缺工具时，--install-tools经Homebrew安装Colima/Docker。
python3 scripts/local/napcat.py setup --install-tools
python3 scripts/local/napcat.py status
# 用户扫码成功并明确测试群/私聊目标后，再替换数字运行：
python3 scripts/local/napcat.py connect --conversation 123456789 --ui
```

setup保存实际image digest、独立WebUI/OneBot随机凭据及配置；重跑复用现有值，
不自动升级或覆盖用户登录配置。WebUI地址`http://127.0.0.1:6099/webui/`，本机
密码文件`data/napcat-qq/webui-password`。setup只证明WebUI就绪，仍可能等待QQ登录。
status分别报告容器、WebUI、经认证的OneBot只读get_login_info与get_status.online，
未登录/未在线/无法验证均非零退出，未知状态不当作已在线。
connect从平台读取实际QQ号，仅在用户提供明确会话后启动独立`data/real-qq`链路；
该步骤本身不主动发送测试消息。运行中的其他本机Panel需先停止或安排其他端口。
如5173被其他进程占用，可传`connect ... --ui --ui-port 5174`；不停止未知占用者。

驱动只接管owner标签匹配的容器，发现非loopback发布或外来容器拒绝；端口占用
不杀他人进程。状态目录0700、秘密/配置0600且拒绝符号链接；同目录操作用flock串行。
镜像、VM、QQ登录态和本机秘密不提交。命令失败仅输出稳定本地诊断，不打印平台
异常正文、token或docker inspect的完整环境。stop保留全部数据，仅停止本驱动容器：

```bash
python3 scripts/local/start.py stop --state-dir data/real-qq
python3 scripts/local/napcat.py stop
```

本机专用Colima profile不再使用时，可在确认其中没有其他运行容器后执行
`colima stop kiarabot`，保留VM磁盘和QQ状态。其他机器应使用其实际专用profile名称，
不能停止不属于本程序的共享VM。先停止业务实例、再停NapCat、最后停专用VM。

当前安装/扫码/真实收发的实际进度见doc26及对应执行报告，不能把本节命令当作全项已通过。

## P08 Host 部署配置与凭据（P08a 实现，第一阶段）

`kiarabot-plugin-host` 使用与其它服务相同的严格 `service` 配置（schema_version/service/
probe_listen/grpc_listen/tls），并额外要求 Host-only 的 `host` 对象（其它服务出现该对象即拒绝，
退出 2）：

```json
{
  "schema_version": 1,
  "service": "plugin-host",
  "probe_listen": "127.0.0.1:18081",
  "grpc_listen": "127.0.0.1:15082",
  "tls": {"certificate_file": "...", "private_key_file": "...", "ca_file": "..."},
  "host": {
    "host_id": "host-a",
    "sqlite_path": "/var/lib/kiarabot-host/host.sqlite",
    "runtime_endpoint": "127.0.0.1:50051",
    "credential_directory": "/etc/kiarabot-host/credentials",
    "packages": [{"plugin_type_id": "event-observer", "root": "/opt/kiarabot/packages/event-observer",
                  "sha256": "<64 lowercase hex>", "launch_profile": "python",
                  "allowed_argument_names": ["instance"]}],
    "profiles": {"python": {"name": "python", "executable": "/opt/kiarabot/.venv/bin/python",
                 "fixed_arguments": ["-m", "kiarabot_sdk"], "allowed_environment_names":
                 ["PYTHONUNBUFFERED", "KIARABOT_PLUGIN_BOOTSTRAP"],
                 "credential_environment": "KIARABOT_PLUGIN_BOOTSTRAP"}},
    "environment": {"PYTHONUNBUFFERED": "1"}
  }
}
```

- 路径必须是绝对路径；TLS 材料同时用于 Host 对外探针/进程 TLS 与到 Runtime 内部 `grpc_listen`
  的 mTLS 客户端身份（部署须给 Host 叶证书 clientAuth）。
- 每个受管实例的 bootstrap 凭据由部署生成，写成 `credential_directory/<instance_id>.bootstrap`，
  权限必须 `0600` 且属主为 Host 运行用户；值只注入 profile 的 `credential_environment`，
  不进入 Runtime 存储/outbox/日志/观察。SDK 从该环境变量读取后仍按 P06 在 PluginBridge 首帧使用。
- 本地 SQLite（`sqlite_path`）是 Host 唯一权威：`command_journal`/`managed_executions`/
  `execution_observations`；已解决收据 7 天、未知/未决 30 天后人工处置。Host 启动时按 marker
  恢复/标记 UNKNOWN，不因重启重复 spawn 同一执行代次。
- Runtime 部署侧 `hosts[]` 预注册 `host_id` 与证书指纹/SAN、包摘要/profile 白名单；Host 配置的
  `host_id` 必须与之一致。HostBridge 只在 Runtime 内部 mTLS 监听，不注册外部 Bridge 监听。

- **P08b-2b Runtime HostBridge 服务端（2026-11-02 传输层实现）**：Runtime 配置新增
  `hosts` 数组，每项恰好 `host_id`（安全字符集）与 `certificate_sha256`（64 位小写 hex），
  对应部署签发的 Host 客户端叶证书；其它 service 出现该键、重复 host/cert、非法字符均退出 2。
  空数组或缺省表示**拒绝所有 Host 注册**（fail closed），不是“不启用校验”：
  ```json
  {
    "service": "runtime", "probe_listen": "127.0.0.1:18080",
    "grpc_listen": "127.0.0.1:15081", "bridge_listen": "127.0.0.1:15082",
    "tls": {"certificate_file": "...", "private_key_file": "...", "ca_file": "..."},
    "hosts": [{"host_id": "host-a", "certificate_sha256": "<64 lowercase hex>"}]
  }
  ```
  服务端在 owner mTLS `grpc_listen` 注册真实 `HostBridge.Connect`：首帧 registration 的
  `host.host_id` 必须与 peer 证书指纹 allowlist 绑定一致，回 `registered{host,fence}`；后续
  inventory/receipt/observation/heartbeat 必须携带当前 fence，旧 generation 以 `CONFLICT`
  拒绝。heartbeat_ack 的 `received_at`/`interval_seconds=5`/`resync_required` 由 P08b-1 应用
  计算；observation/inventory 的持久投影与 Host 命令 outbox 仍属 P08b-3。**未完成**：PG
  `host_connections` 持久化尚未接到该 server（当前 composition 用进程内 store，Runtime 重启
  后注册/代次不保留），真实 Host 二进制/PG 端到端验收属 P08b-2c；不得据此宣称 T11b 通过。
- **P08b-3 受管命令/凭据（部分实现）**：带 `host_id` 的受管实例创建/desired 状态变更会在
  Runtime 同一 UoW 写 `host_commands` outbox（确定性 command_id、generation/desired/package/
  profile/payload 快照）；只存 SHA-256 的受管 bootstrap 与外部自管共用 `bridge_credentials`，
  明文仅出现在 CreateInstance 响应，部署方须按 P08a 约定写入 Host
  `credential_directory/<instance_id>.bootstrap`（0600、属主 Host 用户）并随部署回收/轮换。
  **未完成**：HostBridge server 尚未实际下发命令和持久化 receipt/observation（当前帧校验后
  只接 transport-only 回调），未确认命令保持 Pending/Unknown、不伪造 stopped；重连重放和
  `Resync` 去重无端到端证据，P08b-3 未验收。
  **P08b-3a**：HostBridge server 已接 PG 回调，注册/心跳后按 generation 下发 pending
  apply/stop/drain；receipt 以 (command_id, connection_generation) 条件更新（旧 generation
  返回失败），observation 按 `(host_id,connection_generation,cursor.sequence)` 去重并校验 cursor
  epoch；重连新 generation 重放未确认命令。30s 无回执转 Unknown（不伪造 stopped），7d/30d
  保留清理随 P13a 60s 批次。**未完成**：真实 Host 二进制 e2e、受管 bootstrap 自动预置。
  未确认命令在运维视图应显示 Pending/Unknown，不能当 stopped。
- **P08b-3b（部分）**：新增 `p08_host_bridge_e2e` 真 PG16.4 + 真 Runtime + fake Host 客户端
  用例（p08 标签），覆盖 apply 快照、receipt、observation 去重、重连 generation 与旧 fence；
  实测 Runtime schema v16 / descriptor `ec12933ab8bff61620db24ebcb05ef44` / 152 relations。
  **未完成**：真实 `kiarabot-plugin-host`+受管 SDK 插件进程 e2e；受管 bootstrap 自动预置受
  冻结协议限制（无 HostBridge 凭据交付帧，Runtime 不直接写 Host 文件系统），仍需部署方按
  0600 文件约定预置或补契约后实现；不得据此宣称 P08 退出。
- 当前验证范围：P08a 用 in-process fake Runtime bridge 与 fake supervisor 覆盖注册/fence/心跳/
  resync/命令回执/观察/恢复；真实 Runtime Bridge 监听（P06a-2）与 Host 受管配置的端到端验收
  尚未执行，不能据此宣称 T11b 或真实部署通过。
- P08c SDK 配置应用（2026-11-02 已实现 Python/SDK 侧）：Host 按 launch profile 注入
  `KIARABOT_PLUGIN_BOOTSTRAP`，SDK 读取后走 P06 `PluginBridge` 首帧认证；配置 ACK 回带当前
  fence 与收到的 `config_version`/`delivery_id`，只有 Runtime 判定 `config_version ==
  desired_version` 才推进持久 `applied_version`。SDK 对高于注册响应的 durable desired 的投递
  以 `INVALID_ARGUMENT` 拒绝且不应用；apply 失败/超时保留上一成功配置快照与本地 applied，
  重连/进程重启后按新 delivery 幂等重放并重新 ACK。Host 只持久进程/命令/观察证据（L2），
  `applied_version`/`apply_state` 仍归 Runtime（L4），HostBridge 不承载配置应用证据。
- P08c 验证：`sdk/python/tests/test_host_managed.py` 用真实 `kiarabot-plugin-host`（P08a 二进制，
  无构建产物时 skip）+ 临时 mTLS 证书 + fake HostBridge/PluginBridge + 真实 SDK 受管子进程，
  覆盖 HostBridge 注册/命令回执/观察/停止/新一代重放与配置 apply→ACK→重启重放；真实 Runtime
  HostBridge（P08b）与真实 PG 配置证据 e2e 仍待 P08b/P08d 合入后联调。

### Host 离线启动与注册重试（P08a 回归修复，2026-11-02）

`kiarabot-plugin-host` 在 Runtime HostBridge 不可达时必须仍能启动：Host 入口先打开本地
SQLite journal、恢复受管执行、启动探针/gRPC 监听，然后由 HostBridge client 在后台以有界指数
退避（200ms→5s）持续尝试连接 Runtime 内部 mTLS `grpc_listen` 并完成注册。启动阶段不伪造
“已注册”：readiness 在 Runtime 不可达期间为 false（`/health/ready`=503 UNHEALTHY），
`/health/live`=200；Runtime 恢复后注册成功、读到 Runtime 分配的 fence 并通过
`bridge_ready` 补发积压的 execution observations，readiness 转为 true。离线不等于受管进程
已停止：Host 不会伪造 stopped/FAILED，也不会在启动失败时删除 journal。

P02 生命周期 fixture 因此为 host 提供合法 `host` 材料（临时 `sqlite_path`/
`credential_directory`、dummy package/profile、`runtime_endpoint=127.0.0.1:1` 不可达），只验证
进程存活、probe/readiness 语义和 mTLS 未实现方法行为，不启动任何受管实例。Host 的重试循环
在进程停止（SIGINT/SIGTERM）时通过取消在途流并 join 连接/写/心跳线程退出；未注册期间
`send_observation`/`send_receipt` 返回 `UNAVAILABLE`，观察仍保留在 journal 中等待重连补发。

## P10a Scheduler 服务运行与维护（第二阶段）

`kiarabot-scheduler` 的组合根在配置含 `database` 时打开 Scheduler 私有 PostgreSQL 库并注册
真实 SchedulerService unary 适配器；未配置数据库时使用拒绝型工厂：进程仍可启动、探针存活、
`/health/ready`=503，但不接受任何业务调用（P02 无数据库生命周期）。readiness 依赖库可用性与
IANA zoneinfo；Identity delegation/current-authorization 尚未接线（authorizer fail-closed），
因此当前真实部署 readiness 始终为 false、RPC 返回 UNAUTHENTICATED，不能作为任务功能可用性
证据。维护循环每秒在进程线程执行有界 due sweep/过期 trigger/保留期清理，busy 不退出，其它
数据库故障终止服务。数据库 schema 为合并 P07a/P11a 后的 Scheduler v9（观察列/quota counter/索引/descriptor
已在真 PG16.4 重测验证）。生产环境需确认 `/usr/share/zoneinfo` 存在且包含所需 IANA zone；非法或不
存在的 zone、DST gap/fold 返回 INVALID_ARGUMENT，缺失 zoneinfo 返回 UPSTREAM_UNAVAILABLE。

## P10b Scheduler→Runtime 触发交接（第一阶段）

Scheduler 配置新增可选的顶层 `identity_endpoint`（与 Runtime 相同：Identity owner gRPC endpoint，
复用服务 TLS 作为出站 mTLS 身份）和 `runtime_endpoint`（Runtime 内部 mTLS owner 监听，用于
`RuntimeService.AcceptTimerTrigger`）。两者缺省时 Scheduler 仍可启动但 readiness=false（Runtime/
Identity 依赖不可用），拒绝一切业务调用。配置值必须是 host:port；其它服务出现这些键会被拒绝。

维护循环每秒在进程线程执行有界操作：due sweep、过期 trigger、保留期清理，然后在
`runtime_endpoint` 配置时 claim 最多 5 个 trigger outbox 并调用 Runtime AcceptTimerTrigger
（5s 预算），成功则 confirm occurrence/task COMPLETED；Runtime 永久拒绝将 occurrence/task 置
FAILED；Runtime 不可用/超时/限流保留 claim，到期重试同一 occurrence/event id（Runtime 以
event id+fingerprint 去重，不会重复入队或 fan-out）。COMPLETED 仅表示 Runtime 已持久入队，
不代表插件已执行。当前 Scheduler RPC authorizer 与多进程 e2e 仍在收尾：authorizer 仍 fail-closed，
readiness 不会因真实 Identity 授权链而转 true；部署前需等待后续增量完成 Identity delegation
接线与真 PG+多进程验收。

## P10b Scheduler 授权与多进程验收状态（第二阶段）

Scheduler 配置：`runtime_endpoint` 与 `identity_endpoint` 均为可选顶层键（仅 Scheduler；
host:port）；Runtime 的入站 owner 方法 authorizer 仍由 `identity_endpoint` 激活。Scheduler
authorizer 需要 Identity 签发、`aud=scheduler`、客户端证书绑定到调用方 mTLS 叶证书、且权限
绑定中包含 Installation resource scope 的 delegation proof；owner instance 取 proof subject。
缺任一字段/依赖故障均 fail-closed。readiness 需 DB、zoneinfo、Runtime delivery、Identity
全部可用才为 true；未配置 `identity_endpoint` 时保持 fail-closed readiness=false。

**部署/验收阻塞**：Scheduler→Runtime AcceptTimerTrigger 目前没有可用的平台服务主体凭据来源
（Runtime owner authorizer 要求 Identity delegation proof；契约/配置未定义 Scheduler 服务主体
的免委托 internal 授权或预置 delegation 字段）。因此多进程 e2e 未执行；在补齐
Identity/Runtime 服务主体约定前，不能把 Scheduler 交接标记为多进程验收通过。单进程/真实 PG
覆盖见 p10_scheduler_identity、p10_scheduler_rpc、p06_postgres_runtime_events 与
p10_postgres_scheduler。

P12a-2（2026-11-02，契约登记）：受权 `GET /api/v1/health` 使用显式 `health.read`（Permission 50），匿名 live/ready 探针不暴露组件；target Control v1 无 WebSocket 服务端，运维排障/面板恢复以 `/api/v1/operations/...` REST 快照为准。`/logs` 与 `/audit` 是 Control 只读聚合，分别需 `log.read`/`audit.read`（48/49），日志默认 7 天、必要审计最小 180 天；无 owner 权威来源时返回 503，不返回空页冒充无事件。旧 `/plugins/{id}/enable|disable|config` 不映射到实例级启停/配置，实例路径见 doc/06。

## P14c 本地真实备份/恢复与故障 runbook（2026-11-02）

### 状态与范围

- **已实现并本地真实验证**：`scripts/ops/target-backup.sh`、`target-restore.sh`、`target-drill.sh` 与 `scripts/ops/lib.sh`；真 PG16.4 四库 + Host/Adapter SQLite（WAL）+ CA/证书/root key/config/credential 目录的备份/恢复和故障矩阵。演练记录见 `reports/p14c/p14c-local-drill.md`。
- 仅验证**本机单实例**；Docker Compose、双主机、离站复制和真实生产数据量下的 RTO 未验证，不得宣称已验证。
- 脚本不自动迁移、不清库、不启动服务；恢复是显式停机例外，必须 `--confirm`。
- 文档影响：本段仅描述运维脚本/流程，未修改 `api/`、`proto/`、`control/`、`core/` 契约或服务行为。

### 备份

前置（与 P13b 协调）：先完成 drain/停写。建议顺序：Control（停止新流量）→ Adapter → Host → Scheduler/Runtime/Model → Identity（最后）。**所有服务停止且四库无客户端连接后**才允许备份；`target-backup.sh` 会以 `pg_stat_activity` 校验并在有活跃 writer 时失败。

命令（算子环境文件需 `chmod 600`，属于秘密材料）：

```bash
scripts/ops/target-backup.sh \
  --root /path/deploy/target \
  --env /path/operator/target.env \
  --output /path/backups \
  --retain 7 --offline
```

产出 `target-<UTC>-<rand>/`，含：`postgres/{identity,runtime,scheduler,model}.dump`（`pg_dump -Fc` + schema_version/SHA-256）、`sqlite/{host,adapter-outbox[,adapter-actions]}.sqlite`（先 `wal_checkpoint(TRUNCATE)` 再 `VACUUM INTO`，`integrity_check`）、`material.tar.gz`（config/tls/secrets/host credentials）、`manifest.txt`。只有 manifest 写成功才算完整备份；失败会删除临时集，不发布部分备份。

环境文件至少包含：`PG_HOST/PG_PORT/PG_CA_FILE/PG_BIN`、四个 `<SERVICE>_DB`、`<SERVICE>_MIGRATION_ROLE`、`<SERVICE>_MIGRATION_PASSWORD_FILE`、`HOST_SQLITE_PATH`、`ADAPTER_OUTBOX_PATH`、可选 `ADAPTER_ACTIONS_PATH`/`HOST_CREDENTIAL_DIRECTORY`。`PG_CA_FILE` 用于连接 PG 的受信 CA，建议由算子保存在材料目录之外（恢复材料中的 CA 丢失时仍能连库恢复）。

保留/离站/密钥托管是待用户确认策略：脚本默认保留最近 7 份完整集；**不提供异地复制**，必须由算子将备份卷复制到独立受保护位置。

### 恢复

前置：所有服务停止；PG 可达；SQLite 无 `-wal/-shm` 活跃使用者；备份集完整。恢复脚本先做全量预检（manifest、每个 PG dump/SQLite/material 的 SHA-256、SQLite integrity、user_version、可选 `--expect` schema 版本），任一缺失/损坏立即失败，**不触碰 live 数据**；随后按“材料 → SQLite → 四个显式 PG 库”顺序恢复。它不调用迁移、不 drop 数据库、不跨库级联；若中途失败会显式提示“可能已部分恢复”，必须保持停机并按本节人工核对，不得直接启动服务。

```bash
scripts/ops/target-restore.sh \
  --root /path/deploy/target \
  --env /path/operator/target.env \
  --input /path/backups/target-20261102T121944Z-XXXX \
  --confirm \
  --expect identity=14 --expect runtime=15 --expect scheduler=9 --expect model=8 \
  --verify-sql /path/operator/verify.sql
```

恢复后校验：四库 `kiarabot.schema_versions` 与备份/`--expect` 一致、关系数 >0、`--verify-sql` 返回 `ok`；SQLite `integrity_check=ok` 且 `user_version` 与 manifest 一致；材料 `tls/ca.pem`、`secrets/model-root-key.hex`（0600）存在。随后按 **PostgreSQL/Identity → Runtime/Scheduler/Model → Host → Adapter → Control** 启动，并执行观察窗口（见下）。

### 启停、健康与降级（与 P13b）

- 启动顺序：PG（迁移仅由显式初始化/迁移命令，不在服务启动时自动迁移）→ Identity → Runtime/Scheduler/Model（owner 私库+在线授权）→ Host（SQLite journal/credential dir）→ Adapter → Control。任一步 readiness 不通过就停在原地，不跳过健康检查。
- 停止顺序：Control → Adapter → Host → Scheduler/Runtime/Model → Identity；先完成 P13b drain，再确认 owner 私库无活跃 writer，最后执行备份。P13b 的清理/保留（60 秒有界批次、unresolved receipts/inbox/outbox 不提前清理）在停机前自然停止；不要为缩短停机手工删除 journal/outbox。
- 健康口径：`/health/live` 进程存活；`/health/ready` 就绪（Identity 未就绪时 503）；受权 `/api/v1/health` 为四依赖（identity/runtime/scheduler/model）聚合：单点 Runtime/Scheduler/Model 故障为 **DEGRADED（HTTP 200）**，相关读取返回 UPSTREAM_UNAVAILABLE，不伪造空数据；Identity/凭证链/撤销异常为 **UNHEALTHY（HTTP 503）**。恢复后观察至少一个完整备份周期/健康轮询周期，确认无 DEGRADED/UNHEALTHY 再恢复流量。

### 秘密与轮换

- 备份材料包含 CA/证书/root key/配置/凭据目录；必须与备份卷一起离线保管并限制访问。root key/CA 丢失时优先从最近完整备份恢复材料，再重启依赖服务；不要用新 CA 替换旧材料后直接启动，否则已签发证书/加密状态可能不可读。
- 轮换顺序（停机窗口）：新 CA/证书/root key 先写入备份/材料副本 → 按依赖顺序重启 Identity → Runtime/Scheduler/Model → Host → Adapter → Control；旧材料保留到观察窗口结束。模型 provider secret 轮换只通过受控接口/离线流程，Panel/日志不回显密钥。
- 每次轮换前做一次完整 `--offline` 备份，并记录 manifest/SHA-256；密钥托管方式（KMS/离线介质/分片）待用户确认，脚本不假设。

### 故障处理

| 故障 | 处置 |
|---|---|
| 单 PG 库丢失/损坏 | 保持全部服务停机；用最近完整备份执行 `target-restore.sh --confirm`（会恢复四库+SQLite+材料并校验版本）；不得只恢复一库后直接启动，也不得让服务自动迁移。 |
| 单服务停机 | 其他服务可继续；该服务恢复后按启动顺序/readiness 归队。备份必须在所有 writer 停止后做，单服务未停时脚本会显式失败而不是做部分备份。 |
| 证书/root key 缺失 | 立即停止依赖服务；从备份恢复 `tls/`/`secrets/`（保持 0600、非符号链接）；确认文件完整后再启动。禁止用空/新密钥顶替。 |
| Host journal 损坏 | 停止 Host/Adapter；从备份恢复 Host SQLite（脚本会 integrity_check），不要删除 live `-wal/-shm` 后手工拼库；Adapter outbox/.actions 同步恢复，未终态记录不得被重放。 |
| 部分恢复 | 只接受“全部组件预检通过后完整恢复”。若脚本中途失败或人工只拷贝了部分文件：保持停机、核对 manifest/`schema_versions`/SQLite integrity，重新完整恢复；**禁止**启动服务或用迁移“补齐”。 |
| 备份介质损坏 | 换上一份完整备份集；保留损坏集用于审计，不用部分文件拼装。 |

### 演练与检查

```bash
bash scripts/ops/tests/ops_scripts_test.sh   # shell syntax/help/safety gates
bash scripts/ops/target-drill.sh             # 真 PG16.4 + SQLite 本地全矩阵（约 1 分钟）
```

演练脚本使用 owned 临时集群和临时目录，不触碰仓库/真实部署；需要 `PG16_BIN`、`KIARABOT_PG_INITIALIZE`、`OPENSSL_BIN`/PATH、`sqlite3`。实测（本地小数据量）**RPO=0s（停写备份）、RTO≈4.6s（最近一次；范围 4.6–8.5s）**，详见 `reports/p14c/p14c-local-drill.md`。生产数据量、Docker/双主机与离站 RTO 仍未验证。

### P14 范围决定对齐与验收分类（2026-11-02 用户确认）

- **默认部署形态是单主机 Docker Compose**：PG + 控制面 + 执行面同机，但每个服务独立容器/TLS/凭据/DB 权限边界；两主机为可选，跨主机断网/重连演练**延期并单独记录**（当前未验证）。
- **PostgreSQL 镜像**：生产目标 `postgres:16.4-bookworm`（复现 P00），发布时使用 PG16 当前修订并固定 digest；full-locked CI 保留 P00 源码构建。本机无 Docker 时，本地演练使用 P00 源码构建 PG16.4（server_version_num=160004）；**官方镜像 digest 与容器生命周期验证待 Docker 环境**。备份/恢复脚本只使用 libpq/pg_dump/pg_restore 公共语义，不依赖源码构建特有能力。
- **秘密**：以只读文件挂载为主，按需增加 Docker secrets；本期人工轮换，KMS/热轮换延期。轮换流程见上节，停机窗口内按 Identity→Runtime/Scheduler/Model→Host→Adapter→Control 顺序重启并验证旧凭据失效、新凭据可用。演练脚本已验证一次凭据替换（旧密码被拒/新密码用于备份成功）；**真实服务进程的按序重启需在单机 Compose 验收补做**。
- **验收分类**：

| 类别 | 状态 |
|---|---|
| 单机数据层备份/恢复/故障演练（P00 PG16.4 + SQLite） | 本机实测 PASS（RPO=0s，RTO≈4.6–12.1s，小数据量；见 reports/p14c） |
| 单主机 Docker Compose（默认） | 未验证（本机无 Docker；启动顺序/健康口径已文档化） |
| 两主机 + 跨主机断网/重连 | 延期并单独记录 |
| 远端 CI | 未运行远端 CI；P14b 入口存在 |
| 正式发布 | 未就绪（官方镜像 digest、主机/秘密托管、保留离站、Compose 验收待完成） |

- **仍需用户确认**：备份保留周期、离站复制位置/频率、密钥托管方式（KMS/离线/分片）；脚本默认只保留本地最近 7 份完整集，不提供异地复制。
P13b（2026-11-02）：Host 进程 SIGTERM 走有界优雅收尾（活动受管进程 TERM≤grace10s，再 KILL；最终观察上报；Host SQLite journal prune 后关闭；HostBridge 断开），退出码 0；无法确认的进程结果保持 UNKNOWN，不伪造 stopped。SDK 收到 ShutdownNotice 后停止接新事件、在 drain 窗口内收完已受理事件的 ACK/回执、最后调用 `on_stop` 并以 0 退出；Adapter 保留未确认本地 outbox，drain 窗口后按退避重连，致命鉴权错误以非零退出。崩溃恢复：Host 重启按 marker/代次恢复，不重复 spawn 同代次、不把崩溃执行改写成成功；Host 本地日志与 Runtime 中央库的恢复/清理分别由 P13a/P14 协调，不跨库级联。

## P14a-2 默认单主机 Compose、官方 PG 镜像与秘密轮换（2026-11-02）

- 状态：**文件/脚本/离线检查完成；本机无 Docker，镜像构建、`docker compose config/up`、官方镜像初始化/迁移/恢复均未运行**。不得把本篇当作已验收部署。
- 默认拓扑（用户确认）：单主机 Docker Compose，PG + Identity/Runtime/Scheduler/Model/Control/Panel + Plugin Host/Adapter 同机，各服务独立容器、TLS 材料、凭据文件与 DB 角色/权限边界。`deploy/compose.target.yml` include 三个组件文件；单机 `--profile exec`，一次性初始化 `--profile init`。两主机可选，Host-A=infra+core、Host-B=exec `--profile host --profile adapter`；跨主机断网/重连演练**延期并单独记录**（未验证）。
- PostgreSQL：默认 `postgres:16.4-bookworm`（复现 P00）；release 用发布时 PG16 当前修订并固定 digest，`target-smoke.sh --release` 强制 `POSTGRES_IMAGE=postgres:<PG16>-bookworm@sha256:<64 hex>`，并同时校验 UBUNTU_BASE/服务镜像 digest、非 dev `KIARABOT_VERSION` 与 secrets 模式。full-locked CI 的 P00 源码构建不变。官方镜像的初始化/迁移/恢复步骤：启动 postgres → `--profile init run --rm init-*`（逐库、非原子）→ 显式迁移 → P14c 备份/恢复脚本；**本机无 Docker，实际未运行**。
- 秘密：默认只读文件挂载（config/tls/secrets）；`target.env.example` 增加 `KIARABOT_SECRETS_DIR/MANIFEST` 与逐秘密路径，`deploy/secrets.manifest.example` 记录文件名/mode/消费者/轮换重启顺序。Docker secrets 为按需覆盖（`deploy/compose.docker-secrets.yml`，挂到 `/run/secrets`，选择时 config 路径需同步）。本期人工轮换，KMS/热轮换延期。
- 人工轮换流程：先做 P14c 离线备份（`scripts/ops/target-backup.sh --offline`）→ 用
  `scripts/deploy/target-rotate-secret.sh --env deploy/target.env --name <name> --value-file <file> --confirm`
  原子替换（0600、旧值备份、manifest 校验）→ 按 Identity→Runtime/Scheduler/Model→Host→Adapter→Control→Panel 顺序重启 → 验证新凭据可用/旧失效 → 观察窗口结束后再清理备份。脚本不自动重启容器、不访问 secret store；离线测试 `scripts/deploy/tests/target_rotate_test.sh`。
- 检查（本机实际运行）：`scripts/deploy/target-smoke.sh` 通过（Compose YAML、target include、host/adapter profiles、官方 PG 默认、安全约束、shell 语法、rotate 测试、release 规则可离线校验）；`target-smoke --release` 对含 digests 的临时 env 通过、缺失 PG digest 失败（见提交说明）；`bash -n` 全部部署脚本；`scripts/deploy/tests/target_rotate_test.sh` PASS。
- 未验证/未完成：Docker 构建/Compose 启动/healthcheck/官方 PG 镜像初始化/迁移/恢复、两主机与跨主机故障、真实秘密轮换后的容器按序重启（本机无 Docker，数据层轮换仅 P14c 演练覆盖）。
- 剩余待确认：备份保留/离站策略、密钥托管（KMS/离线/分片）；正式发布仍受官方 PG 镜像 digest/Compose 验收/秘密托管阻塞。
## P12e Identity/Runtime 只读审计/日志查询（2026-11-02）

Identity 与 Runtime 各新增两个只读 owner RPC（不新增服务、权限编号或数据库表）：

- `IdentityService.ListAudit` / `ListLogs`：caller 必须是 Control platform peer，Identity 在自身库
  复核该 Control 主体的当前注册/凭据，并要求 Control principal 上有显式 `audit.read`(49) /
  `log.read`(48) 的精确 `RESOURCE_CATALOG` id `control` assignment；不跨库、无委托转发。
- `RuntimeService.ListAudit` / `ListLogs`：caller 必须是 Control 且携带当前在线委托；Runtime
  通过 Identity.CheckAuthorization 复核精确权限与同一 `RESOURCE_CATALOG`/`control` scope 后才查询
  自己的 `audit_entries`。
- 统一语义：`occurred_at` 降序 + 稳定 bigint id 的 keyset 分页（`from` 含、`until` 含、时间窗、
  actor、resource 过滤；默认 20 最大 100，无效/篡改游标 `INVALID_ARGUMENT`）；日志是同一权威
  审计事实的脱敏运维投影（severity `info|rejected`、component=owner、message 为闭集
  `request.recorded`/`request.rejected`），不返回凭据、消息正文、模型密钥或自由错误文本。
- 失败语义：持久化查询依赖不可用返回稳定的 `UPSTREAM_UNAVAILABLE`（RPC/Control 映射 503），
  不返回成功空页；只有确认无匹配记录的已授权查询才返回空页；权限/凭据失败分别
  `UNAUTHENTICATED`/`PERMISSION_DENIED`。
- Control 的 `/logs`、`/audit` 聚合接线仍由 P12c 完成；在此之前 REST 继续 503，owner RPC 已可被
  Control 受权调用。验证：`p03_postgres_database`（真 PG16.4 分页/过滤/脱敏）与
  `p12e_audit_query`（游标契约）以及 `proto/tests/check_target.sh` 契约门禁。
P13c（2026-11-02）：Runtime BeginDelete 对受管实例的 Host 参与者已改为写 durable StopExecution host_command outbox，按确定性 command_id 幂等；只有 Host receipt 的已知终态才算完成，Pending/Unknown/拒绝保持 blocked，UNKNOWN 不伪 stopped。P08b-3b 完成 HostBridge 命令下发/回执后自动收敛，无需改 SDK/Control；Scheduler 参与者需 Runtime→Scheduler 实例委托配置（P10b-2），未配置时删除实例保持 BLOCKED。`/operations/...` 参与者进度已在 Control owner gateway 投影。
