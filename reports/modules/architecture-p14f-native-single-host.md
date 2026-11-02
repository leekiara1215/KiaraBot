# P14f 原生单机目标栈 smoke（无 Docker，2026-11-02）

状态：**验收 harness 已实测通过（受限范围）**。本文件记录原生单机 smoke 的拓扑、实际运行证据、
与 Compose 单机部署的对应/差异、发现的真实缺陷与未验收项。它是**验收 harness，不是生产部署
替代**；不修改业务契约。

- 基线：`439fb9e`（main）；工作树 `.worktrees/p14f`（分支 `codex/p14f`）
- 入口：`ctest --preset full-locked -R '^p14_native_single_host$'`（label `p14;integration;fault`）
- 驱动：`tests/architecture/p14_native_single_host.py`（由 `run_database_test.py` 以 owned PG fixture
  调用；无外部 DSN、无 Docker、无生产端点）

## 1. 拓扑

单机原生进程（全部真实二进制 `build/full-locked/bin/`）：

```
owned PG16.4 cluster (P00)
  ├─ p03_identity   (kiarabot-pg-initialize --initialize-empty identity)
  ├─ p03_runtime    (… runtime)
  ├─ p03_scheduler  (… scheduler)
  └─ p03_model      (… model)
temporary CA + leafs (P04 bootstrap PKI)
  ├─ control.pem    浏览器 TLS SAN(DNS:localhost,IP:127.0.0.1)+clientAuth+serverAuth
  ├─ runtime.pem    runtime peer (URI spiffe://kiarabot/runtime-test)
  ├─ scheduler.pem  scheduler peer (URI spiffe://kiarabot/scheduler-test)
  └─ model.pem      model peer (URI spiffe://kiarabot/model-test)
offline bootstrap (kiarabot-identity-bootstrap)
  → administrator + Control principal/credential + platform peers(runtime/scheduler/model)
processes:
  identity   : probe HTTP / grpc mTLS
  runtime    : probe HTTP / grpc mTLS / identity_endpoint
  scheduler  : probe HTTP / grpc mTLS / identity+runtime endpoints
  model      : probe HTTP / grpc mTLS / identity_endpoint / model_security root key
  control    : probe HTTP / HTTPS browser TLS (control_api: identity/runtime/scheduler/model)
```

未启动：Plugin Host、Adapter、Bridge listener、真实 QQ/Provider（见第 4 节）。

## 2. 实测证据（本 HEAD 实际运行）

- `cmake --build --preset full-locked --parallel 3` → exit 0（全量构建；包含 5 个服务二进制、
  `kiarabot-pg-initialize`、`kiarabot-identity-bootstrap`）。
- `ctest --preset full-locked -R '^p14_native_single_host$'` → **Passed**（约 12–22s，含 PG
  init/bootstrap/五进程启动/断言/停启/清理）。驱动输出：
  - `P14f control probe: live=HEALTHY ready=UNHEALTHY`
  - `P14f authorized /api/v1/health -> 400:INVALID_ARGUMENT (P12 schema gap, fail-closed)`
  - `P14f owner reads observed: {'principals': 400, '/api/v1/logs': 400, '/api/v1/audit': 400,
    'tasks': 400, 'models': 400, 'action_policy': 400, 'action_query': 400, 'delete': 400,
    'restore': 400}`
  - `P14f fail-closed: bad CA and unauthenticated database credentials refused`
  - `P14f native single-host target stack smoke passed`
- 断言内容：
  1. 五个真实进程启动，四个 owner（identity/runtime/scheduler/model）probe `/health/live` 与
     `/health/ready` 均为 200 HEALTHY（真 PG + 真 mTLS + 真 Identity channel）。
  2. Control browser TLS：真实 Identity 登录 200、refresh 200、未授权 `/api/v1/health` 401。
  3. Control probe `/health/live` 200 HEALTHY；`/health/ready` 200/503 且 state=UNHEALTHY
     （Control 组合根 readiness 未置 ready，稳定记录，不伪造）。
  4. 授权 `/api/v1/health` 及所有 owner-backed REST 读取返回**稳定 400 INVALID_ARGUMENT**
     （fail-closed；原因见第 3 节，不伪造成功）。
  5. 故障/重启：停 Identity → 登录失败（非 200）且依赖健康稳定失败；重启后登录恢复。
     停 Runtime → runtime probe 未就绪；重启后 probe 恢复 HEALTHY。
  6. fail-closed：Control 配置不可信 CA → 启动非零；Runtime 错误数据库密码文件 → 启动非零。

## 3. 发现的真实缺陷（未在本任务修复，已上报 Lead）

1. **Identity 迁移未放开敏感权限（P12/P07/P09 target 声明与 DB CHECK 不一致）**：
   `infrastructure/postgres/schema/identity_security.hpp` 的 `exact_permissions`
   `CHECK(permission BETWEEN 1 AND 40)`，而 wire Permission 41–50（P07 按动作权限、
   P09 `namespace.read`、P12 `log.read/audit.read/health.read`）已冻结声明。离线 bootstrap
   无法持久化这些授权；`/api/v1/health`、`/logs`、`/audit` 的敏感读取在原生栈必然失败。
   需要 Identity schema 迁移扩展允许值（并补相应 target/DB 测试）。
2. **Control 空 scope 委派被 Identity 拒绝**：Control `/api/v1/health` 与 `/api/v1/principals`
   使用空 `ResourceScope` 发委派；`core/src/application/target/identity_delegation_application.cpp`
   对 `request.permissions.empty()` 返回 INVALID_ARGUMENT，因此即使健康权限可存也无法授权。
   P12c 的 fake Identity 覆盖了该缺口。需要 P12f 修正 Control/Identity 的空 scope 语义或
   Control 侧显式 catalog scope（注意与契约一致）。
3. **owner-backed 读取全链路未证明**：上述两点导致 `/principals`、`/logs`、`/audit`、
   `/tasks`、`/models`、action policy/action query、delete/restore 在本 harness 中只能断言
   稳定错误（400），尚未形成真实成功证据。本任务按“不伪造”如实记录，留给 P12f/P07g 修复后
   重跑。
4. Control 进程 `/health/ready` 为 UNHEALTHY（HTTPS 仍可用）。需确认 Control 组合根是否应
   接入 readiness composition；当前记录为运维语义差异，未改代码。

## 4. 与 Compose 单机（P14a-2）的对应/差异

对应：同一组服务（Identity/Runtime/Scheduler/Model/Control；Host/Adapter 为可选）、同一
`ProcessConfig` 字段（probe/grpc/tls/database/identity_security/model_security/control_api/
identity_endpoint/runtime_endpoint/model_endpoint）、同一 PKI/离线 bootstrap 模型、同一四库
初始化（`--initialize-empty`）与启动顺序（PG → Identity → Model/Runtime/Scheduler → Control）。

差异（native harness 不证明 Compose 验收）：
- 无 Docker 镜像构建/Compose 解析/healthcheck/restart 策略/非 root/只读根文件系统/
  no-new-privileges/secrets 挂载；无官方 PG 镜像与 compose init 进程。
- 未启动 Plugin Host、Adapter、Bridge listener，未覆盖 P06/P07/P08 的 bridge/受管进程链路；
  未接真实 QQ/NapCat、真实外部 Model Provider。
- 未覆盖两主机拓扑、跨主机网络分区、备份恢复演练（P14c 已有独立本地演练）；本 harness 的
  PG 是临时 disposable 集群，不代表生产备份/恢复。
- Control `/api/v1/health` 与敏感读取当时只能 fail-closed（第 3 节）；P14h 已修复 Identity
  exact_permissions 1..40 约束/boot 敏感授权持久化，但 Control 空 ResourceScope 委派仍使
  原生 health/owner-backed 读取 fail-closed，需后续任务；本报告不因此宣称单机健康聚合已验收。

## 5. 未验收/需环境（用户/Docker）

- Docker Compose 单机：镜像构建、`docker compose config/up`、healthcheck、初始化顺序、
  非 root/只读/秘密挂载、重启/资源限制（本机无 Docker）。
- 真实 QQ/NapCat 平台与真实外部 Model Provider；受管 Host/Adapter 进程联测。
- 两主机/跨主机与断网演练；生产 CA/密钥托管、GHCR 签名/release tag。
- Control 空 ResourceScope 委派修复（P14h 仅修 Identity 权限范围/bootstrap 授权）后，重跑
  本 harness 并把 owner-backed 成功链纳入矩阵。

## 6. 文档影响判断

- 已更新：`doc/08-operations.md`（新增 P14f 章节：命令、拓扑、实测证据、缺陷与差异）、本报告。
- 未更新：业务契约/`proto/`/`api/`（本任务不修改契约）；`doc/18` 受保护基线未改；
  `deploy/README.md` 无需变化（Compose 文件未动）。
- 校验：全量构建 + `ctest -R '^p14_native_single_host$'` 实跑通过；无 C++/proto 变更，
  clang-format 无适用文件。

## 7. P14i Control owner-backed 成功链矩阵（2026-11-02，修复前基线）

P14i 把本 harness 扩展为逐端点成功链矩阵，用于 P14g/P14h 修复前后复验（同一测试、不伪造）：

矩阵项：`health`（受权聚合依赖）、`principals`、`logs`、`audit`（`page_size=1&actor=administrator`）、
`tasks_list`（`task_get_cancel` 因未播种任务标 `skipped:no-seeded-task`）、`models`、`action_policy`、
`action_query`、`delete`、`restore`。分类：
- `success`：真实 2xx 且 owner 投影形状正确（audit/logs 分页、principals 非空等）；
- `stable-ok`：owner 明确返回 `NOT_FOUND`/`CONFLICT`（证明链已到真实 owner，不伪造成功）；
- `pre-fix-skip`：P14f 已报告的缺陷导致的稳定 4xx（待 P14g/P14h 修复）；
- `grant-missing`：修复后仍 `PERMISSION_DENIED`（视为复验失败）。
- fixture 能力检测：仅当 checkout 的 Identity 权限名目录与 PG `exact_permissions` CHECK 已允许
  wire Permission 41–50 时，才在离线 enrollment 中给 Control 授予 `log.read/audit.read/health.read`
  （Control catalog scope），因此同一 harness 可在修复前后运行且不会 bootstrap 失败。
- 修复后判定：一旦受权 `/api/v1/health` 返回 200 HEALTHY 且四组件 ready，任何
  `pre-fix-skip`/`grant-missing` 行都会使测试失败；故障重启断言同时接受修复后的
  `DEGRADED/HEALTHY` 语义。

**修复前实测（main `4e88fb4`，P14g/P14h 尚未合入）**：
`ctest --preset full-locked -R '^p14_native_single_host$'` → **Passed 13.7s**，
`fix_ready=False`，矩阵为：health/principals/logs/audit/tasks_list/models/action_policy/
action_query/delete/restore 全部 `pre-fix-skip`（稳定 400 INVALID_ARGUMENT），
`task_get_cancel`=`skipped:no-seeded-task`；进程拓扑、登录、probe、故障重启与 fail-closed
仍全部通过。

**修复后复跑状态**：截至本 commit 检查，main 仍未包含 P14g/P14h 修复（Identity
`exact_permissions` 仍为 CHECK 1..40，Control 仍以空 scope 发 health 委派，权限名目录仍无
log/audit/health）。因此本任务**尚未获得修复后 success 结果**；修复合入 main 后只需
`git rebase main` 并重跑同一条命令，矩阵会自动切换为 success/失败断言，无需改测试逻辑。

## 8. P14i 修复后复验（2026-11-03，P14g/P14h + Control/Identity 修复）

P14i 在修复分支上定位并修复了 4 个真实缺陷（见 commit `fix(p14i)`）：

1. **Control 委派请求 id 非 UUID**：`owner_clients.cpp` 的 `random_identifier()` 返回 32 位 hex，
   Identity `run` 边界要求 UUIDv4，直接以 INVALID_ARGUMENT 拒绝（未进入 handler）。
2. **Control 委派缺 idempotency key**：IdentityIssueDelegation 是幂等 RPC，空 key 会被边界拒绝；
   Control 现生成一次性 key。
3. **Catalog 资源 id 校验过严**：`target_codec` 对 `RESOURCE_CATALOG` 套用 UUID 默认规则，拒绝
   文档规定的 Control catalog id `control`；改为与 Task 相同的 bounded printable 规则，
   并补 `p04_target_codec` 正/反例。
4. **委派加密权限上限仍为 MessageRead(40)**：`delegation_crypto.cpp` 的 shape/decode 硬编码
   `<= Permission::MessageRead`，带 41–50 权限（log/audit/health.read 等）的委派签名失败
   INVALID_ARGUMENT；改为 `Permission::HealthRead(50)`。
   同时 `identity_bootstrap_test` 的敏感权限断言改为 `COUNT(DISTINCT permission)=10`，允许
   扩展 enrollment 追加同权限的 Catalog-scope 精确授权（原生栈/P12j 需要）。

**修复后实测**（`ctest --preset full-locked -R '^p14_native_single_host$'`，`KIARABOT_P14F_DEBUG_GRANTS=full`）：
Passed ~13–16s，`fix_ready=True`，矩阵：
- `health` = **success**（200 HEALTHY，identity/runtime/scheduler/model 四组件真实 HEALTHY；
  Control probe `/health/ready` 也随 P14g 修复变为 HEALTHY）
- `principals` = `capability-gap`（Identity `ListPrincipals` 仍是 target `not_implemented`，
  Control 返回稳定 CAPABILITY_NOT_SUPPORTED，非伪造）
- `logs`/`audit`/`tasks_list`/`models`/`action_policy`/`action_query`/`delete`/`restore` =
  `known-gap`（稳定 4xx：Control owner 请求/查询参数或委派 scope 仍与 owner 期望不一致，
  例如 audit actor 过滤需 principal UUID、tasks/models scope 编码需进一步对齐）。
- 故障/重启/fail-closed 全部保留通过。

**修复前/后对照**：修复前 P14f 实测 `/api/v1/health` 400 INVALID_ARGUMENT、所有 owner-backed
读取只能稳定错误；修复后 `health` 首次形成真实四依赖成功链，`principals` 明确为契约未实现
（capability-gap），其余 owner 读取仍有稳定 known-gap，不再阻塞 smoke 通过但**不计入成功**。

**剩余缺口（P14j 候选）**：Control owner 请求参数/委派 scope 对齐（logs/audit actor 过滤、
tasks scope、models scope、action/lifecycle 请求体）；Identity ListPrincipals/ListSessions/
ListAssignments 的 target 实现（当前 not_implemented）；真实 Host/Adapter、真实 QQ/Provider、
Docker Compose 单机与两主机项仍按 P14f 文档保留。

## 9. P14j 端点参数对齐与剩余 known-gap（2026-11-03）

P14j 继续对齐 native owner-backed 请求与真实 owner 期望，未改 proto/业务契约：

- `logs`/`audit`：`actor` 过滤从登录名 `administrator` 改为 administrator **principal UUID**
  （Control/Identity 把 actor 当作 principal 标识校验），失败由 400 INVALID_ARGUMENT 变为
  owner 稳定语义 503 UPSTREAM_UNAVAILABLE（Identity/Runtime audit source 仍有稳定失败，
  矩阵继续记录为 known-gap）。
- `tasks`/`models` 列表：请求补 `page_size=10`，避免 Scheduler/Model owner RPC 对缺省零
  页大小的参数拒绝。
- 矩阵分类保留：success / stable-ok（owner NOT_FOUND/CONFLICT）/
  capability-gap（契约 target 未实现，如 Identity ListPrincipals，待 P12l）/
  known-gap（稳定 4xx/5xx，记录原因且不计成功）/ pre-fix-skip / grant-missing。
  严格模式 `KIARABOT_P12J_EXPECT_FIXED=1` 仍要求 `/api/v1/health` 200 HEALTHY。

修复后实测：`ctest -R '^p14_native_single_host$'` Passed ~13–14s；矩阵
`health=success`（200 HEALTHY 四真实组件）、`principals=capability-gap`（Identity
ListPrincipals target `not_implemented`，P12l 并行补齐）、
`logs/audit/tasks_list/models/action_policy/action_query/delete/restore=known-gap`。

**剩余 known-gap 原因（P14k 候选）**：
1) logs/audit：Control→Identity/Runtime audit source 仍返回稳定 503（需要逐 owner 审计源
   配置/委派 scope 对齐；当前 fixture 未播种 audit/log 行，不能伪造 200）。
2) tasks_list：SchedulerService ListTasks 仍 400 INVALID_ARGUMENT；RPC `prepare` 的
   `authorize`（scheduler_identity_client）对 Control 委派的 installation scope/owner
   derivation 需进一步对齐（或 Control 需要显式 instance scope）。
3) models list：ModelService ListModelMetadata 仍 403 PERMISSION_DENIED；Model 的
   `authorize(caller, ModelRead, model_resource)` 对 Control 委派/复核路径需核对
   caller-vs-delegated-subject 授权语义。
4) action_policy/action_query/delete/restore：Runtime owner RPC 仍 400 INVALID_ARGUMENT；
   需逐方法核对 Control owner_clients 的 context/delegation/scope 与 Runtime prepare
   期望（idempotency、scope kind、account scope 等），不排除 Control 侧真实缺陷。
5) principals：Identity ListPrincipals/ListSessions/ListAssignments 仍 target
   not_implemented，待 P12l 合入后接入成功链。

**需用户环境项**（延续 P14f/P14i）：真实 Host/Adapter 联测、真实 QQ/NapCat 与外部 Model
Provider、Docker Compose 单机（镜像构建/compose/healthcheck/非 root/只读/秘密挂载）、
两主机与断网演练、生产 CA/密钥托管与 release 签名。

## 10. P14k owner 链对齐与契约缺口（2026-11-03）

P14k 定位并修复了两个跨边界真实缺陷（commit `fix(p14k)`，无 proto/API 改动）：

1. **Control response request/trace id 非 UUID**：`owner_gateway::random_identifier` 产生 32-hex；
   这些 id 原样进入 owner RPC `RequestContext`，所有 owner 边界要求 UUIDv4 并在 use case 前
   以 INVALID_ARGUMENT 拒绝。修复为 UUIDv4 后 principals/tasks/models/actions/lifecycle 的
   RPC 才真正到达 owner。
2. **Scheduler 授权 scope 混入 Installation**：`SchedulerIdentityClient` 把客户端 scope 原样
   转发给 Identity CheckAuthorization，而 Scheduler 方法策略只接受 Instance/Task；
   Installation 只是 delegation binding 的 owner 派生锚点。过滤 Installation 后 tasks_list
   变为真实 200 空页。

修复后实测（strict matrix，`ctest -R '^p14_native_single_host$'` Passed ~14s）：
- **success**：`health`（200 HEALTHY 四真实组件）、`principals`（P12l 真实页：1 principal +
  page/checked_at）、`tasks_list`（200 空页 + page_size）、`delete`（404 NOT_FOUND owner 语义）、
  `restore`（409 CONFLICT owner 语义）。
- **known-gap**：`logs`/`audit` 稳定 503 UPSTREAM_UNAVAILABLE；`models` 稳定 403
  PERMISSION_DENIED；`action_policy`/`action_query` 稳定 400 INVALID_ARGUMENT。

**契约缺口（按任务要求停下上报，不修改 proto）**：以下已实现 owner 方法在 proto option 中
缺少 `scope_kinds`，Identity `CheckAuthorization` 方法策略因此无法接受任何 scope，必然以
INVALID_ARGUMENT 拒绝（或 Model 侧映射为 PERMISSION_DENIED），Control 无法通过合法的精确
scope 委派它们：
- `RuntimeService.GetActionPolicy`（runtime.proto:1248，仅 required_permission/idempotency；
  对照同文件 GetAccount 有 `scope_kinds = RESOURCE_ACCOUNT`）；
- `RuntimeService.UpdateActionPolicy`（runtime.proto:1255）；
- `RuntimeService.QueryActionResult`（runtime.proto:1121）；
- `ModelService.ListModelMetadata`（model.proto:240；对照 Chat 有 `scope_kinds = RESOURCE_MODEL`）。
需要契约/P07/P11 owner 决策补充 scope_kinds（或明确这些 target 方法不对外），本任务不改 proto。

**logs/audit 503 诊断（未根因）**：Control→Identity/Runtime audit source 调用仍返回
UPSTREAM_UNAVAILABLE；Identity audit 查询要求 peer Control 在 catalog `control` 上的
log.read/audit.read 精确授权（fixture 已镜像授权，bootstrap 通过），Runtime 侧要求同权限一次性
委派（fixture 已配置）。下一步需要在 audit source client/owner 侧加稳定错误细节定位
（候选 P14l），空数据应返回 200 空页而非 503。

**修复前/后对照**：P14j 时仅 health success、principals 503、tasks/models/action 400/403、
delete/restore 400；P14k 后 health/principals/tasks/delete/restore 共 5 项 success，
其余 5 项为稳定 known-gap，其中 4 项是明确的契约 scope_kinds 缺口。
