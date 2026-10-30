# 部署与可观测性设计

T07b 真实联调环境尚未提供（用户已确认）。准备说明和不含密钥的模板见 [真实联调环境准备](../tests/acceptance/real-environment.md)。先以 event-observer 验证接收，模板不自动发送平台消息或模型请求；真实收发、模型和恢复结果仍待实际环境验收，不能用 T07a 替代。

T06c 持久化插件入口装配 TaskScheduler 并协商 host.tasks.v1；插件权限显式添加 task.schedule，定时事件订阅为 event.timer.triggered。数据库升级至 v5，包含任务/领取 token；启动在开启监听前恢复 RUNNING，恢复失败拒绝启动。一个数据库只运行一个 Core；替换进程必须先停止旧进程。任务每 100ms 检查，系统必须安装 IANA zoneinfo（Linux tzdata）；备份任务/inbox/幂等记录须保持同一快照。COMPLETED 只表示入队，排障同时检查插件投递状态。

T06b 可选配置 KIARABOT_MODEL_ENDPOINT（完整 /chat/completions URL）、KIARABOT_MODEL_API_KEY 和 KIARABOT_MODEL_NAME，三者必须同时设置且持久化插件入口已启用；部分/非法配置启动失败 INVALID_ARGUMENT。未配置时不协商 host.model.v1。插件须获授 model.chat。只允许 HTTPS 或本机 127.0.0.1 HTTP；禁用环境代理/重定向，证书校验开启，最大请求三十秒，停机可能等到该调用期限。密钥不要放命令行或入库；受管 Python 环境白名单不包含任何模型配置。模型只读探活与实际外部供应商联调仍未验证。

T06a 持久化插件入口装配 HostDispatcher；存储、可选模型和任务能力按实际配置协商，storage.namespace 必须显式列入 KIARABOT_PLUGIN_PERMISSIONS。T06a 引入 schema v4，当前版本升级为 v5，回滚使用迁移前备份。Host 存储和幂等响应随 Core 数据库一起备份，不能单独删除幂等响应后重放写入。

T04b 启用持久化插件入口时装配 ActionRouter，插件 message.send 权限仍显式配置；只声明已实现的 action.result.v1。T04b 引入 schema v3，当前 T06c 已继续升级至 v5，旧二进制不能读取新 schema，回滚必须使用迁移前备份。Python 适配器在 OUTBOX_PATH 旁维护 .actions 执行日志，备份/恢复须与事件 outbox 一起处理，禁止删除未终态记录后重放平台请求。

## T03b 受管 Python 插件

在 T03a 插件入口基础上，配置 KIARABOT_PLUGIN_EXECUTABLE（Python 可执行文件绝对路径）和 KIARABOT_PLUGIN_MANIFEST（清单绝对路径）；可选 KIARABOT_PLUGIN_PYTHONPATH 指定 SDK、生成协议和插件模块目录。Core 执行 `python -m kiarabot_sdk --manifest ...`，不经过 shell。默认启动等待 5000 ms、停止宽限 2000 ms，可分别用 KIARABOT_PLUGIN_START_TIMEOUT_MS、KIARABOT_PLUGIN_STOP_TIMEOUT_MS 配置为 10–30000 ms。配置不完整或缺少插件入口时启动失败。

子进程只接收最小 PATH、插件 token、endpoint、manifest 参数和可选 PYTHONPATH，不继承其它 Core 秘密。stdout/stderr 当前重定向到空设备；Core 仅报告启动、退出、强制回收及稳定错误。插件崩溃不退出 Core；停止对插件进程组先 TERM、后 KILL，并回收直接子进程。当前面向本机 POSIX 开发运行，生产资源沙箱、自动重启和镜像仍待后续任务。

T03a：在持久化入口配置基础上，设置 KIARABOT_PLUGIN_ID/TOKEN 及 KIARABOT_PLUGIN_PERMISSIONS（逗号分隔权限；事件插件至少 message.read），即可启用单个已授权插件连接。库级 PluginHub 支持多个策略/连接，CLI 当前只配置一个插件。没有权限配置仍只提供插件传输认证并拒绝业务。插件启动器、Action/Host 和控制服务已接线；启动日志的 rpc_ready 仍为 false，plugin_delivery_ready 只表示本机插件投递入口已启用。

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

数据库迁移由 `infrastructure/src/sqlite_runtime_store.cpp` 实现，独立命令入口为 `infrastructure/src/migrate.cpp`；运维目录只负责编排、启动、健康检查、备份和回滚。

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
/api/v1/health   认证后的组件详情
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

**当前不能作为完整部署验收**：Core 已有插件/Action 闭环、控制 API 和进程探针，T11a 提供独立迁移程序；适配器及面板镜像、生产 TLS、真实环境及隔离部署恢复仍归 T11b/T07b。进程探针为根路径 `/health/live`（不访问存储）、`/health/ready`（写事务探测）；详情为认证的 `/api/v1/health`，不提供历史 `/health/details`。唯一来源是 OpenAPI。

Compose 在 `deploy/.env` 存在时可通过可选 `env_file` 注入运行配置，也可以使用 `KIARABOT_ENV_FILE` 指定外部文件；Compose 插值变量通过启动进程环境或 Compose 的 `--env-file` 提供。环境文件和密钥不应放进仓库或构建上下文，权限应限制为部署账户可读。镜像构建不需要 Token、API Key 或 SSH key。不要把这些值放进镜像标签、命令行或日志。

```bash
# 检查配置；生产环境先设置三个真实的镜像引用（含 digest）
docker compose -f deploy/compose.staging.yml config
docker compose -f deploy/compose.prod.yml config
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh start
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh status
KIARABOT_ENVIRONMENT=staging scripts/deploy/deploy.sh stop

# 运维探针不输出响应正文；详情需要有 health.read 权限的 access token 文件
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /health/live
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /health/ready
scripts/deploy/healthcheck.sh --url http://127.0.0.1:8080 --endpoint /api/v1/health --token-file /path/to/access-token
```

`--token-file` 仅读取包含 access token 的文件，用于 `/api/v1/health`；调用方应将文件限制为当前用户可读并在使用后删除。令牌不会出现在命令参数或脚本日志中。缺少、无法读取或格式非法返回 2；服务认证拒绝或不可用返回 1。

Linux 主机可将仓库部署到 `/opt/kiarabot`，将 `deploy/systemd/kiarabot.service` 安装到 systemd，使用 `/etc/kiarabot/production.env` 指定环境变量，再运行 `systemctl daemon-reload` 和 `systemctl enable --now kiarabot.service`。该服务由 Docker 守护进程管理容器重启；停止服务会执行 Compose down，但保留数据卷。变更这些示例路径时应同步修改 unit。容器 stdout/stderr 使用 Docker 日志驱动，部署脚本输出 JSON 字段 `timestamp`、`level`、`component`、`operation`、`outcome`、`request_id`、`trace_id`、`resource_id`、`message`、`error_code`；业务进程仍须自行提供上文要求的结构化字段与脱敏，运维脚本不会将日志作为命令通道。

### 备份、迁移和回滚

`scripts/deploy/migrate.sh` 使用排他锁；数据库已存在时先用 SQLite `VACUUM INTO` 创建一致性快照，并写入 `PRAGMA user_version`、SHA-256 和创建时间。数据库不存在视为首次部署；未设置 `MIGRATION_COMMAND` 时以非零状态停止，因为必须显式指定迁移命令；仓库已提供可重复运行的 `kiarabot-migrate`，但当前默认镜像未打包该程序。部署镜像完成接线后，通过受控环境变量提供命令。确认无需迁移时可明确设置 `MIGRATION_COMMAND=true`；命令失败会以非零状态阻止后续服务。异常终止可能留下锁文件，确认无迁移进程后才能人工删除。备份保存在数据卷内，操作员还须定期复制到独立的受保护位置；本配置不包含插件配置备份。

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

1. **Core**：已提供持续进程、本机 RPC、持久化及控制服务健康探针。默认镜像未启用协议和基础设施，Compose 默认入口未启动这些服务，仍无法通过 ready 检查；生产镜像和配置接线待 T11b 验收。
2. **协议和控制 API**：T11a 已冻结根路径 live/ready 与认证的 `/api/v1/health`；部署时须提供正确监听配置及有权限的详情查询凭据。
3. **基础设施／数据库**：独立、可重复执行的迁移程序已实现；仍须在部署演练中验证与运维脚本的 SQLite 快照和 `PRAGMA user_version` 记录兼容。不兼容新 schema 的旧版本须在镜像回滚前按恢复流程处理数据。
4. **适配器与面板**：各自提供可运行的镜像、启动命令和进程健康接口，并确认可用 UID、探针工具、监听端口及必要配置。Compose 中的镜像、命令、`CORE_ENDPOINT`、`CONTROL_API_URL` 和端口均须由所属模块核对；其中连接地址是占位配置，不能作为已确认的跨模块协议。
5. **业务日志与插件配置**：各业务进程负责输出本节此前列出的关联字段和脱敏内容，运维 JSON 日志不能替代业务日志。插件负责人还须明确配置的权威存储、导出和恢复方式；当前脚本只备份 SQLite 数据库，不覆盖插件配置。

依赖完成后，应使用真实发布镜像及隔离数据重新验证迁移失败阻断、live/ready/details、服务启动顺序、备份恢复、指定 digest 回滚及旧版本的 schema 兼容性。

## T01b 本机传输验证

使用 proto 构建和 Core 文档中的 `--grpc-listen` 参数可启动本机桥接监听；凭据通过成对 ID/TOKEN 环境变量提供。禁止将回环明文监听当作生产 TLS 服务或 ready 端点。已有 Docker 默认 dev 构建仍不包含 gRPC，镜像和跨主机 TLS 在 T11 中集成。停止信号取消流、等待处理器退出后返回 0；绑定/凭据配置错误返回 1。

## SQLite 运行时开发验证

当前数据库初始化为 PRAGMA user_version=5，支持 v1–v4 事务升级并拒绝未知版本与损坏数据。T02a 已可选接入进程启动，健康探针及独立迁移命令也已实现。运行时快照包含事件、投递、会话、Action、插件、命令审计、Host 命名空间和任务；完整生产备份与恢复仍须演练。

## T02a 持久化接收启动

使用 proto 且 KIARABOT_ENABLE_INFRASTRUCTURE=ON 构建后，可设置 KIARABOT_RUNTIME_DB（非空文件路径，禁止 :memory: 和 file: URI）及 KIARABOT_ADAPTER_ACCOUNTS（逗号分隔的非空账号允许列表），并保留 KIARABOT_ADAPTER_ID/TOKEN 和 --grpc-listen。启动先打开 SQLite/迁移，失败退出 1；配置仅部分提供时拒绝启动。未配置数据库时仍为传输认证模式，所有业务显式拒绝。接收可用只表示 adapter_ingress_ready=true，完整 rpc_ready 仍为 false；插件入口可按 T03a 配置启用，Action 纯文本闭环已由 T04b 实现，模型和任务已接线，具体能力取决于配置。账号授权来自该配置，不能由适配器自报。

## 控制 API（T08b）

proto + infrastructure 构建可通过 `KIARABOT_CONTROL_CONFIG` 指定私有 JSON 文件，在同一 Core 进程启动 REST 服务。文件须为当前用户所有的普通文件且权限 0600，不得提交。字段包括 `listen`（127.0.0.1:端口）、`encryption_key`（32 字节的 hex）、`users`（id、username、Argon2id password_hash、role）及可选 `plugins` 公共清单。角色为 admin/operator/viewer；用户名 ASCII 且不区分大小写。密钥丢失或变化会导致已有加密状态无法读取，必须与数据库备份一起安全保管。

构建提供 `kiarabot-control-credentials`：默认从 stdin 读一行密码并输出 Argon2id 哈希；`--key` 输出随机控制密钥。只在受保护终端/管道中运行，不在命令行传密码。不配置控制文件时原 gRPC 启动方式保持不变。控制服务要求现有持久化配置，不接受临时内存库作为部署状态。监听占用、无效配置或无法解密状态使启动失败。

请求体上限 64 KiB、HTTP 头上限 16 KiB、连接有界；登录每用户名/分钟限额，超限返回 429/Retry-After。服务绑定回环地址，浏览器通过同源代理连接；不启用宽泛 CORS，不信任代理头提供的身份。

## 结构化运行日志（T10b）

持久化运行模式通过日志存储装饰器写入有界结构化日志缓冲，sink 也支持可选 JSON 输出（daemon 默认不写阻塞 stdout），字段含 id、occurred_at、level、component、operation、outcome、resource、request_id、trace_id。禁止输出消息正文、模型请求、配置值、密码或令牌；标识限制为安全字符。sink 写失败不影响已提交业务，并累计失败计数。`GET /logs` 只对 operator/admin 返回十分钟内有界内存记录，日志不代替 `/audit` 的持久审计，也不是无限历史查询。


## T11a 独立迁移入口

`kiarabot-migrate DATABASE_PATH` 调用同一 SQLite schema v5 迁移实现；缺参数/非法路径退出 2，未知 schema、损坏快照、依赖故障或锁等待超时退出 1，成功 0。迁移依赖 SQLite 的 BEGIN IMMEDIATE 互斥，不创建可能遗留的文本锁文件；冲突最多等待三十秒并重试整个未提交迁移，其他错误不重试。重复运行安全，成功只表示数据库就绪，不宣称其他服务 ready。daemon 启动沿用相同迁移逻辑，坏 schema/无法解密控制状态禁止监听控制端口。

运行 `build/proto/infrastructure/kiarabot-migrate /path/to/runtime.db`；生产迁移前仍须备份，旧二进制回滚不得直接读取新 schema。该命令不绕过端口修改业务记录。

## 配置应用确认

启用私有 KIARABOT_CONTROL_CONFIG 且配置插件桥接身份/权限后，支持 plugin.config.v1 的 SDK 自动接收配置。PUT 成功后轮询 GET /plugins/{plugin_id}/config 的 delivery；APPLIED 才表示当前运行连接完成回调。REJECTED 需修复配置提交新版本或重连，UNAVAILABLE 需检查插件连接/停用/能力协商。Core 重启后重新确认，不能用旧进程的 applied_version 作健康证明。不新增配置项、SQLite schema 或真实环境验收要求。
