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
