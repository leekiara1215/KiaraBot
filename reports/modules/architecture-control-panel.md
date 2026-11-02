# P12f：Control/Panel 退出验收矩阵（真实 Control 进程全端点）

- 基线：main@`3ac3761`（P12c-1..7 + P12d-1..5 + P12e/P12e-2）
- 验收方式：真实 `kiarabot-control` 进程 + 真实浏览器 TLS（mTLS/证书校验）+ fake Identity/Model/Runtime/Scheduler owner 服务（typed canned responses）；不是 mock 单测、不是目标六服务栈。
- 测试入口：`p04_control_auth_https` / `p11_control_model_rest` / `p12_audit_logs_rest`（同一 Python harness：`control/api/tests/architecture/auth_https_test.py`，owner fixture：`control/api/tests/architecture/owner_fixture.cpp`）。
- 结论：**P12 Control 的传输/认证/授权分层/错误映射/分页/合并/operation 引用已在本机真实进程+TLS 上逐端点验收**；模型管理、日志/审计、生命周期、任务、策略、绑定、安装实例均可走真实 Control → fake owner 成功链。目标六服务真实栈、Panel 浏览器真实 HTTP、真实 QQ/模型 Provider 仍未验收。

## 1. 端点验收矩阵

| 端点 | 权限/owner | 成功证据（真实 Control+TLS） | 拒绝/冲突/不可用证据 | 真实成功链标注 |
|---|---|---|---|---|
| `GET /health/live`、`/health/ready` | 无/鉴权 ready | 进程存活、ready 布尔 | 依赖未就绪→ready=false；受权健康 401/403/503 | 真实进程；依赖为 fake |
| `GET /api/v1/health` | health.read / identity+runtime+scheduler+model | **200**，四组件齐全，state HEALTHY/DEGRADED | 匿名 401；health.read 拒绝 403；Identity 故障 503 | Control 聚合真实；owner 为 fake |
| auth login/refresh/me/logout/session revoke | identity.* | 200（既有 P04 用例，含撤销 4401/WS） | 401/403/429/503、严格输入 400 | 真实 Control↔fake Identity |
| `GET /api/v1/actions/{id}` | action.inspect / runtime | **200**，action_id/status/execution_outcome 正确 | 无 token 401；无 Runtime 403/503；不存在 404 | 真实 Control→fake Runtime |
| `GET /api/v1/actions/by-idempotency-key` | action.inspect / runtime | 路径与参数校验（单测）；本次矩阵未跑成功 | 缺 owner_instance/Idempotency-Key→400 | 仅路径/单测 |
| `GET /api/v1/principals` | identity.principal.read / identity | **200**，分页主体列表 | 拒绝 403；无源 503 | 真实 Control↔fake Identity |
| `GET /api/v1/logs` | log.read / identity+runtime | **200**，两 owner 行合并、owner 标注、page 完整 | 非法 cursor/时间/actor/未知 query→400；无 token 401；源不可用 503 | 真实 Control↔fake owners（P12e 已验真实 PG owner） |
| `GET /api/v1/audit` | audit.read / identity+runtime | **200**，两 owner 行合并；与 logs 不互串 | 同上；audit 拒绝不降级为 logs | 真实 Control↔fake owners |
| `PUT/DELETE /api/v1/adapter-bindings/{account}` | account.manage / runtime | **200**，binding+operation 返回 | 冲突 marker→**409 CONFLICT**；不可用 marker→**503**；非法 400 | 真实 Control→fake Runtime |
| `POST /api/v1/installations` / `.../instances` | plugin.manage / runtime | **200**，installation/instance 返回 | 校验失败 400；owner 不可用 503 | 真实 Control→fake Runtime |
| `PUT .../plugin-state`、`GET/PUT .../configuration` | plugin.manage / plugin.config.read / runtime | **200**，apply_state=CONFIG_APPLY_PENDING、期望/应用版本分离 | 404/409/503 契约映射 | 真实 Control→fake Runtime（Host 进程事实未验） |
| `GET/PUT /accounts/{id}/action-policy` | account.policy.* / runtime | **200**，policy/version 往返 | 409/404/503 | 真实 Control→fake Runtime |
| `GET /api/v1/tasks`、`GET/POST cancel` | task.read / task.schedule / scheduler | **200**，TaskSummary/取消后 CANCELLED | 非法 scope 400；无源 503 | 真实 Control→fake Scheduler |
| 模型管理（metadata/list/update/disable/enable/secret/config/call/resolve/operation） | model.read/manage / model | **200**，含 Disable/Enable、secret 不回显、UNKNOWN 结算、operation SUCCEEDED | 404/409/503 | 真实 Control→fake Model；真实 Provider 未验 |
| `GET /api/v1/operations/{runtime,scheduler,model}/{id}` | resource.read / owner | **200**；runtime 返回 **OPERATION_BLOCKED + participants + next_attempt_at**，scheduler SUCCEEDED | host/未知 owner→400（P12a 未声明）；404 映射 | 真实 Control→fake owners |
| `POST /api/v1/resources/{kind}/{id}/delete|restore` | resource.manage（ACCOUNT 为 account.manage）/ runtime | **202** + `operation.operation_id`（delete/restore-operation） | 400 缺 expected_version/幂等键/路径不匹配；409/404/503 | 真实 Control→fake Runtime；30 天窗口由 Runtime/P13 保证 |
| 旧 `/api/v1/plugins/{id}/enable|disable|config` | 不适用 | 未复用 | **404**（不映射到实例） | 契约冻结：旧命令不复用 |
| WS `/ws/control` logs/health.updated | — | 未验收 | 仍 not_implemented/延期 | P12a-2 延期 |

## 2. Q8 首批验收清单核对

| Q8 项 | 后端（真实 Control+TLS） | Panel | 状态与测试名 |
|---|---|---|---|
| 会话撤销 | login/refresh/revoke/me 全通过（fake Identity） | 已接线 | **已实测（后端）**；`p04_control_auth_https` |
| 账号/适配器绑定解绑 | PUT/DELETE 200 + operation；冲突 409 | live 页面用 mock transport | **已实测（fake owner）**；`p04_control_auth_https` |
| 安装/实例创建、启停、配置 | create/plugin-state/config 200；三类事实字段齐 | 已接线 | **已实测（fake owner）**；Host 真实进程未验 |
| 插件配置 | 实例配置 GET/PUT 200；旧 `/plugins/{id}/config` 不复用(404) | 实例配置页已接线 | **已实测（fake owner）**；真实插件 ACK 未验 |
| Action 元数据 | `GET /actions/{id}` 200；`listActionMetadata` 仍 not_implemented | 已接线（action.inspect） | **部分**：单条查询已实测，元数据列表**未接线**（契约 not_implemented） |
| 任务查询/取消 | list/get/cancel 200；取消后 CANCELLED | 已接线 | **已实测（fake Scheduler）** |
| 模型配置/密钥更新/Disable/Enable | metadata/list/update/disable/enable/secret/config 200 | 已接线 | **已实测（fake Model）**；真实 Provider/密钥托管未验 |
| 群白名单/ActionPolicy | GET/PUT policy 200，白名单字段往返 | 已接线 | **已实测（fake Runtime）**；真实群生效未验 |
| 日志/审计 | `/logs` `/audit` 200 双 owner 合并、过滤/分页/400/401/503 | P12c-6 字段已接 | **已实测（fake owners；P12e 真实 PG 数据层）**；目标栈 live 未跑 |
| 生命周期删除/恢复 | delete/restore 202+operation；BLOCKED 200 快照 | P12d mock | **已实测（fake Runtime）**；Host 参与者真实停止未验 |
| 健康四依赖 | 200 四组件 | 已接线 | **已实测（fake 依赖就绪）**；真实故障注入未跑 |

## 3. 缺陷与修复

1. **真实缺陷（已修）**：Control owner gRPC 错误映射缺少 `ABORTED/NOT_FOUND/RESOURCE_EXHAUSTED/UNIMPLEMENTED/INTERNAL`，导致 owner 的 CONFLICT 被降级为 `503 UPSTREAM_UNAVAILABLE`（真实 Control+fake Runtime 矩阵暴露）。修复 `control/api/src/target/owner_clients.cpp::extract_error_code`，补 `CONFLICT/NOT_FOUND/RATE_LIMITED/CAPABILITY_NOT_SUPPORTED/INTERNAL`；矩阵断言冲突 409、不可用 503 回归通过。
2. 无其他产品代码缺陷；fixture 侧修正了 protobuf repeated setter 误用（验收工具，不属产品）。

## 4. 契约缺口（不在本任务自造）

- `listActionMetadata`（Action 元数据列表）仍 `x-contract-status: not_implemented`，无 Control owner 映射 → Q8“Action 元数据”只覆盖单条 action 查询；需 P12a/P12c 后续声明。
- operation `host` 未在 OpenAPI 枚举/owner RPC 声明 → Control 返回 400，不伪造 host operation。
- 旧 `/plugins/{id}/enable|disable|config` 不复用（避免把插件命令映射到单实例）；需要实例插件管理走 P12c-5 路径。
- WS 目标频道（logs/health.updated 等）延期；P12a-2 已确认 REST operation 快照为 resync 源。
- 模型 Disable/Enable 在 main `3ac3761` **已有 REST 且本矩阵 200**（P11d 已接线）；任务描述中的“无 REST”缺口已不成立。
- 未在 Control 层实现限流（429 由 owner/网关基础设施提供）；Panel 真实浏览器 live 尚未跑。

## 5. 未验收项与需用户环境

- 目标六服务真实栈（Identity/Runtime/Scheduler/Model/Host + PG16.4）通过 Control 的 live 验收；本矩阵 owner 均为 fake。
- 单主机 Docker Compose/真实部署 topology；QQ/NapCat、真实模型 Provider、Host 参与者真实停止/插件 ACK。
- Panel 浏览器真实 HTTP（非 mock transport）与断流/重连 live。
- 真实故障注入（owner 重启/网络分区/超时）与真实 PG 数据可见性。
- 需用户环境/确认：目标栈部署与账号/凭据；是否补 `listActionMetadata` 契约；WS 频道排期。
