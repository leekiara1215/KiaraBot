# P07 Action 模块报告（target 全链路验收与登记收口）

- 状态：P07 target Action 应用/桥协议/Control 查询 handler 已实现，真实 PG16.4 + TLS +
  Runtime 进程 + 手工 wire 客户端验收通过（2026-11-02，P07a-C）。
- 代码边界：契约 `proto/kiarabot/v1/{actions,adapters,plugins,runtime,policy}.proto`；领域
  `core/include/kiarabot/core/domain/target/action_rules.hpp` 与 `identity_permissions.hpp`；
  应用 `core/src/application/target/runtime_action_application.cpp`（P07a-B）；PG 六表/typed
  仓储 `infrastructure/postgres/{schema,src}/runtime_actions.hpp`、`src/action_records.*`
  （P07a-A/C3）；Bridge 帧 `core/src/runtime/platform/runtime_bridge.cpp`；RPC
  `core/src/runtime/platform/runtime_rpc.cpp`；组合根 `core/app/runtime_composition.cpp`；
  测试 `tests/architecture/p06_bridge_acceptance.py` 的 P07 片段。
- 不新增服务/进程；Action 仍由 Runtime 在 Runtime 私有 PG 库内持久化，外部插件只经
  PluginBridge 流，适配器只经 AdapterBridge 流。

## 输入/输出与状态机

- 输入：插件 `PluginToCore.action/action_query/action_cancel`（owner=认证连接实例，manifest
  权限+`action.result.v1` feature）；适配器 `AdapterToCore.action_result/action_evidence`
  （必须属于该 adapter instance 且回带 dispatch_attempt_id）。
- 输出：`PluginActionResponse`（ActionResult 或稳定 Error）；`CoreToAdapter.action`（Runtime
  填充 dispatch_attempt_id/dispatch_sequence）；`CoreToAdapter.action_receipt`
  （RECORDED/DUPLICATE/EVIDENCE_ONLY/CONFLICT）；`RuntimeService.QueryActionResult`
  （Control caller、`action.inspect`、显式单账号 scope）。
- 状态：ACCEPTED/NOT_STARTED → dispatched/UNKNOWN → SUCCEEDED/KNOWN_SUCCEEDED、
  FAILED/KNOWN_FAILED、EXPIRED/NOT_STARTED|UNKNOWN、CANCELLED/NOT_STARTED；EXPIRED 后的迟到
  terminal 结果只追加 `adapter_result_evidence`，不改终态；重复相同 terminal 快照
  DUPLICATE，异结果 CONFLICT+证据；非终态证据仅追加。
- 受理：身份/kind/权限/feature/账号绑定/适配器 active/每-kind policy/持久窗口配额/deadline
  300s 上限在短 UoW 内校验；Action 行先插入、配额后准入，拒绝回滚整个 UoW；幂等键重放同
  fingerprint 返回原快照，异内容 CONFLICT；commit Unknown 只按原 identity 新 UoW 复核。
- 派发：`list_due` 在 UoW 内 `FOR UPDATE SKIP LOCKED`，逐行条件 mark_dispatched（attempt、
  sequence）后提交，再写 Adapter 流；写失败只会使该 Action 保持 UNKNOWN/EXPIRED，不二次派发。
- 取消：只有 Accepted+NotDispatched 可取消；已 dispatched 返回 CONFLICT；expired/cancelled
  幂等返回原状态。

## 验收证据

- `ctest -R p06_bridge_acceptance`（标签 p06;p07）真实 PG16.4/TLS/Runtime：
  注册/授权/refresh 后，P07 片段覆盖：
  * 插件 submit → ACCEPTED/NOT_STARTED；Runtime 派发 attempt/sequence → adapter；
  * adapter terminal → RECORDED（SUCCEEDED+platform_message_id），重发 → DUPLICATE；
  * plugin owner query 观察终态；evidence UNKNOWN → EVIDENCE_ONLY 且保持 ACCEPTED/UNKNOWN；
  * evidence/result 同 attempt 竞争按 wire 顺序产生 EVIDENCE_ONLY→RECORDED；
  * 已 dispatched 取消 → CONFLICT；
  * 相同 action_id/idempotency 重复提交返回原快照且不产生第二次派发；
  * 同账号/群持久配额窗口：3 个群 Action 受理、第 4 个 RATE_LIMITED，派发 id 与受理一致；
  * adapter 断线重连后回带旧 dispatch attempt 的 terminal 结果 → RECORDED，可查询；
  * deadline 到期后 EXPIRED/UNKNOWN，迟到 SUCCEEDED → EVIDENCE_ONLY，终态不重写。
- 领域单测 `action_rules_test`（p07_action_rules）、PG 矩阵
  `p07_postgres_action_records`、codec/权限映射 p04_target_codec 保持通过。
- 契约门禁：check_implementation_status / check_semantic_rules /
  check_enum_consistency / evidence_scopes_test / target_api_test.rb /
  check_contracts.sh 全通过；target_spec 的 RuntimeService.QueryActionResult 标记
  implemented，internal_dispatcher 的 Submit/Query/Cancel 仍未对外暴露。
- 未做：ActionPolicy Get/Update 的 RPC 接线与 wire 测试（应用层已实现，RPC 方法仍
  not_implemented）；Mute/Kick policy gate 的 wire 级验收（领域规则有测试）；
  Control REST 转发（P12）；真实 P07c Adapter 进程（本任务用 wire 级 fake adapter，
  P07c 客户端对 fake target 已有单测，真实客户端联调归 P07c/P07 集成）。

## 检查命令

```bash
cmake --build --preset full-locked --parallel 3
ctest --preset full-locked -R 'p07|p06|p03_postgres|p05' --output-on-failure
ctest --preset full-locked -R p06_bridge_acceptance --output-on-failure
python3 proto/tests/check_implementation_status.py
python3 proto/tests/evidence_scopes_test.py
bash proto/tests/check_contracts.sh
```

## P07g 退出证据与 target 状态核对（2026-11-02，HEAD 95495c2）

工作树 `.worktrees/p07g`，分支 `codex/p07g`，基于 main@95495c2；本阶段只做证据/文档，不改实现/契约。

### 实际运行（命令+结果）
```text
cmake --build --preset full-locked --parallel 8   # build=0
ctest --preset full-locked -R \
  'p07_action_rules|p07_postgres_action_records|p07_adapter_e2e|p06_bridge_acceptance|p12_control_owner_gateway'
# 5/5 passed, 49.27s
# p07_action_rules 0.45s; p07_postgres_action_records 3.79s (真 PG16.4)
# p06_bridge_acceptance 24.48s (真 TLS/PG/Runtime，P07 帧段)
# p07_adapter_e2e 18.91s (真实 NoneBot adapter + fake OneBot + 真实 Runtime)
# p12_control_owner_gateway 1.63s
PYTHONPATH=src:build/generated .venv/bin/python -m pytest -q tests/test_actions.py
# 28 passed, 0.66s
```
- `p06_bridge_acceptance` P07 段：插件 submit→ACCEPTED/NOT_STARTED、Runtime 派发 attempt/sequence、adapter terminal→RECORDED、重发→DUPLICATE、evidence UNKNOWN→EVIDENCE_ONLY、result/evidence 同 attempt 竞争顺序、已派发取消→CONFLICT、同 action/idempotency 重复提交回放不二次派发、3 群/分钟配额第 4 个 RATE_LIMITED、adapter 断线重连旧 attempt terminal→RECORDED、deadline EXPIRED+迟到 SUCCEEDED→EVIDENCE_ONLY 不改终态。
- `p07_adapter_e2e`：真实进程链路 send_message 成功、平台 retcode→KNOWN_FAILED、重复平台帧不新增 Runtime event/Action、dispatch 后取消 CONFLICT、平台 WS 断线重连后继续且无重复平台调用。
- `p07_postgres_action_records`：真 PG16.4 Action 行/evidence/receipt/配额窗口/过期/重启持久。
- `action_rules_test`：kind/权限/policy/deadline/状态×执行已知性/取消/EXPIRED 纯规则。
- SDK `tests/test_actions.py`：Action preflight/status×outcome 解释/Query 回放/UNKNOWN 不误判失败。

### target/契约核对
- 已 implemented 且有上述证据：`ActionEnvelope`、`ActionResult`、`ActionExecutionEvidence`、`ActionReceipt`、`ActionMetadata`、`PluginActionRequest/Query/Cancel/Response`、`RuntimeQueryActionResult*`；`ActionResult.status/execution_outcome`、evidence/receipt 全字段、dispatch attempt/sequence、send_message payload、权限枚举 41–46、5 条 action semantic rule；target_spec `QueryActionResult=implemented`（wire + Control caller）。
- P07f-2 已翻转（2026-11-02）：`RuntimeService.GetActionPolicy/UpdateActionPolicy`、ActionPolicy message/fields、`ActionEnvelope.react_message/delete_message/mute_member/kick_member/approve_request` payload 及对应 message/fields 均登记 target implemented；证据为 P07f-1 real TLS/PG bridge policy gate、dispatch→result→receipt、target source 与 tests/architecture/p06_bridge_acceptance.py/P07 测试。target_spec 两个 owner 方法改 implemented。
- 未翻转项及理由：
  - plugin 权限过滤（manifest 权限 × kind × policy）作为独立 target 语义未单列；现有 41–46 枚举已翻，实际 kind/policy gate 由 application 覆盖。
  - `ListActionMetadata`/`InspectActionMetadata`：target_spec 仍 not_implemented（Control 查询走 QueryActionResult；list 未接线）。
  - Control REST 的 ActionPolicy OpenAPI `x-contract-status` 按 owner-RPC 规则保持 not_implemented；REST handler 已由 P12c-3 接线。
- 契约门禁：`check_implementation_status.py` 通过；`target_api_test.rb`/`evidence_scopes_test.py` 在 main@95495c2 仍失败（listAuditEntries/listLogs/beginDeleteResource/restoreResource 带 owner-RPC 却标 implemented + 冻结集缺这 4 项），属 P12c-7/P12e/P13c 登记漂移，Lead 已安排 core-target 在 P12c-8 修，P07g 未扩改。

### 剩余缺口
| 缺口 | 状态 |
| --- | --- |
| 真实 QQ/NapCat 平台（登录、真实撤回/禁言/踢出/审批凭证、副作用） | 待用户环境（M2/T07b） |
| 平台延迟/EXPIRED 迟到证据故障注入 | 部分实测：bridge fake adapter 的 deadline+迟到 SUCCEEDED 已覆盖；真实 adapter 进程下延迟/迟到矩阵未注入（待补） |
| UNKNOWN 重连补报窗口（adapter 崩溃/断线后 action journal 补报） | 部分实测：bridge acceptance 覆盖 adapter 重连旧 attempt；P07d 覆盖平台断线重连，但进程崩溃恢复窗口未专项 |
| Host/Adapter 跨主机 | 待环境 |
| 真实平台 reaction API（NapCat set_msg_emoji_like） | 待环境/未验收（T07b） |
| Control listActionMetadata REST | 未接线；现有 QueryActionResult REST 已测 |

## P07 退出判定（P07f-2，2026-11-02）

**通过**。Action target 契约（payload 五类、ActionPolicy owner RPC/message/fields）、真实
TLS/PG16.4 Runtime wire 证据、dispatch→result→receipt/evidence、Mute/Kick policy 允许/拒绝矩阵、
旧策略/预算拒绝无副作用、target_spec/执行范围/证据登记均通过门禁。非阻塞残余：真实平台
reaction API（NapCat set_msg_emoji_like）与 Control `listActionMetadata` REST 未接线，属环境/
后续能力，不影响 P07 target Action 契约退出。
