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
