# 运维（kiarabot-operations）覆盖与缺口分析

> 分析对象：main@8caecb1（`git log -1` = `8caecb1 fix: repair CI pipeline and close verification gaps`）。
> 以静态分析为主：本机无 `docker`/`cmake`/`node`/`protoc`，**未构建镜像、未执行 `docker compose`、未跑 CTest/CI**。
> 本机实测补充（仅用 python3.9/bash 3.2/sqlite3 3.51/shasum，未触碰仓库文件）：`scripts/deploy/tests/` 两个套件 7 个用例在本机全部通过；另做了 8 项失败路径探针（见 §3、§8），这些是 macOS+bash 3.2 下的补充证据，不能替代 Linux/Ubuntu+真实 Docker 验收。

## 1. 范围与规模

- 目录/文件（`git ls-files` 实际跟踪）：`Dockerfile`(31)、`.dockerignore`(20)、`deploy/compose.staging.yml`(104)、`deploy/compose.prod.yml`(104)、`deploy/systemd/kiarabot.service`(19)、`scripts/deploy/{deploy,migrate,backup,healthcheck,log}.sh`(67/35/77/61/27)、`scripts/deploy/tests/{deployment,healthcheck}_test.py`(116/69)。
- 规模：12 文件 / 730 行（测试 185 行，实现 545 行）。
- 关键入口与代码地图：
  - 镜像构建：`Dockerfile:2-12`（build 阶段）→ `:14-31`（runtime 阶段，`USER 10001:10001` `:29`，`CMD ["/usr/local/bin/kiarabot-core"]` `:31`）；构建上下文白名单 `.dockerignore:2-8`。
  - 编排：`deploy/compose.staging.yml` / `compose.prod.yml` 均有 4 服务 `migrate`(:4-20)、`core`(:22-48)、`adapter`(:50-73)、`panel`(:75-100)，依赖链 `migrate:completed → core:healthy → adapter:healthy → panel:healthy`（`:36-38,:61-63,:88-90`）。
  - 状态机：`scripts/deploy/deploy.sh:30-66`；迁移编排 `migrate.sh:8-34`；备份/恢复 `backup.sh:36-77`；探针 `healthcheck.sh:47-61`；JSON 日志 `log.sh:14-27`。
  - 主机编排：`deploy/systemd/kiarabot.service:1-19`（`Type=oneshot` `:8`，`ExecStart=... deploy.sh start` `:13`，`ExecStop=... deploy.sh stop` `:14`）。
- 对应设计文档：`doc/08-operations.md`(220)、`doc/12-ci-cd.md`(343，部署段 :174-279)、`doc/16-development-tasks.md:334-361`（T11a/T11b）、`doc/17-account-ban-risk.md:82-87,125-129`、`doc/18-architecture-baseline.md:339-342,407`（受保护，只读引用）。

## 2. 模块职责（依据）

- `doc/08:21-23`：运维层负责“让各模块可启动、可停止、可检查、可升级和可恢复”；`:53-59` 定义启动顺序（迁移→Core→适配器→NapCat→面板）；`:61-80` 定义每进程 `/health/live`、`/health/ready`、认证 `/api/v1/health` 及语义；`:102-111` 要求迁移前备份、失败升级可回滚、备份含 schema 版本与校验值；`:120-126` 验收（独立重启、断线恢复、live/ready 区分、trace 关联、备份方案）。
- `doc/12:7-23`：CD 责任 = 可追踪产物 + GHCR 镜像 + staging 自动部署/生产审批 + 迁移/健康检查/冒烟/失败回滚 + 部署记录；`:174-238` staging/production 流程与 Compose 顺序；`:240-279` 密钥与回滚规则；`:320-331` 验收标准。
- `doc/17:82-87`（账号/网络运维规范：小号、固定出口 IP、NapCat WebUI/OneBot 只监听本机、必须 token、禁止公网暴露）；`:125-129`（监控指标、账号冻结演练、错误分类验收）。`doc/17:12` 结论第 4 项明确要求“第四项写入运维文档”，`:109` 要求实现配额/急停时同步 `doc/08` 的账号与网络规范。
- `doc/16:334-347` T11a（健康契约/迁移命令，已标“本机自动验收”）；`:348-361` T11b（镜像、部署与恢复演练，5 条验收全部未勾选）。
- 契约来源：`api/openapi.yaml:44-79`（根路径 `/health/live`、`/health/ready`，`servers: /`），实现映射在 `control/api/src/http_mapper.cpp:143-152`；启动参数/环境变量以 `core/app/main.cpp` 为准。

## 3. 已实现能力（逐条带证据）

| # | 能力 | 证据（file:line） | 对应测试 / 本机验证 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 多阶段构建 + 非 root 运行（UID/GID 10001，数据目录归属 10001，`curl`/`sqlite3` 装入 runtime 阶段） | `Dockerfile:2-12,14-22,29-31` | 无测试；CI 无 docker job（`ci.yml:26-260`） | 部分（构建目标是 dev Core，见缺口 P0-1） |
| 2 | 构建上下文白名单，`.env`/密钥/SSH key 不进上下文 | `.dockerignore:1-20` | 无自动化校验 | 完整（但白名单同时排除了构建可运行镜像所需的 `proto/`、`infrastructure/`） |
| 3 | staging/prod 四服务拓扑、依赖条件、`restart`、`init`、`cap_drop:[ALL]`、`no-new-privileges`、仅回环发布端口 | `compose.staging.yml:33,36-38,45-48,70-73,87,97-100`（prod 同结构 `compose.prod.yml:33,36-38,45-48,70-73,87,97-100`） | 无 compose 校验；本机用 ruby YAML 解析两文件成功（结构合法，非 compose-spec 校验） | 部分（服务镜像/命令/端点不可用，见 P0-2/3/5） |
| 4 | prod 镜像以整段 `@sha256:0…0` 占位，staging 用 `:staging` 标签，可用 `KIARABOT_*` 覆盖 | `compose.prod.yml:5,23,51,76`；`compose.staging.yml:5,23,51,76`；`doc/08:134` | 无 | 部分（占位是“不可拉取”值，从未有真实 digest 注入流程） |
| 5 | 迁移→Core→适配器→面板的启动顺序由 `depends_on` 条件编码 | `compose.staging.yml:36-38,61-63,88-90`（prod 同） | 无 | 部分（被 P0-1/2/5 阻断，实际无法达成 ready） |
| 6 | `deploy.sh` 状态机：`start/stop/status/upgrade/rollback`；digest 格式校验；upgrade 先停栈→新 digest 跑迁移→按 core/adapter/panel 逐个 `up --wait`；失败不自动回旧镜像并保持停机 | `deploy.sh:8-29,30-66` | `deployment_test.py:17-53` 覆盖两条 upgrade 失败路径；本机用 docker stub 实测 start/upgrade/rollback 调用序列与 digest 注入符合预期 | 部分（成功路径不可用，见 P0-1/2/3/6） |
| 7 | `migrate.sh`：`noclobber` 排他锁 + `trap` 清理；DB 存在则先调 `backup.sh` 快照；`MIGRATION_COMMAND` 缺失/失败时非零阻断 | `migrate.sh:8-34` | `deployment_test.py:83-92`（失败释放锁）；本机实测缺命令返回 1（`MIGRATION_NOT_CONFIGURED`）、`kill -9` 后锁残留 | 部分（任意命令 + 无默认命令 + 无陈旧锁处理，见 P1-16/P1-17） |
| 8 | `backup.sh` 一致性快照与自描述元数据：`sqlite3 ... VACUUM INTO`、`umask 077`、SHA-256（sha256sum/shasum 二选一）、`PRAGMA user_version`、`created_at` | `backup.sh:27-60` | `deployment_test.py:55-112`（含篡改校验失败、shasum 回退）；本机实测通过 | 完整（就单机快照能力而言；策略层缺口见 P1-7） |
| 9 | `backup.sh restore`：元数据/文件存在校验、SHA-256 比对、拒绝 `-wal/-shm` 存在的目标、临时文件 `mv` 原子替换 | `backup.sh:61-75` | `deployment_test.py:55-81`；本机实测 sidecar 存在时返回 `DATABASE_BUSY` | 部分（sidecar 守卫对当前非 WAL 存储近乎恒不触发，见 P1-18） |
| 10 | `healthcheck.sh`：仅允许 3 个白名单端点；`/api/v1/health` 用 `--token-file`（不回显 token，token 格式校验）；conn/max 超时；退出码 0/1/2 与 `doc/08:156` 一致 | `healthcheck.sh:8-61` | `healthcheck_test.py:30-69`（公开探针、旧路径拒绝、认证成功/失败/坏 token）；本机实测通过 | 完整（脚本能力完整；未被部署流程调用，见 P1-11） |
| 11 | `log.sh` 结构化 JSON：`timestamp/level/component/operation/outcome/request_id/trace_id/resource_id/message/error_code`，含 JSON 转义 | `log.sh:4-27` | 无专门测试；本机实测输出可被 JSON 解析（人工检查） | 完整（字段与 `doc/08:158` 部署脚本字段一致） |
| 12 | 部署脚本测试已接入 CTest：`deploy_healthcheck`、`deploy_deployment`（TIMEOUT 30） | `core/CMakeLists.txt:44,49-53` | `ctest --preset dev` 会执行；本机直接跑两个 .py：7/7 通过 | 部分（无 compose/Dockerfile/systemd/start 成功路径/rollback 成功测试，见 P1-15） |
| 13 | 失效通知：脚本失败以非零退出 + 稳定 `error_code`，便于 systemd/journald 捕获 | `deploy.sh:21,50,62`；`migrate.sh:15,29,33`；`backup.sh:32,38,48,62,66` | 失败路径测试/探针见上 | 完整（仅“可观测失败”层面） |

## 4. 缺口清单

> 严重度口径按 `reports/_brief.md` §3：P0=阻断已声明主流程/验收项/安全底线；P1=功能域缺失；P2=体验/文档/增强。**多个 P0 是 `doc/16:600` 已自认的“部署骨架不可运行”，本报告的价值在于补全其未列出的阻断点。**

| # | 缺口 | 类型 | 严重度 | 证据（file:line / 文档行） | 影响 |
| --- | --- | --- | --- | --- | --- |
| P0-1 | **Dockerfile 实际构建的是 dev Core（无 proto、无 infrastructure）**：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` 未传任何 KiaraBot 选项，默认 `KIARABOT_GENERATE_PROTO=OFF`、`KIARABOT_ENABLE_INFRASTRUCTURE=OFF`；`--target kiarabot-core` 产物不含 gRPC、控制 API、SQLite | 未接线（构建目标） | **P0** | `Dockerfile:11-12,24`；`CMakeLists.txt:4,21,120-122`；`core/app/main.cpp:97,338-341`；健康端点只在控制服务器：`control/api/src/http_mapper.cpp:143-152`；`doc/08:136,138`；`doc/16:600` | 镜像内二进制既无 `/health/live`、`/health/ready`，也无持久化/控制 API；compose 的 core 健康检查必然失败，后续 adapter/panel 永不启动；“按 digest 回滚”回滚的仍是一个不可用进程 |
| P0-2 | **compose 引用的 adapter/panel 镜像在仓库中不存在**：无 `adapters/` 或 `panel/` 的 Dockerfile（全仓仅根 `Dockerfile`）；adapter 声明 `tool.uv package = false` 且无 `[project.scripts]`，面板只有 Vite 构建（无 `kiarabot-panel` 服务端），但 compose 默认命令分别是 `kiarabot-adapter-nonebot`、`kiarabot-panel` | 未实现 | **P0** | `compose.staging.yml:51-53,76-78`；`compose.prod.yml:51-53,76-78`；`adapters/nonebot/pyproject.toml:1-17`；`panel/package.json:6-13`；`find . -name 'Dockerfile*'` 仅 `./Dockerfile` | staging/prod `docker compose up` 在 adapter/panel 必失败（镜像拉不到或命令不存在），4 服务栈无法成形 |
| P0-3 | **compose 未把 Core 配置成可服务模式**：`core.command` 无 `--grpc-listen`；未注入 `KIARABOT_RUNTIME_DB`、`KIARABOT_CONTROL_CONFIG`；而持久化/控制/健康只在 `--grpc-listen` 非空且控制配置存在时启动；`HEALTH_PORT` 全仓无读取者；compose 也没有控制配置文件的挂载 | 未接线 | **P0** | `compose.staging.yml:25,29-31,34-35`；`compose.prod.yml` 同；`core/app/main.cpp:105-110,223-271,382-392`；`HEALTH_PORT` 仅出现在 compose/doc（grep 无代码引用）；`doc/08:138` | 即使换成 proto+infra 镜像，容器仍只进入“等待信号”的最小进程（`main.cpp:341`），既不迁移、也不监听；健康检查无对象 |
| P0-4 | **跨容器网络在现行绑定策略下不可达**：Core 的 gRPC 桥接只接受 `127.0.0.1:` 前缀（`BridgeServer::start`），控制 HTTP 也只接受 `127.0.0.1:`（`HttpServer::start`）；compose 却让 adapter/panel 通过服务名访问 `http://core:8080`，并把 core 端口发布到宿主 `127.0.0.1:18080/8080`；Docker 端口映射转发到容器 eth0，不会命中容器内 loopback 监听 | 契约不一致（部署拓扑 vs Core 绑定语义） | **P0** | `core/src/runtime/bridge_server.cpp:527-529`；`control/api/src/http_server.cpp:296`；`compose.staging.yml:33,59,84`；`compose.prod.yml:33,59,84`；`doc/08:189,205` | 多容器 Compose 拓扑与“只监听回环”安全策略直接冲突；需要改用共享网络命名空间/host 网络或在 Core 增加受控监听地址（属跨模块决策），当前配置无法工作 |
| P0-5 | **adapter/panel 健康服务未实现**：`/health/ready` 只存在于控制 API；adapter 源码无 HTTP 健康服务，panel 是纯静态前端；compose 对 8081/8082 的 `curl /health/ready` 健康检查无实现 | 未实现 | **P0** | `compose.staging.yml:60,64-69,85,91-96`；`compose.prod.yml` 同；`adapters/nonebot/src/kiarabot_adapter_nonebot/`（无 health 模块）；`panel/package.json:6-13`；`doc/16:600` | 即使 adapter/panel 镜像存在，容器也不会变 healthy；`depends_on: service_healthy` 使面板永远起不来 |
| P0-6 | **迁移步骤默认必然失败，且镜像内没有迁移程序**：compose 把 `MIGRATION_COMMAND` 默认设为空 → `migrate.sh` 直接 `MIGRATION_NOT_CONFIGURED` 退出 1；仓库有 `kiarabot-migrate` 目标但 Dockerfile 只构建/复制 `kiarabot-core`；文档建议用 `MIGRATION_COMMAND=true` 旁路，此时迁移实际由 Core 打开数据库时自动完成（v1–v4→v5） | 未接线 / 半实现 | **P0** | `compose.staging.yml:15`；`compose.prod.yml:15`；`migrate.sh:25-34`；`Dockerfile:12,24`；`infrastructure/CMakeLists.txt:89-90`；`infrastructure/src/sqlite_runtime_store.cpp:138-195`；`doc/08:162,214-216` | `migrate` 服务失败 → `core` 的 `service_completed_successfully` 不满足 → 全栈不启动；用 `true` 旁路则等于放弃“迁移前备份”这一运维保证（备份虽在 `migrate.sh:19-23` 先行，但语义被弱化） |
| P1-1 | **compose 环境变量与各模块实际读取的名字不匹配**：adapter 读 `CORE_GRPC_ENDPOINT/CORE_AUTH_TOKEN/ADAPTER_ID/ACCOUNT_ID/ONEBOT_WS_URL/ONEBOT_ACCESS_TOKEN/OUTBOX_PATH`，compose 只给 `CORE_ENDPOINT`；panel 在构建期读取 `VITE_API_MODE/VITE_API_BASE/VITE_WS_URL`（默认 mock），compose 给运行期 `CONTROL_API_URL`（面板不读） | 契约不一致 | P1 | `compose.staging.yml:57-60,82-85`；`adapters/nonebot/src/kiarabot_adapter_nonebot/__main__.py:21-27,45,56`；`adapters/nonebot/README.md:14`；`panel/src/main.tsx:7-11`；`doc/08:182`（仅笼统称“占位配置”） | adapter 启动即 KeyError；面板部署后默认显示 mock 数据，既连不上 Core 也不暴露真实状态 |
| P1-2 | **adapter 无数据卷**：outbox 与 `.actions` 执行日志写在容器可写层，`down/up`、重建、升级即丢失；而 `doc/08:11` 要求 outbox/action 日志与事件一起备份、禁止删除未终态记录后重放 | 未接线 | P1 | `compose.staging.yml:50-73`（无 `volumes`）；`compose.prod.yml:50-73`；`doc/08:11`；`backup.sh` 仅备份 SQLite | 幂等/去重依据丢失，升级或重启后可能重复向平台发送 Action（封号风险放大） |
| P1-3 | **面板被错误地置于 adapter healthy 之后启动**，且 NapCat 不在 compose 中：设计明确“Panel ready 依赖 API 可达，不要求 QQ 在线”，实际 `panel.depends_on.adapter: service_healthy`；同时 `doc/08:44-49` 的进程清单含 NapCat，compose 无该服务 | 契约不一致 | P1 | `compose.staging.yml:88-90`（prod 同）；`doc/08:77,44-49`；`adapters/nonebot/README.md:14`（需要 `ONEBOT_WS_URL` 指向 NapCat） | QQ 不在线/适配器未注册时，面板也不可用；NapCat 需人工在 Compose 之外单独部署，没有编排/健康/网络约定 |
| P1-4 | **完全无 TLS/反向代理**：compose 无 nginx/caddy/traefik 服务、无证书挂载；控制 HTTP 只监听容器内回环，仅能靠宿主回环端口+SSH 隧道访问 | 未实现 | P1 | `deploy/` 文件清单；`compose.*.yml` 全文无 TLS/proxy；`control/api/src/http_server.cpp:296`；`doc/08:136` 自认“生产 TLS 仍归 T11b” | 生产单机部署无法提供 HTTPS/WSS，浏览器同源代理前提不成立；与 `doc/12:174-238` 的 staging/production 部署承诺不符 |
| P1-5 | **无日志驱动/轮转/集中收集**：compose 未配置 `logging:`（`max-size/max-file`），默认 json-file 无上限；无 vector/promtail/fluentd/logrotate；`doc/08:100` 把 JSON sink、轮转、集中收集、保留期、告警列为本层职责但无任何实现 | 未实现 | P1 | `compose.staging.yml`、`compose.prod.yml`（无 `logging`）；`doc/08:82-100`；`.github`/`scripts` 无相关配置 | 长期运行会因容器日志撑满磁盘；失败/回滚无法按 trace_id 集中检索；宕机后现场丢失 |
| P1-6 | **无指标/告警**：无 Prometheus/OTel exporter、无告警规则、无部署失败/未 ready/备份失败告警；`doc/12:313-318`（第 5 步）与 `doc/17:125-129` 要求的指标全部未落地 | 未实现 | P1 | `deploy/`、`scripts/deploy/` 无任何 metrics/alerts 配置；`doc/08:100`；`doc/12:313-318`；`doc/17:127` | 失败只能靠人工看 journald；T11b“未 ready 可恢复”缺少触发面 |
| P1-7 | **备份策略层缺失**：备份写在数据卷内（`/var/lib/kiarabot/backups`，与 DB 同卷），无保留/轮转/清理、无异地复制、无备份完整性校验（`restore` 只比 sha256，不做 `PRAGMA integrity_check`）、无定时任务；`doc/08:162` 只说“操作员还须定期复制” | 半实现 | P1 | `migrate.sh:10-11,19-23`；`backup.sh:41-60,61-75`；`compose.staging.yml:14,17,35`；`doc/08:162`；`doc/16:359`（T11b“备份校验恢复”未勾选） | 宿主/卷损坏时备份与主数据同归于尽；备份集无限增长；无法证明备份可恢复 |
| P1-8 | **备份/恢复演练、schema 回滚、升级演练全部只有脚本没有演练记录**：仓库无演练脚本/记录/隔离环境说明；T11b 5 条验收（真实镜像、启动关闭顺序、备份校验恢复、回滚前 schema、隔离 staging）全未勾选 | 未验证 | P1 | `doc/16:348-361`；`doc/17:129`；`tests/acceptance/` 无部署/恢复入口（`grep deploy tests/acceptance/run.sh` 无命中） | “备份可用/可回滚”只是代码意图，没有一次端到端证据；与 `doc/08:185` 要求的“隔离数据重新验证”不符 |
| P1-9 | **`rollback` 无 schema 兼容性检查/工具**：脚本只按 digest 重启旧镜像，不读 schema、不比对、不提示恢复备份；`doc/08:173`、`doc/12:259-279` 要求“确认旧版本与当前 schema 兼容，否则显式恢复迁移前备份”，无任何辅助命令 | 未实现 | P1 | `deploy.sh:55-65`；`doc/08:173`；`doc/12:270-279`；`doc/16:360` | 旧二进制遇新 schema 会在 `SqliteRuntimeStore::open` 直接失败（`sqlite_runtime_store.cpp:200-202`），但运维没有机械可执行的“先验证再回滚”步骤，易误操作 |
| P1-10 | **升级窗口必然停机且无信号/超时保护**：`upgrade` 先 `stop panel adapter core` 再迁移再启动，全窗口服务不可用；脚本无 `trap`（仅分支 `|| return 1`）；systemd `TimeoutStartSec=300` 到时 SIGTERM 会杀掉 `deploy.sh` 而留下已停/半启栈 | 半实现 | P1 | `deploy.sh:42-54`；`deploy/systemd/kiarabot.service:15`；`doc/08:173` | 不满足“失败升级可回滚/可恢复”的自动化要求；超时或 Ctrl-C 后需要人工判断栈状态；一次升级 = 一次计划内宕机 |
| P1-11 | **部署流程未使用 `healthcheck.sh`，也没有部署后检查/冒烟**：staging 清单（数据库迁移→Core live→Core ready→适配器注册→OpenAPI health→最小 smoke）只存在于文档，`deploy.sh` 只依赖 compose `--wait`；无 smoke 脚本、无部署后 live/ready 双探针 | 未接线 | P1 | `deploy.sh:23-34`（仅 compose up --wait）；`healthcheck.sh` 仅被测试/doc 引用（`grep` 无部署调用）；`doc/12:176-187` | 即使栈能启动，也没有“部署完成”的独立判定；live/ready 语义未被部署流程验证 |
| P1-12 | **部署记录/可追溯性缺失**：无记录 image digest、commit、schema 版本、配置版本的文件/命令；无“上次成功版本”状态，`rollback` 只能靠操作员手抄/传参 digest | 未实现 | P1 | `deploy.sh:20-22,42-65`；`doc/12:7-23,240-258,320-331` | 与“可追踪部署/失败可回滚”责任直接冲突；`doc/12:260-266` 要求的五项发布前保留物无载体 |
| P1-13 | **CI 无镜像构建/推送/部署 job，且自动触发已关闭**：`ci.yml` 仅 dev/proto 构建与检查；部署脚本测试虽进 CTest，但只在手工 `workflow_dispatch` 运行；无 GHCR/环境审批/staging 部署 | 未接线 | P1 | `ci.yml:7-17,26-260`；`doc/12:144`（自认“待实施”）；`doc/16:599,612` | 镜像永远不会由流水线产出/校验；部署脚本回归只能靠手动 CI 或本机；`doc/12:320-331` 验收多数不成立 |
| P1-14 | **控制配置/密钥无注入通道**：`KIARABOT_CONTROL_CONFIG` 要求属主 0600 的普通文件且必须是运行用户（容器内 10001）所有，但 compose 无挂载/卷；只有可选 `.env`（`required:false`）。systemd 的 `EnvironmentFile` 只进 `deploy.sh` 进程用于 compose 插值，**不会进容器**；`doc/08:158` 的“使用 `/etc/kiarabot/production.env` 指定环境变量”易被误读为容器配置 | 未接线 | P1 | `control/api/src/configuration.cpp:10-25`；`core/app/main.cpp:105,223-271`；`compose.staging.yml:8-10`；`deploy/systemd/kiarabot.service:12`；`doc/08:158,199-203` | 控制 API 永远拿不到合法配置；运维以为改 `/etc/kiarabot/production.env` 就改了容器配置，实际容器读的是 `deploy/.env` |
| P1-15 | **部署测试覆盖有明确盲区**：只有 upgrade 两条失败路径 + backup/restore + migrate 失败；缺 `deploy.sh start` 成功路径（真实 compose 语义）、`rollback` 成功路径、compose 文件校验、Dockerfile 构建、systemd unit、`healthcheck.sh` 对真实 Core 的探针、迁移成功 + 幂等、备份在并发写下的快照一致性 | 测试缺失 | P1 | `deployment_test.py:17-113`；`healthcheck_test.py:30-69`；`core/CMakeLists.txt:49-53` | “脚本被测试覆盖”只对失败路径成立；部署成功与否无法由测试证明 |
| P1-16 | **`MIGRATION_COMMAND` 是任意 shell 执行且无审计**：`bash -c "$MIGRATION_COMMAND"` 在数据卷内执行任意脚本；来源可为 `.env`；`true` 旁路被文档认可 | 半实现 | P1 | `migrate.sh:25-31`；`doc/08:162`；`sqlite_runtime_store.cpp:138-195` | 泄露/误配的 `.env` 可在数据卷内执行任意操作（含删除备份），而运维层只有 JSON 日志、无命令白名单/审计 |
| P1-17 | **陈旧迁移锁无处理工具**：`kill -9`/OOM/宿主断电会残留 `migrate.lock`，之后所有迁移与升级永久失败，脚本无 TTL/心跳/`--force`；本机已实测该行为（返回 `MIGRATION_LOCKED`），`doc/08:162` 只叫人手工删，且未给 `pgrep` 类核验命令 | 半实现 | P1 | `migrate.sh:8-17,33-34`；本机探针：SIGKILL 后锁残留、二次运行退出 1；`doc/08:162` | 一次硬中断即可让部署流水线卡死到人工介入；无“确认无迁移进程”的受支持工具 |
| P1-18 | **restore 的“已停止所有 DB 使用者”守卫不可靠**：只检查 `-wal/-shm` 是否存在；当前存储使用回滚日志（`PRAGMA synchronous=FULL`，未设 WAL），正常情况下根本不会产生这两个 sidecar，因此 Core 仍在运行时 `restore` 也会通过检查，用 `cp+mv` 覆盖活跃数据库 | 半实现 | P1 | `backup.sh:63,68-73`；`infrastructure/src/sqlite_runtime_store.cpp:138-144`；`doc/08:167-170` | 可能用旧快照覆盖运行中的 DB，导致数据丢失或 Core 状态与磁盘不一致；守卫给出虚假安全感 |
| P1-19 | **无账号/网络/暴露面运维规范（doc/17 强制第四项）**：`doc/08` 没有任何 NapCat WebUI/OneBot 只监听本机、必须 token、固定出口 IP、单账号、禁公网暴露等条目；compose 也不包含 NapCat 网络策略；无速率/急停/封禁风控相关的运维配置 | 未实现 | P1 | `doc/17:66-70,82-87,125-129`（要求写入运维文档）；`deploy/` 无相关配置；`doc/08:41-49` 仅列进程 | 2025-09 公网 OneBot 无 token 被利用导致封号的同类风险在部署层无任何约束/检查清单 |
| P2-1 | systemd unit 偏弱：默认 root 运行、无 `User=`/`Group=`、无 `Restart=`/`ExecReload=`、无 `ProtectSystem`/`PrivateTmp`/`NoNewPrivileges`/日志速率限制；`Type=oneshot`+`RemainAfterExit` 使容器死掉后 unit 仍显示 active，systemd 不感知；升级无法通过 systemd 触发（只有 start/stop） | 半实现 | P2 | `deploy/systemd/kiarabot.service:7-19`；`deploy.sh:30-66` | 主机侧无法用 systemd 观察/恢复容器；升级必然绕过 systemd 手工跑脚本 |
| P2-2 | 镜像缺少时区数据显式安装：任务调度依赖 `absl::LoadTimeZone` 读取 `/usr/share/zoneinfo`（`doc/08:5` 要求系统安装 tzdata），runtime 阶段只装 `ca-certificates curl sqlite3`；基础镜像是否自带 tzdata 未核实 | 未验证 | P2 | `Dockerfile:17-19`；`infrastructure/src/time_zone_resolver.cpp:28`；`doc/08:5` | 镜像内建定时任务可能全部返回 INVALID_ARGUMENT（时区解析失败） |
| P2-3 | 镜像不含 Python：受管 Python 插件（`KIARABOT_PLUGIN_EXECUTABLE`）在容器内无法运行；`doc/16:378` 明确“运行镜像不加入 Python”，属有意取舍，但与 T03b 的生产运行目标冲突 | 半实现 | P2 | `Dockerfile:14-31`；`core/app/main.cpp:299-336`；`doc/16:378`；`doc/08:17` | 容器部署只能跑不带插件的 Core；插件落地方案缺失 |
| P2-4 | 镜像内混入主机专用脚本：`COPY scripts/deploy/ /usr/local/bin/` 把需要宿主 `docker` CLI 的 `deploy.sh`（及 `healthcheck.sh`）装进镜像，容器内无法执行 | 半实现 | P2 | `Dockerfile:25-27`；`deploy.sh:16` | 镜像体积/攻击面增加，运维误在容器内执行会得到 `docker: not found` |
| P2-5 | Compose/Docker 最低版本与主机依赖未文档化：`env_file` 的 `path/required` 长语法需 Compose ≥2.24，`name:`、`--wait`、`--no-deps` 亦依赖较新 plugin；`doc/08:142-158` 示例未给版本要求；`docker`/`compose` 插件、`curl`、`sha256sum|shasum`、`sqlite3` 等主机前置条件无清单 | 文档缺失 | P2 | `compose.staging.yml:1,8-10`；`deploy.sh:16`；`healthcheck.sh:48`；`backup.sh:27-34` | 在老版本 Compose 上 `config`/`up` 直接报错，排障成本高；不同主机行为不一致 |
| P2-6 | 容器安全加固不完整：无 `read_only`、`tmpfs`、`pids_limit`、`mem_limit`/`cpus`、`ulimits`、独立网络；无 docker secrets；`migrate` 与 `core` 用同一数据卷且迁移脚本可删备份 | 半实现 | P2 | `compose.staging.yml`/`compose.prod.yml` 全文；`migrate.sh:25-31` | 资源失控/数据误删时缺少隔离；单机环境可接受但不应视为生产加固 |
| P2-7 | staging 使用可变 `:staging` 标签而非 commit SHA（`doc/12:82-86` 的标签策略是 `git-<sha>`/语义化版本） | 契约不一致 | P2 | `compose.staging.yml:5,23,51,76`；`doc/12:82-86` | staging 部署不可复现，无法证明“部署的到底是哪个提交” |

## 5. 与文档/台账声明的不一致

| 声明（文档/台账） | 实际（代码） | 证据 |
| --- | --- | --- |
| `doc/08:27-38` 目录清单：`deploy/` 下应有 `compose.dev.yml`、`docker/`；`scripts/` 下应有 `check-contracts.sh`、`check-boundaries.sh` | 实际 `deploy/` 只有 `compose.staging.yml`、`compose.prod.yml`、`systemd/`（无 `compose.dev.yml`、无 `docker/`）；`scripts/` 实际为 `check-toolchain.sh`、`ci/`、`deploy/`、`quality/`，边界检查是 `scripts/quality/check-boundaries.py`，契约检查在 `proto/tests/check_contracts.sh` 与 `scripts/ci/check-descriptor.sh`（无 `scripts/check-contracts.sh`） | `git ls-files deploy scripts`；`ls deploy scripts`；`ls scripts/quality` |
| `doc/08:44-49` 进程清单含 `napcat`，`doc/08:53-59` 启动顺序第 4 步是 NapCat 连接适配器 | compose 只有 migrate/core/adapter/panel，无 NapCat 服务、无其网络/健康约定；compose 也不设置 `ONEBOT_WS_URL` | `compose.staging.yml:3-104`；`adapters/nonebot/README.md:14` |
| `doc/08:136` “Core 已有…进程探针”（未加构建前提）；`doc/12:68,140` 称“生产 CD 还需要实现真实进程、迁移命令和 `/health/live`、`/health/ready` 接口”“`api/openapi.yaml` 仅定义 `/api/v1/health`” | 探针确实已在 **proto+infrastructure** 构建中实现并冻结进 OpenAPI；但默认镜像/compose 不启用；`doc/12:140` 关于 OpenAPI 的描述已过时 | `api/openapi.yaml:44-79`；`control/api/src/http_mapper.cpp:143-152`；`CMakeLists.txt:4,23,120`；`Dockerfile:11-12` |
| `doc/08:134` “Compose 的四个服务依次等待迁移成功、Core ready 和适配器 ready，面板也有独立 ready 检查”“适配器、面板镜像必须自带 `curl`…”（现在时陈述） | 依赖条件与 healthcheck 声明本身与 compose 一致，但端点/镜像不存在，`doc/08:138` 随即自我否定；该段若脱离 `:138` 阅读会高估可用性 | `compose.staging.yml:36-96`；`doc/08:134,138`；`doc/16:600` |
| `doc/08:158` “使用 `/etc/kiarabot/production.env` 指定环境变量” | 该文件经 systemd `EnvironmentFile` 只进入 `deploy.sh` 进程，供 compose 插值；容器环境来自 compose 的可选 `env_file`（默认 `deploy/.env`），两者不是同一通道 | `deploy/systemd/kiarabot.service:12`；`compose.staging.yml:8-10`；`doc/08:158` |
| `doc/12:176-187` staging 部署后检查（数据库迁移→Core live→Core ready→适配器注册→OpenAPI health→smoke） | `deploy.sh` 无任何检查/冒烟步骤，`healthcheck.sh` 未被部署流程调用；仅 compose `--wait`（且目标端点不存在） | `deploy.sh:23-54`；`healthcheck.sh` 仅出现在 doc/tests |
| `doc/12:165-172` “CI 在 `main` 或正式 tag 上构建镜像、smoke、SBOM、推送 GHCR、保存 digest” | `ci.yml` 无 docker/GHCR/buildx 步骤；`doc/12:144` 自认“待实施”，`:97` 说明自动触发暂停 | `ci.yml:7-17,26-260`；`doc/12:97,144` |
| `doc/12:216-222` 迁移服务示例 `command: ["/usr/local/bin/kiarabot-migrate"]` | 实际 compose 用 `/usr/local/bin/migrate.sh`，且镜像内没有 `kiarabot-migrate` 二进制 | `doc/12:216-222`；`compose.prod.yml:7`；`Dockerfile:12,24` |
| `doc/16:336` T11a “已完成并验收（本机自动验收）”（验证命令组 P） | 控制面探针/迁移命令确已实现并有测试；但“启动接线”只在 proto+infra+控制配置+`--grpc-listen` 下成立，默认镜像/compose 未接线（`doc/16:600` 已认）；T11a 验收不能外推为“部署可用” | `doc/16:334-347,600`；`core/app/main.cpp:105-110,223-271` |
| `doc/18:339-342`（基线目录）`deploy/ { docker/, systemd/ }`；`doc/18:407` A.1 记 `deploy/{docker,systemd}` | 实际 `deploy/` 为 `compose.{staging,prod}.yml` + `systemd/`，无 `docker/`；A.1 同行“职责一致；生产部署、备份恢复和发布镜像尚未验收”与事实一致 | `doc/18:339-342,407`（只读引用，不修改）；`git ls-files deploy` |
| `doc/16:617` 台账只记录注册了 `deploy_deployment`（5 用例） | `deploy_healthcheck`（2 用例）也已注册，且两套件都在 `ctest --preset dev` 中执行 | `core/CMakeLists.txt:49-53`；本机运行 7/7 通过 |
| `doc/12:95-101` “自 2026-10-30 起只保留 `workflow_dispatch`”；`README.md:52` 同 | 与 `ci.yml:7-17` 一致（无漂移）；但由此部署脚本测试当前不受自动门禁保护 | `ci.yml:3-17` |

## 6. 跨模块影响（只陈述，不替其他模块下结论）

- **Core**：gRPC 桥接/控制 HTTP 只绑定 `127.0.0.1`（`bridge_server.cpp:527-529`、`http_server.cpp:296`）与 compose 多容器拓扑冲突（P0-4）；`--grpc-listen`/`KIARABOT_RUNTIME_DB`/`KIARABOT_CONTROL_CONFIG` 是持久化/控制/健康的启动前提（`main.cpp:105-110,223-271`），compose 均未提供（P0-3）。**待 core 模块确认**：是否计划支持容器网络绑定（涉及 doc/08:189、doc/06 的回环安全语义修订）。
- **基础设施**：`kiarabot-migrate` 目标只在 `KIARABOT_ENABLE_INFRASTRUCTURE=ON` 下构建（`infrastructure/CMakeLists.txt:89-90`），未进镜像；Core 打开数据库时会自动迁移 v1–v4→v5（`sqlite_runtime_store.cpp:138-195`），因此“迁移步骤失败阻断启动”实际由 Core 兜底，运维备份时点与自动迁移时点可能错位（P0-6/P1-16）。
- **适配器**：入口名、环境变量名、健康服务、outbox 卷均与 compose 不一致（P1-1/P1-2/P0-5）；**待 nonebot 模块确认**：容器化入口应为 `python -m kiarabot_adapter_nonebot`，`CORE_ENDPOINT` 是否为未来 C++ 适配器预留。
- **面板**：静态前端 + 构建期 `VITE_*` 配置 + 默认 mock（`panel/src/main.tsx:7-11`），compose 的 `CONTROL_API_URL`/`kiarabot-panel` 不适用（P1-1/P0-2）；**待 control-panel 模块确认**面板生产形态（静态托管/反向代理）与同源代理要求。探针与认证 health 已有实现测试覆盖：`control/api/tests/test_operations.py:22-37`。
- **控制 API**：探针与认证 health 已实现且被 `control/api/tests/test_operations.py` 覆盖，但承载进程只有 proto+infra 构建才有；`HEALTH_PORT` 未被任何代码读取（`grep` 全仓仅 compose/doc 命中）——与 `doc/08:138` 一致。
- **CI/CD**：镜像构建/GHCR/staging 部署全部未实现且自动触发暂停（`ci.yml:7-17`）；本模块的 7 个 Python 测试虽已进 CTest，但当前只能在手动 CI 或本地运行（P1-13）。与 ci-cd 报告结论可互相印证。

## 7. 覆盖结论

- **一句话结论**：运维模块的**脚本层质量是本项目里较扎实的部分**（7 个部署/探针用例本机全绿、备份/恢复/锁/失败路径均有真实行为），但**镜像、Compose、Core 启动参数、容器网络四层互不相接**，默认 `deploy.sh start/upgrade` 成功路径不可运行，T11b 未验收；日志只有容器默认输出，告警/指标/风控/演练全部缺位。
- 覆盖度估计（口径：把 `doc/08:21-126` 职责、`doc/12:174-279` 部署流程、`doc/16:348-361` T11b 验收、`doc/17:82-129` 运维要求合并为 25 个功能点）：
  - **完整覆盖 3 个**：`.dockerignore` 白名单、`healthcheck.sh` 探针脚本、`log.sh` JSON 日志。
  - **部分覆盖 11 个**：非 root 多阶段镜像、compose 拓扑/依赖顺序、prod digest 占位、healthcheck 命令/端口、卷持久化、环境变量/密钥注入、deploy 状态机、migrate、backup、部署测试、systemd unit。
  - **未覆盖 11 个**：可运行生产镜像、镜像健康端点约定、网络隔离与 TLS、备份保留/离线/完整性、备份恢复与 schema 回滚演练、升级窗口风险控制、日志轮转/集中收集、指标、告警、封禁/速率风控运维配置、部署记录与可追溯（含 CI 构建镜像/部署）。
- 与 T11b 的关系：`doc/16:348-361` 的 5 条验收（真实镜像、启动关闭顺序、备份校验恢复、回滚前 schema、隔离 staging 演练）在代码层均只具备“脚本片段”，**没有一条具备可执行路径**；结论支持 `doc/16:600` 的“部署骨架不可运行”，并补充其未列出的 P0-3/4/6 与 P1 各项。

## 8. 未核实事项

1. **Docker 构建/运行结果**：本机无 `docker`，未执行 `docker build`、`docker compose config/up`。推断 dev 镜像可构建成功（`core/CMakeLists.txt:40-44` 只需 python3，build 阶段已装），但未验证；`env_file` 长语法/`--wait` 对 one-shot `migrate` 服务的行为、`--wait-timeout` 语义均未实测，可能影响 `deploy.sh start` 的判断（compose 版本未在仓库中锁定）。
2. **基础镜像内容**：`ubuntu:24.04` 是否自带 `/usr/share/zoneinfo`（tzdata）未验证；也未验证 runtime 阶段是否真的有 `bash`/`awk`/`mktemp`/`sha256sum`（Ubuntu 基础镜像预期有，但未在镜像内确认）。
3. **注册表真实状态**：`ghcr.io/leekiara1215/kiarabot-{core,adapter-nonebot,panel}` 是否已有可拉取镜像（尤其 adapter/panel，仓库内无 Dockerfile、CI 无镜像 job）无法离线核实；prod 全零 digest 一定是占位。
4. **systemd 实机行为**：`Type=oneshot` + `TimeoutStartSec=300` 在慢速拉镜像/升级超时时的杀进程行为、`docker compose down` 时长与 `TimeoutStopSec=120` 的匹配，需 Linux 主机验证。
5. **远端 CI 是否已全绿**：仅能引用 `doc/16:599`/`README.md:52` 的自述（历史 5 次未通过、自动触发暂停），无法核实部署测试在 CI 的实际执行结果；本机 7/7 通过只证明 macOS+bash 3.2 兼容性。
6. **`HEALTH_PORT` 的设计意图**：全仓无代码读取（已核实），但它是“预留给适配器/面板未来健康服务”还是“本应由 Core 读取”，需 core/adapters/panel 模块确认；compose 端口 8080/8081/8082 与实际监听端口的一致性因此无法定论。
7. **速率/封禁风控的接口归属**：`doc/17:125-129` 的指标/演练是否应由运维配置承担、还是由 Core/Action 提供数据源，属于跨模块决策；本报告只判定“运维侧无配置”，不判定风控逻辑本身是否实现（见 core/adapter 报告）。
8. **`MIGRATION_COMMAND` 的生产取值**：文档只说“通过受控环境变量提供命令”，未给出唯一权威值；`kiarabot-migrate` 与 Core 自动迁移并存时哪一个是权威迁移路径，需 infrastructure/运维共同确认（本报告已把两者时点错位列为 P0-6/P1-16 风险）。
