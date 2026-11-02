# P13a Runtime 生命周期协调、清理与可观测性（实现报告）

日期：2026-11-02；worktree `.worktrees/p13a`，分支 `codex/p13a`；已 rebase 到 main@8f3bfd3（含 P08b-2c `host_registration()`/host_connections、P12c-3、P10b-2；冲突原则：双方 UoW 访问器/service_config/scope_kinds 全保留，未手工丢弃任一侧）。
实现范围：P13a（Runtime BeginDelete/RestoreResource/GetOperation 参与者进度 + 60s 保留清理 + schema v13），零新增 proto 消息/字段/RPC/权限。

## 交付内容

1. **契约元数据**（commit 1）：`RuntimeService.BeginDelete/RestoreResource/GetOperation` 补 `scope_kinds`（ACCOUNT/ADAPTER/INSTALLATION/INSTANCE/HOST）。原因：冻结方法缺该 option 时 Identity method-policy 解析失败，真实在线授权返回 INVALID_ARGUMENT，RPC 不可用；不新增任何字段/RPC/权限。
2. **持久化/清理原语**（commit 2）：domain `OperationParticipant`/`LifecycleTombstone`、Operation 保留字段；UoW fail-closed 默认方法 + PG 实现（participants 替换、tombstone CRUD/扫描、outbox/audit/operations prune）；schema v13、四个逻辑库真实 PG16.4 descriptor/relation_count 实测；operations/audit/outbox 清理索引。
3. **应用/RPC/组合根**（commit 3）：`begin_delete`/`restore_resource`/`lifecycle_maintenance`；BeginDelete/Restore RPC 接线；GetOperation 参与者投影；Runtime 1s 维护钩子接入生命周期推进 + 每 60 轮保留清理；Host/Scheduler 参与者端口在未接线时 BLOCKED（fail-closed），注入 fake 端口单测。
4. **测试**（commit 3/4）：应用测试覆盖 runtime-only 完成/重复删除冲突/恢复窗口与重复恢复、实例参与者 BLOCKED→依赖恢复完成、30 天外恢复拒绝、180 天 PURGED、60 轮清理批次；真实 PG16.4 测试覆盖 operation 保留/participants 替换/过期 operation 清理/audit 保留/outbox 未决不清理/tombstone CRUD；真实双进程 Runtime↔Identity TLS e2e 覆盖 BeginDelete→GetOperation(participants)→Restore。
5. **文档**（commit 5）：doc/02、doc/05、doc/22、本报告。

## Rebase 后验证（main@8f3bfd3）

- Runtime schema 保持 **v13**：合并 P08b-2c 后真 PG16.4 实测，host_connections 已在 v12/P08b-2 计入，P08b-2c 未新增 relation，无需 v13→v14；不猜数字。
- 实测 manifest（rebase 后 p03 真实 PG 校验通过）：Identity 135/`3caa2e6995fdd42d88f45d01f34017ac`、Runtime 145/`8a4aba6dc6cbc2b555a1fd8053a38cee`、Scheduler 83/`4ec3a4dd616f66e5529d2edc8e67f5f4`、Model 77/`ccee8cca142d137ac2726c48ce8ab8a7`。
- full-locked 全量构建通过；`ctest -R 'p05|p06|p07|p08|p10|p03_postgres'` 通过；`p03_postgres_database`/`p03_service_database`、`p01_target_roundtrip` 通过；clang-format 16 dry-run 与 `git diff --check` 通过。

## 验证

- `cmake --build --preset full-locked --parallel 8`：通过。
- `ctest --preset full-locked -R 'p05_runtime|p03_postgres_database|p03_service_database'`：通过（p03 两个真实 PG 测试含新 schema/participants/tombstone；p05 应用与真实 Identity e2e 通过）。
- `ctest --preset full-locked -R 'p10|p06|p07|p08'`：待最终全量复跑（见下）。
- `ruby proto/tests/target_api_test.rb`：50 operations 通过；`python3 proto/tests/check_implementation_status.py`：一致。
- `p01_target_api` 未运行：本机缺 `scripts/ci` npm 安装（环境限制，非本次变更）；proto target_spec 由 ruby/implementation 检查覆盖。
- format/diff：提交前运行 clang-format 16 + `git diff --check`（最终验证补记）。

## 未完成 / 交接

- **P08b-3 Host 命令 outbox / HostBridge server** 未实现：Host 参与者端口未接线，真实删除实例会保持 BLOCKED（诚实语义）；需在 P08b-3 合入后实现 `HostLifecycleParticipantPort`（stop/drain + observation）。
- **P10b Scheduler→Runtime 真实交接链路** 仍受 delegation 配置阻塞：Scheduler 参与者端口未接线；需按 P10b 契约用 Runtime instance delegation 调 `ListTasks/CancelTask`，实现 `SchedulerLifecycleParticipantPort`。
- **P12c Control REST/Panel**：BeginDelete/Restore/GetOperation 的 REST 映射与面板进度展示未接；P13a 只保证 Runtime 服务端语义。
- 180 天 PURGED 目前只做状态/墓碑清理，未做内容物理最小化（账号显示名/配置值/命名空间等）；属 P13 后续清理阶段。
- 契约状态登记（target_spec/contract_status/OpenAPI status）未翻转：需 P13 完整验收后统一登记；本次 proto 只补授权元数据。
- 未跑真实 QQ/真实平台；未跑 p01_target_api（缺 npm ci 环境）。Host/Scheduler 真实参与者需 P08b-3/P10b 合入后补多进程验收。
