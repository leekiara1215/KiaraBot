# P13c 跨服务生命周期 participant + target 登记（实现记录）

日期：2026-11-02；worktree `.worktrees/p13c`，分支 `codex/p13c`，基于 main@29c01a7。

## 实现
1. `RuntimeHostLifecycleParticipant`（新 header/source，接入 `kiarabot_runtime_resource_application`）：从 Runtime UoW 读取实例/安装，按确定性 command identity 向 `host_command` outbox 写 StopExecution 命令；receipt 已知终态→Done；Pending/Dispatched/Unknown→BLOCKED；persisted=false→BLOCKED(Conflict)；同 command_id 幂等。Runtime 组合根创建并注入真实端口（此前为 null）。
2. 应用测试：fake host_command outbox 验证 enqueue 快照、blocked→known success Done、UNKNOWN/拒绝不完成。
3. 目标登记：target_spec Runtime BeginDelete/RestoreResource/GetOperation = implemented；contract_status target override 覆盖对应 request/response、Operation/OperationRef/OperationProgress 消息字段、OperationState 枚举与 `operations.getOperation`（REST 操作 marker 按 target_api 规则保持 not_implemented）；doc/01 target 表补 45 个键。
4. 文档 doc/01/02/08/22 + 本报告。

## 验证
- full-locked 全量构建通过；`p13_runtime_lifecycle`/`p05_runtime_resource_application` 通过；target_api/check_implementation/evidence_scopes 通过（getOperation 仅 target override，REST marker 未翻转）。
- 需要 P08b-3b 的 HostBridge 命令下发/回执与 P10b-2 的 Runtime→Scheduler 实例委托配置完成真实 Host/Scheduler 收敛；P13b Host drain 语义已就绪。
- Control operation participants/blocked/reason 字段由 P12c owner gateway 投影，P12c-4 测试覆盖快照；本任务未重复实现 REST。

## 未完成 / 集成点
- P08b-3 HostBridge 命令下发/回执 pipeline（host_bridge_server callbacks 目前可为空）：接通后 Host participant 从 BLOCKED 自动收敛；UNKNOWN 仍不伪 stopped。
- Runtime→Scheduler delegation/endpoint：Scheduler participant 真实 ListTasks/CancelTask 未接线，fake 端口已验证离线/恢复语义。
- Control REST beginDelete/restore 未实现（P12c 责任）；P13 target 只登记 owner RPC 与 operation 投影。
- 跨服务 shutdown 编排的部署级顺序（P14 compose/systemd）与真实平台故障演练未执行。
