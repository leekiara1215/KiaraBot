# 部署与可观测性设计

T06a 持久化插件入口同时装配 HostDispatcher，只协商已实现的 host.storage.v1；storage.namespace 必须显式列入 KIARABOT_PLUGIN_PERMISSIONS。数据库迁移到 schema v4，回滚使用迁移前备份。Host 存储和幂等响应随 Core 数据库一起备份，不能单独删除幂等响应后重放写入。

T04b 启用持久化插件入口时装配 ActionRouter，插件 message.send 权限仍显式配置；只声明已实现的 action.result.v1。T04b 引入 schema v3，当前 T06a 已继续升级至 v4，旧二进制不能读取新 schema，回滚必须使用迁移前备份。独立适配器在 OUTBOX_PATH 旁维护 .actions 执行日志，备份/恢复须与事件 outbox 一起处理，禁止删除未终态记录后重放平台请求。

## T03b 受管 Python 插件

在 T03a 插件入口基础上，配置 KIARABOT_PLUGIN_EXECUTABLE（Python 可执行文件绝对路径）和 KIARABOT_PLUGIN_MANIFEST（清单绝对路径）；可选 KIARABOT_PLUGIN_PYTHONPATH 指定 SDK、生成协议和插件模块目录。Core 执行 `python -m kiarabot_sdk --manifest ...`，不经过 shell。默认启动等待 5000 ms、停止宽限 2000 ms，可分别用 KIARABOT_PLUGIN_START_TIMEOUT_MS、KIARABOT_PLUGIN_STOP_TIMEOUT_MS 配置为 10–30000 ms。配置不完整或缺少插件入口时启动失败。

子进程只接收最小 PATH、插件 token、endpoint、manifest 参数和可选 PYTHONPATH，不继承其它 Core 秘密。stdout/stderr 当前重定向到空设备；Core 仅报告启动、退出、强制回收及稳定错误。插件崩溃不退出 Core；停止对插件进程组先 TERM、后 KILL，并回收直接子进程。当前面向本机 POSIX 开发运行，生产资源沙箱、自动重启和镜像仍待后续任务。

T03a：在持久化入口配置基础上，设置 KIARABOT_PLUGIN_ID/TOKEN 及 KIARABOT_PLUGIN_PERMISSIONS（逗号分隔权限；事件插件至少 message.read），即可启用单个已授权插件连接。库级 PluginHub 支持多个策略/连接，CLI 当前只配置一个插件。没有权限配置仍只提供插件传输认证并拒绝业务。插件启动器、Action/Host 和控制服务尚未完成，因此 rpc_ready 保持 false；plugin_delivery_ready 只表示本机插件投递入口已启用。

## 责任

运维层负责让各个模块可启动、可停止、可检查、可升级和可恢复。

## 目录

```text
deploy/
  compose.dev.yml
  compose.staging.yml
  compose.prod.yml
  docker/
  systemd/
scripts/
  check-toolchain.sh
  check-contracts.sh
  check-boundaries.sh
```

数据库迁移源码位于 `infrastructure/sqlite/migrations/`，运维目录只负责编排、启动、健康检查、备份和回滚。

## 进程

```text
napcat
kiarabot-adapter-nonebot
kiarabot-core
kiarabot-panel
```

开发环境可以使用 Docker Compose，单机生产环境也可以使用 systemd。无论使用何种方式，进程配置都不能改变业务协议。

## 启动顺序

1. `kiarabot-migrate` 获取迁移锁并完成数据库迁移；
2. Core 启动并检查 schema 版本；
3. Python 适配器注册并报告能力；
4. NapCatQQ 连接适配器；
5. 面板静态资源或面板服务连接 API。

## 健康检查

每个进程必须提供：

```text
/health/live     进程是否存活
/health/ready    是否可以接收请求
/health/details  依赖和版本详情
```

`live` 只检查进程；`ready` 检查数据库 schema、数据库可写、协议服务和必要运行时；`details` 必须脱敏。Core 未 ready 时 API 返回 503，适配器未 ready 时不接收新 Action。

```text
Core live       不依赖数据库
Core ready      依赖迁移完成 + 数据库可写 + RPC 监听
Adapter ready   依赖 Core 注册 + NapCat 连接
Panel ready     依赖 API 可达，不要求 QQ 在线
```

适配器还要报告 NapCat 和 Core 的连接状态。插件通过 PluginSDK 上报健康状态。

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

每个操作的成功、拒绝、重复、超时、重试、死信、依赖故障和状态转换都必须产生至少一条结构化记录，并通过 `request_id`、`trace_id`、`event_id` 或 `action_id` 关联。日志中不得记录 Token、API Key、消息正文和未经脱敏的私聊内容。日志端口和 sink 在后续实现任务中落地，运维层负责 JSON sink、轮转、集中收集、保留期和告警。腾讯 [BqLog](https://github.com/Tencent/BqLog) 是候选 sink，接入前需要单独完成许可证、性能、格式和跨平台评估。

## 备份和升级

- `.env` 和密钥不进入 Git；
- 数据库在迁移前备份；
- 插件升级前记录版本和校验值；
- 协议升级先做兼容测试；
- NapCat、适配器和 Core 分别记录版本；
- 失败升级应可以回滚到上一个版本。
- 迁移命令必须可单独、可重复运行，失败时阻止后续服务启动；
- 备份必须包含 schema 版本和校验值。

## 不属于本层的内容

- 不修改业务数据绕过 API；
- 不在启动脚本中实现权限判断；
- 不把第三方源码复制进 Core；
- 不通过日志系统传递业务命令。

## 验收标准

- 任一进程退出后可被单独重启；
- Core 和适配器断线可以恢复；
- 健康检查能区分存活和就绪；
- 日志可以按 trace_id 关联一条消息链路；
- 数据库和插件配置有备份方案。

## 代码启动句

> 请只实现 KiaraBot 各进程的 Docker Compose/systemd 启动配置、健康检查、结构化日志、重启策略和本地开发环境，不修改 Core、插件或适配器业务代码。

## 当前部署配置与依赖

运维模块提供 `Dockerfile`、`.dockerignore`、`deploy/compose.staging.yml`、`deploy/compose.prod.yml`、`deploy/systemd/kiarabot.service` 和 `scripts/deploy/`。镜像只复制 Core 构建输入及运维脚本，构建上下文使用白名单；运行用户为 UID/GID 10001。Compose 的四个服务依次等待迁移成功、Core ready 和适配器 ready，面板也有独立 ready 检查。适配器、面板镜像必须自带 `curl`，以 UID 10001 运行，并在各自容器的 8081、8082 端口提供健康接口。镜像引用和命令可通过 Compose 中的 `KIARABOT_*` 变量配置；生产配置中的全零 digest 仅是无法拉取的占位值，部署前必须指定真实 digest 镜像。staging 和 production 使用独立数据卷及默认端口。

**当前不能作为完整部署运行**：Core 已有 POSIX 持续等待与受控关闭入口，并可显式启动本机 gRPC 认证传输，并可在 T02a 配置下持久化接收适配器事件，但尚未具备插件/Action 闭环；仓库仍缺迁移程序、适配器及面板服务镜像，也没有三个进程健康端点。OpenAPI 当前只定义控制 API 的 `/api/v1/health`，并未定义这里的 `/health/live`、`/health/ready`、`/health/details`。这些进程端点的可访问性、认证、响应和失败语义仍需所属模块及公共契约明确；运维配置保留既有文档要求，不将它们伪装成已实现的 API。补齐这些依赖后才能验证真实服务启动及跨模块健康状态。

Compose 在 `deploy/.env` 存在时可通过可选 `env_file` 注入运行配置，也可以使用 `KIARABOT_ENV_FILE` 指定外部文件；Compose 插值变量通过启动进程环境或 Compose 的 `--env-file` 提供。环境文件和密钥不应放进仓库或构建上下文，权限应限制为部署账户可读。镜像构建不需要 Token、API Key 或 SSH key。不要把这些值放进镜像标签、命令行或日志。

```bash
# 检查配置；生产环境先设置三个真实的镜像引用（含 digest）
docker compose -f deploy/compose.staging.yml config
docker compose -f deploy/compose.prod.yml config
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh start
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh status
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh stop

# 运维探针不输出响应正文；details 的脱敏由服务实现负责
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /health/live
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /health/ready
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /health/details
```

Linux 主机可将仓库部署到 `/opt/kiarabot`，将 `deploy/systemd/kiarabot.service` 安装到 systemd，使用 `/etc/kiarabot/production.env` 指定环境变量，再运行 `systemctl daemon-reload` 和 `systemctl enable --now kiarabot.service`。该服务由 Docker 守护进程管理容器重启；停止服务会执行 Compose down，但保留数据卷。变更这些示例路径时应同步修改 unit。容器 stdout/stderr 使用 Docker 日志驱动，部署脚本输出 JSON 字段 `timestamp`、`level`、`component`、`operation`、`outcome`、`request_id`、`trace_id`、`resource_id`、`message`、`error_code`；业务进程仍须自行提供上文要求的结构化字段与脱敏，运维脚本不会将日志作为命令通道。

### 备份、迁移和回滚

`scripts/deploy/migrate.sh` 使用排他锁；数据库已存在时先用 SQLite `VACUUM INTO` 创建一致性快照，并写入 `PRAGMA user_version`、SHA-256 和创建时间。数据库不存在视为首次部署；未设置 `MIGRATION_COMMAND` 时以非零状态停止，因为当前仓库没有迁移程序；实际迁移程序必须由所属模块提供，必须可重复运行，并通过受控环境变量提供命令。确认无需迁移时可明确设置 `MIGRATION_COMMAND=true`；命令失败会以非零状态阻止后续服务。异常终止可能留下锁文件，确认无迁移进程后才能人工删除。备份保存在数据卷内，操作员还须定期复制到独立的受保护位置；本配置不包含插件配置备份。

```bash
# 命令在数据卷内运行；输出最后一行是备份文件路径
docker compose -f deploy/compose.prod.yml run --rm --no-deps migrate /usr/local/bin/backup.sh backup
# 停止所有数据库使用者后，检查并清理不再使用的 SQLite -wal/-shm 文件，
# 再用备份的精确路径恢复；脚本先校验 SHA-256，拒绝存在 sidecar 的目标。
docker compose -f deploy/compose.prod.yml stop panel adapter core
docker compose -f deploy/compose.prod.yml run --rm --no-deps migrate /usr/local/bin/backup.sh restore --input /var/lib/kiarabot/backups/<backup-set>/database.db
```

升级前设置 `KIARABOT_CORE_IMAGE_DIGEST=sha256:<64位十六进制>` 和 `ROLLBACK_IMAGE_DIGEST=sha256:<64位十六进制>`，然后运行 `scripts/deploy/deploy.sh upgrade`。脚本停止服务、在数据卷内运行迁移（先备份），再依次等待各服务 ready；任一步失败会尝试以指定的旧 Core digest 依次启动服务并返回非零。`scripts/deploy/deploy.sh rollback sha256:<64位十六进制>` 可单独执行镜像回滚，不再次执行迁移。镜像回滚不会自动恢复数据库：只有确认旧版本与当前 schema 兼容时才能直接使用；否则停止所有数据库使用者，核对备份和 schema 后显式恢复，再启动旧版本。升级和回滚只更换 Core 镜像，适配器和面板的版本由其镜像变量独立管理。备份恢复涉及数据替换，必须先确认恢复点及写入损失。

## 其他模块需要完成的依赖

下列事项由对应模块负责人实现并验证；运维层不代替它们定义业务行为或修改公共契约。

1. **Core**：将现有一次性启动检查替换为可持续运行并可正常停止的服务；提供所属接口约定的健康检查，使存活与依赖就绪可区分。就绪条件须覆盖已确认的 schema、存储及协议监听状态，健康详情不得泄露秘密。当前无参数入口保持存活，SIGINT/SIGTERM 正常退出；`--check` 仅做一次性自检。入口尚无 RPC、存储和健康服务，仍无法通过 Compose 的 ready 检查。
2. **协议和控制 API**：明确进程级 `/health/live`、`/health/ready`、`/health/details` 与现有 `/api/v1/health` 的归属和关系，确定监听地址、认证、响应和失败状态，并在唯一契约及对应文档中同步。当前 OpenAPI 没有定义前述三条路径；运维侧的路径和端口仅为待实现的配置约定。
3. **基础设施／数据库**：提供可独立、可重复执行且失败时返回非零的迁移程序，明确迁移锁、数据库路径和 schema 版本来源，并确认与运维脚本的 SQLite 快照和 `PRAGMA user_version` 记录兼容。迁移还须明确旧版本对迁移后 schema 的兼容性；不兼容时，镜像回滚前需要按恢复流程处理数据。
4. **适配器与面板**：各自提供可运行的镜像、启动命令和进程健康接口，并确认可用 UID、探针工具、监听端口及必要配置。Compose 中的镜像、命令、`CORE_ENDPOINT`、`CONTROL_API_URL` 和端口均须由所属模块核对；其中连接地址是占位配置，不能作为已确认的跨模块协议。
5. **业务日志与插件配置**：各业务进程负责输出本节此前列出的关联字段和脱敏内容，运维 JSON 日志不能替代业务日志。插件负责人还须明确配置的权威存储、导出和恢复方式；当前脚本只备份 SQLite 数据库，不覆盖插件配置。

依赖完成后，应使用真实发布镜像及隔离数据重新验证迁移失败阻断、live/ready/details、服务启动顺序、备份恢复、指定 digest 回滚及旧版本的 schema 兼容性。

## T01b 本机传输验证

使用 proto 构建和 Core 文档中的 `--grpc-listen` 参数可启动本机桥接监听；凭据通过成对 ID/TOKEN 环境变量提供。禁止将回环明文监听当作生产 TLS 服务或 ready 端点。已有 Docker 默认 dev 构建仍不包含 gRPC，镜像和跨主机 TLS 在 T11 中集成。停止信号取消流、等待处理器退出后返回 0；绑定/凭据配置错误返回 1。

## SQLite 运行时开发验证

T05b/T10a 在打开数据库时事务化初始化 PRAGMA user_version=2，支持 v1→v2 升级；拒绝未知版本与损坏数据。T02a 可选接入进程启动；健康探针及独立迁移命令仍待实现。运行时快照包含事件/投递/会话/Action/插件，以及 T10a 命令审计和幂等记录；不包含 Host 配置等未来数据；不可宣称已覆盖完整生产备份。

## T02a 持久化接收启动

使用 proto 且 KIARABOT_ENABLE_INFRASTRUCTURE=ON 构建后，可设置 KIARABOT_RUNTIME_DB（非空文件路径，禁止 :memory: 和 file: URI）及 KIARABOT_ADAPTER_ACCOUNTS（逗号分隔的非空账号允许列表），并保留 KIARABOT_ADAPTER_ID/TOKEN 和 --grpc-listen。启动先打开 SQLite/迁移，失败退出 1；配置仅部分提供时拒绝启动。未配置数据库时仍为传输认证模式，所有业务显式拒绝。接收可用只表示 adapter_ingress_ready=true，完整 rpc_ready 仍为 false；插件入口可按 T03a 配置启用，Action 纯文本闭环已由 T04b 实现，模型和任务仍未接线。账号授权来自该配置，不能由适配器自报。
