# 用户决策注册（2026-11-03）

- 目的：把用户 2026-11-03 对 P13/P05b/P12/P11/P14 的决策原样登记为团队可追溯输入；**不表示实现/验收完成**。
- 来源：用户对 ask 决策问题的回答（Lead 转述）；对应任务：task-140/141/143/144/145/146/147。
- 相关：[P13f 退出决策](../modules/architecture-p13f-exit-decision.md)、[P05b 实例委托提案](p05b-instance-delegation-proposal.md)、[交付证据矩阵](../modules/architecture-delivery-status.md)、[doc/27](../../doc/27-phase-exit-evidence.md)、[doc/23](../../doc/23-implementation-plan.md)。

## 1. P13：批准限定方案 A（仅 ListTasks/CancelTask）

**批准内容（用户原话要点）**：批准 P13 方案 A，但限定为 Scheduler 通过 mTLS 验证 Runtime 平台身份 + 平台 peer 白名单/registration + 显式授权；**仅开放 `ListTasks`、`CancelTask` 两个方法**。

**约束（必须遵守）**：
- `ListTasks` 只返回目标实例（instance scope）的任务；不得泄露其他实例/其他安装任务。
- `CancelTask` 前核对任务归属（task owner = 该 instance/account）；跨实例、跨安装拒绝；不泄露存在性。
- 未注册/白名单外/无显式授权一律 fail-closed；旧 caller（Control/SchedulerRuntimeClient）行为不变。
- 不新增 proto/RPC/权限编号；仅方法授权语义（target_spec caller/authz 注记 + doc/01）。

**生产验收门禁（用户同意）**：
- 实例 BeginDelete 必须等真实 Host 回执 + Scheduler 任务清理 Done 后才进入 DELETED。
- Host 离线、Scheduler cancel UNKNOWN 保持 BLOCKED，不伪完成。
- Restore 不重建任务/租约、不复活旧凭据。

**对应任务（当前状态）**：
| task | 内容 | 状态 |
|---|---|---|
| task-140 | P13h-1 Scheduler 受限平台 peer 授权（仅 ListTasks/CancelTask） | 已派发，pending |
| task-141 | P13h-2 Runtime lifecycle Scheduler participant 生产接线 | 已派发，in_progress |
| task-143 | P13h-3 P13 生产联合验收（真实 Host 回执+Scheduler cleanup→DELETED） | pending，blocked_by 140/141 |

**未批准**：方案 B（Control 签发实例委托）；扩权到其他 Scheduler 方法。P13 生产退出**未完成**，现有 `p13_joint_delete_e2e` 仍只是 test-only 证据。

## 2. P05b：批准 B1（Control 管理员触发实例主体父委托）

**批准内容**：采用 B1——Control 管理员会话 + 显式“为实例签发”授权，调用扩展后的 Identity IssueDelegation，为实例 principal 签发短父委托；Runtime 经 ExchangeDelegation 获取 audience 下游委托。

**约束**：
- 不任意参数化 subject；仅已注册实例 principal + RESOURCE_INSTANCE binding；Control 管理员授权校验。
- TTL ≤56s、证书绑定、在线撤销检查、scope/permission 白名单、审计；失败 fail-closed，无 secret 日志。
- 不改变 Model/Scheduler owner 的最终验证语义。

**P11 验收范围（用户批准）**：真实 **managed plugin**（经 Host/P06g/P08 harness，非测试直接拉起）+ 真实 **HTTP Provider fixture**（本地真实 HTTP server，非 fake port），覆盖成功/超时/4xx/429/5xx/stream=false/usage；负例：父委托撤销、过期、跨实例、错误证书、Runtime/Identity 不可达；不伪跑。**真实付费外部 Provider（T07b）仍属环境项，不在本次 fixture 批准范围**。

**对应任务（当前状态）**：
| task | 内容 | 状态 |
|---|---|---|
| task-144 | P05b-2 Identity 实例主体父委托签发（B1） | 已派发，in_progress |
| task-145 | P05b-4 Runtime 实例委托 provider（Model）+ P11 单链路 e2e | 已派发，pending，blocked_by 144 |
| task-146 | P11h 真实 managed plugin+真实 HTTP Provider 单链路验收 | 已派发，pending，blocked_by 145 |
| task-147 | P05b-5 P10k 真实 Scheduler 委托解锁 | 已派发，pending，blocked_by 144 |

**未批准**：B2（实例凭据/自行 Exchange）；任意实例无授权父委托；以 fake port 替代真实 HTTP Provider fixture。

## 3. P12：host operation / bootstrap_credential 保持 not_implemented

**用户决定**：P12 的 host operation 与 bootstrap_credential 字段级缺口**保持 `not_implemented`**，仅文档化，不扩展契约；不以未验收项冒充完成。

**影响**：
- `semantic.host.unknown_operation_error` 当前仍为契约 not_implemented/实现 INVALID_ARGUMENT 的差异保留；
- `bootstrap_credential` 字段级 target/消费闭环保持未翻；
- P12 其余范围（P12a/l/j、Control REST/Panel、健康/日志审计等）按既有证据，不因本决定改变状态。

**对应任务**：无新增实现任务；文档化并入 P00q 决策注册与 doc/01/23 状态说明。

## 4. P14：环境项由用户准备中

**用户状态**：将准备 Docker 与 GitHub CLI (`gh`) 登录，之后继续 Compose/CI/release 验收。

**当前未验收（不因用户准备动作而改变）**：
- 单机 Docker Compose（官方 PG16.4 镜像、compose config/up、healthcheck、备份恢复）；
- 远端 GitHub Actions（手动 workflow_dispatch 首跑、`CI_AUTO_ENABLED`）；
- 正式发布（release SHA 全量归档、GHCR digest、SBOM/cosign、release-gated Environment）；
- 两主机/跨主机演练（延期）。

真实 QQ/NapCat、真实外部 Model Provider（付费）、生产 CA/密钥托管、备份离站仍为环境/用户决策项。

## 5. 统一状态口径

| 项 | 决策状态 | 实施状态 | 是否可宣称完成 |
|---|---|---|---|
| P13 A（限定 ListTasks/CancelTask） | 已批准 | P13h-1/2/3 派发/实施中 | **否**（生产联合验收未完成） |
| P05b B1 | 已批准 | P05b-2 in_progress；P05b-4/5、P11h 待依赖 | **否**（链未验收） |
| P12 host operation/bootstrap_credential | 用户确认保持 not_implemented | 文档化、不扩契约 | **否/按 not_implemented** |
| P11 真实 Provider 验收范围 | 已批准（真实 managed plugin + 真实 HTTP Provider fixture） | P11h 待 P05b-4 | **否**（真实付费 Provider 仍环境项） |
| P14 环境 | 用户准备 Docker/gh 中 | Compose/CI/release 未跑 | **否** |
| 两主机/跨主机 | 延期（用户已确认可选） | 未做 | **否** |

**原则**：本文件登记“用户已批准/已派发”，不等于 task 完成或阶段退出；只有对应 task 的代码/测试/验收证据合入并在最终门禁复跑后，才能更新 doc/27 的“已实测/退出通过”状态。
