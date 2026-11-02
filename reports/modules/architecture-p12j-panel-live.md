# P12j Panel API client 对原生目标栈真实 live 验证

- 日期：2026-11-03；worktree `.worktrees/p12j`，branch `codex/p12j`；基线 `fc454bc`（原 P12j 基线 `4e88fb4`，已 rebase 到 P14i）。
- 范围：P14f/P14i 原生单机目标栈（真实 Identity/Runtime/Scheduler/Model/Control 五进程 + PG16.4 + 临时 PKI + Control HTTPS），用 HTTP 复现 Panel `api/client.ts` 的 live 矩阵；不启动浏览器。
- 结论：**严格模式 PASS。`/api/v1/health` 真实 **200 HEALTHY**（四个真实 owner 组件），登录/refresh、Control `/health/live|ready`、401/400 和 Panel 错误语义通过；principals 为 capability-gap（P12l ListPrincipals 待实现），logs/audit/tasks/models/action/lifecycle 为 known-gap（P14j scope/参数对齐中），严格矩阵按 Lead 口径允许这些 stable error 并显式标注，不伪造成功。**

## 1. Harness 与命令

P12j 矩阵嵌入 P14f/P14i `tests/architecture/p14_native_single_host.py`（env `KIARABOT_P12J_PANEL=1`），复用 P14i 的 Control owner-backed matrix、fixture 授权与 fail-closed 故障矩阵。实际执行：

```bash
cd /Users/betta/Workspace/KiaraBot
PATH="$PWD/build/pg-16.4-install/bin:$PWD/build/openssl-3.5.0-install/bin:$PATH" \
KIARABOT_P12J_PANEL=1 KIARABOT_P12J_EXPECT_FIXED=1 \
python3 infrastructure/postgres/tests/run_database_test.py \
  --pg-bin build/pg-16.4-install/bin --openssl build/openssl-3.5.0-install/bin/openssl \
  --initializer build/full-locked/infrastructure/postgres/kiarabot-pg-initialize \
  --test .worktrees/p12j/tests/architecture/p14_native_single_host.py \
  --test-argument build/full-locked/bin/kiarabot-identity-bootstrap \
  --test-argument build/pg-16.4-install/bin \
  --test-argument build/full-locked/bin/kiarabot-identity \
  --test-argument build/full-locked/bin/kiarabot-runtime \
  --test-argument build/full-locked/bin/kiarabot-scheduler \
  --test-argument build/full-locked/bin/kiarabot-model \
  --test-argument build/full-locked/bin/kiarabot-control
```

前置：main build 已 `cmake --build build/full-locked --parallel 3` 重建到 `fc454bc`（exit 0）。CTest `p12j_panel_live` 已在 `infrastructure/postgres/CMakeLists.txt` 登记（label `p12;p14;integration;fault`，TIMEOUT 300，ENVIRONMENT 含 `KIARABOT_P12J_PANEL=1`）；本 worktree 无独立 full-locked build，因此本机以同一 owned PG fixture 命令实跑，CTest 条目在共享构建 reconfigure 后可直接跑。

## 2. 严格模式实测（post-fix，main fc454bc）

`KIARABOT_P12J_EXPECT_FIXED=1` **exit 0**，关键输出：

```
P14f control probe: live=HEALTHY ready=HEALTHY
P14f authorized /api/v1/health: status=200 state=HEALTHY
P14f owner matrix (fix_ready=True): [('health','success'), ('principals','capability-gap'),
  ('logs','known-gap'), ('audit','known-gap'), ('tasks_list','known-gap'),
  ('task_get_cancel','skipped:no-seeded-task'), ('models','known-gap'),
  ('action_policy','known-gap'), ('action_query','known-gap'), ('delete','known-gap'),
  ('restore','known-gap')]
P12j Panel API live matrix: health=200-shape, control-probe=live:HEALTHY,ready:HEALTHY,
  principals=known-gap-503:CAPABILITY_NOT_SUPPORTED, log=known-gap-400:INVALID_ARGUMENT,
  audit=known-gap-400:INVALID_ARGUMENT, health-unauth=401:UNAUTHENTICATED,
  health-badtoken=401:UNAUTHENTICATED, logs-invalid-filter=400:INVALID_ARGUMENT,
  actions-metadata=known-gap-400:INVALID_ARGUMENT, action-unknown=400:INVALID_ARGUMENT,
  tasks-list=known-gap-400:INVALID_ARGUMENT, task-get-unknown=400:INVALID_ARGUMENT,
  task-cancel-unknown=403:PERMISSION_DENIED, tasks-cross-scope=403:PERMISSION_DENIED,
  models-list=known-gap-403:PERMISSION_DENIED, model-metadata=400:INVALID_ARGUMENT,
  model-configuration=400:INVALID_ARGUMENT, resource-delete=400:INVALID_ARGUMENT,
  resource-restore=400:INVALID_ARGUMENT, models-outage=503:UPSTREAM_UNAVAILABLE
P14f native single-host target stack smoke passed
```

### 各项语义

| 能力 | 严格结果 | 说明 |
|---|---|---|
| login/refresh | 200 token pair | 真实 Identity session |
| Panel health() | **200 HEALTHY**（components 四 owner） | P14i 修复后真实成功；严格模式强制此形状 |
| healthLive/healthReady | 200 HEALTHY / 200 HEALTHY | Control probe |
| 未认证 / bad token | 401 / 401 UNAUTHENTICATED | P14i 后 bad token 从 400 收敛为 401；Panel `ApiError` unauthorized 路径可达 |
| principals | 503 CAPABILITY_NOT_SUPPORTED | **capability-gap**：Identity ListPrincipals target 未实现（P12l 进行中） |
| logs/audit 分页+actor/resource/owner | 400 INVALID_ARGUMENT | **known-gap**：P14j 对齐 Control/owner scope/参数；目前无 200 成功证据 |
| Action metadata/query | 400 INVALID_ARGUMENT | known-gap；unknown action 稳定 400 |
| tasks list/get/cancel | 400 / 400 / 403 | known-gap；cancel 缺权限映射 403，未伪造成功 |
| 模型管理读 | 403/400/400 | known-gap；models list 403、metadata/config 400，P14j 对齐后重验 |
| delete/restore | 400 INVALID_ARGUMENT | known-gap；stable error，不伪造 operation |
| 依赖故障 | 503 UPSTREAM_UNAVAILABLE | 停 Model 后模型读稳定 503（不返回部分成功） |
| 非法 filter | 400 INVALID_ARGUMENT | Panel query 映射/错误类正确 |
| tasks 跨 scope | 403 PERMISSION_DENIED | 权限边界正确 |

## 3. Pre-fix 对照（4e88fb4，历史证据）

- P14g/P14h 合入前：login/refresh 200、Control `/health/live` 200、未认证 401、非法 filter 400；owner-backed 全部稳定 400 INVALID_ARGUMENT；health() 400；bad token 400；`/health/ready` 不稳定。
- P14g/P14h 初步合入（dc71f9f）时严格模式仍失败于 health 400（当时 native bootstrap/Control catalog 授权链未完全形成）；随后 P14i `1ba07c9`/`a87da27` 修复 Control 委派 UUID/Catalog id/权限上限/readiness，严格模式在 `fc454bc` 通过 health 200。P14j 尚在推进其余 owner scope/参数。

## 4. 未验收/剩余 blocker

- **principals**：P12l ListPrincipals 待实现（capability-gap，503 CAPABILITY_NOT_SUPPORTED）。
- **logs/audit/tasks/models/action/lifecycle**：P14j 正在对齐 Control 委派 scope/参数；本报告只证明当前稳定错误分类，不是成功。
- Panel 浏览器真实渲染/交互未跑（本任务只验证 client HTTP 形状/错误映射）。
- Docker Compose 单机、跨主机、远端 CI、正式发布未验收；真实 QQ/外部 Model Provider/Host/Adapter bridge 链路不在本矩阵。
